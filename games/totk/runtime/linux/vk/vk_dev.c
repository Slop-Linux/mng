/* Device, memory, guest memory pools, submissions and present. One graphics queue and one timeline semaphore: a
   submission signals its fence value; a completion thread applies the submission's CPU-visible effects (event writes,
   counter records) in submit order after the GPU finished it, then frees what the submission used. mtl_wait waits for
   that, not for the semaphore alone, so a game wait sees the effects (MNG's rule for nvn_mtl.m). */
#include "vk.h"
#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <mach/mach_time.h>
#include <unistd.h>

vk_ctx V;
static int g_inited;

uint32_t vk_memtype(uint32_t bits, VkMemoryPropertyFlags want) {
    for (uint32_t i = 0; i < V.mem.memoryTypeCount; i++)
        if ((bits >> i & 1) && (V.mem.memoryTypes[i].propertyFlags & want) == want) return i;
    return UINT32_MAX;
}

/* ---------- window (created by the frontend on the main thread) ---------- */
SDL_Window *vk_sdl_window;   /* runtime/linux/frontend.c */
static VkSurfaceKHR g_surface; static VkSwapchainKHR g_swap; static VkImage g_swap_img[8]; static uint32_t g_nswap, g_sw, g_sh; static VkFormat g_swap_fmt;
static VkSemaphore g_acq[8], g_rel[8]; static unsigned g_acq_i; static VkCommandBuffer g_pcb[8]; static VkFence g_pfence[8];

static void *dev_proc(const char *n) { void *p = (void *)vkGetDeviceProcAddr(V.dev, n); if (!p) VKDIE("device function %s missing", n); return p; }

void vk_init(void) {
    if (g_inited) return;
    g_inited = 1;
    const char *iext[16]; uint32_t niext = 0;
    if (vk_sdl_window) { Uint32 n = 0; const char *const *e = SDL_Vulkan_GetInstanceExtensions(&n); for (Uint32 i = 0; i < n && niext < 14; i++) iext[niext++] = e[i]; }
    iext[niext++] = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
    VkApplicationInfo app = {.sType = VK_STRUCTURE_TYPE_APPLICATION_INFO, .pApplicationName = "botw-native", .apiVersion = VK_API_VERSION_1_3};
    VkInstanceCreateInfo ici = {.sType = VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, .pApplicationInfo = &app, .enabledExtensionCount = niext, .ppEnabledExtensionNames = iext};
    VKCHECK(vkCreateInstance(&ici, NULL, &V.inst));
    uint32_t n = 1; VkPhysicalDevice pd[4]; n = 4; VKCHECK(vkEnumeratePhysicalDevices(V.inst, &n, pd)); if (!n) VKDIE("no Vulkan device");
    V.phys = pd[0]; vkGetPhysicalDeviceProperties(V.phys, &V.props); vkGetPhysicalDeviceMemoryProperties(V.phys, &V.mem);
    VKLOG("device %s, API %u.%u.%u", V.props.deviceName, VK_API_VERSION_MAJOR(V.props.apiVersion), VK_API_VERSION_MINOR(V.props.apiVersion), VK_API_VERSION_PATCH(V.props.apiVersion));
    VkPhysicalDeviceExternalMemoryHostPropertiesEXT hostp = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_MEMORY_HOST_PROPERTIES_EXT};
    VkPhysicalDeviceProperties2 p2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, .pNext = &hostp};
    vkGetPhysicalDeviceProperties2(V.phys, &p2);
    V.host_align = hostp.minImportedHostPointerAlignment ? hostp.minImportedHostPointerAlignment : 16384;
    if (vk_sdl_window && !SDL_Vulkan_CreateSurface(vk_sdl_window, V.inst, NULL, &g_surface)) VKDIE("SDL_Vulkan_CreateSurface: %s", SDL_GetError());
    uint32_t nq = 8; VkQueueFamilyProperties qf[8]; vkGetPhysicalDeviceQueueFamilyProperties(V.phys, &nq, qf);
    V.qfam = UINT32_MAX;
    for (uint32_t i = 0; i < nq; i++) if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) { VkBool32 ok = 1; if (g_surface) vkGetPhysicalDeviceSurfaceSupportKHR(V.phys, i, g_surface, &ok); if (ok) { V.qfam = i; break; } }
    if (V.qfam == UINT32_MAX) VKDIE("no graphics queue");
    float prio = 1;
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = V.qfam, .queueCount = 1, .pQueuePriorities = &prio};
    const char *dext[] = {VK_EXT_SHADER_OBJECT_EXTENSION_NAME, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
        VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    uint32_t ndext = sizeof dext / sizeof *dext - (g_surface ? 0 : 1);
    /* every feature the device has in these structures (the queried chain is the enabled one), minus robust buffer / image
       access (a cost the game does not need; null descriptors stay) */
    VkPhysicalDeviceProvokingVertexFeaturesEXT pv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT};
    VkPhysicalDeviceDepthClipEnableFeaturesEXT dce = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_CLIP_ENABLE_FEATURES_EXT, .pNext = &pv};
    VkPhysicalDeviceIndexTypeUint8FeaturesEXT iu8 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_INDEX_TYPE_UINT8_FEATURES_EXT, .pNext = &dce};
    VkPhysicalDeviceCustomBorderColorFeaturesEXT cbc = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT, .pNext = &iu8};
    VkPhysicalDeviceRobustness2FeaturesEXT rb2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ROBUSTNESS_2_FEATURES_EXT, .pNext = &cbc};
    VkPhysicalDeviceVertexInputDynamicStateFeaturesEXT vid = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VERTEX_INPUT_DYNAMIC_STATE_FEATURES_EXT, .pNext = &rb2};
    VkPhysicalDeviceExtendedDynamicState3FeaturesEXT ed3 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTENDED_DYNAMIC_STATE_3_FEATURES_EXT, .pNext = &vid};
    VkPhysicalDeviceShaderObjectFeaturesEXT so = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_SHADER_OBJECT_FEATURES_EXT, .pNext = &ed3};
    VkPhysicalDeviceVulkan13Features f13 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_3_FEATURES, .pNext = &so};
    VkPhysicalDeviceVulkan12Features f12 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_2_FEATURES, .pNext = &f13};
    VkPhysicalDeviceVulkan11Features f11 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VULKAN_1_1_FEATURES, .pNext = &f12};
    VkPhysicalDeviceFeatures2 f2 = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, .pNext = &f11};
    vkGetPhysicalDeviceFeatures2(V.phys, &f2);
    f2.features.robustBufferAccess = 0; rb2.robustBufferAccess2 = 0; rb2.robustImageAccess2 = 0; f13.robustImageAccess = 0;
    if (!so.shaderObject || !rb2.nullDescriptor || !vid.vertexInputDynamicState || !f13.dynamicRendering || !f12.timelineSemaphore) VKDIE("the device lacks shader objects, null descriptors, dynamic vertex input, dynamic rendering or timeline semaphores");
    VkDeviceCreateInfo dci = {.sType = VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, .pNext = &f2, .queueCreateInfoCount = 1, .pQueueCreateInfos = &qci, .enabledExtensionCount = ndext, .ppEnabledExtensionNames = dext};
    VKCHECK(vkCreateDevice(V.phys, &dci, NULL, &V.dev));
    vkGetDeviceQueue(V.dev, V.qfam, 0, &V.queue);
#define P(n) V.n = (PFN_vk##n)dev_proc("vk" #n)
    P(CmdSetVertexInputEXT); P(CreateShadersEXT); P(DestroyShaderEXT); P(CmdBindShadersEXT); P(CmdSetColorBlendEnableEXT); P(CmdSetColorBlendEquationEXT);
    P(CmdSetColorWriteMaskEXT); P(CmdSetPolygonModeEXT); P(CmdSetRasterizationSamplesEXT); P(CmdSetSampleMaskEXT); P(CmdSetAlphaToCoverageEnableEXT);
    P(CmdSetDepthClampEnableEXT); P(CmdSetLogicOpEnableEXT); P(CmdSetLogicOpEXT); P(CmdPushDescriptorSetKHR); P(GetMemoryHostPointerPropertiesEXT);
    P(CmdSetTessellationDomainOriginEXT); P(CmdSetDepthClipEnableEXT); P(CmdSetProvokingVertexModeEXT);
#undef P
    VkSemaphoreTypeCreateInfo st = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO, .semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE};
    VkSemaphoreCreateInfo sci = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, .pNext = &st};
    VKCHECK(vkCreateSemaphore(V.dev, &sci, NULL, &V.timeline));
    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = V.qfam};
    VKCHECK(vkCreateCommandPool(V.dev, &cpi, NULL, &V.cmdpool));
    VkDescriptorSetLayoutCreateInfo el = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    VKCHECK(vkCreateDescriptorSetLayout(V.dev, &el, NULL, &V.empty_layout));
    VKLOG("host-pointer import alignment %#llx", (unsigned long long)V.host_align);
    if (g_surface) vk_window_open();
}

/* ---------- image memory ---------- */
#define BLOCK (64ull << 20)
typedef struct { VkDeviceMemory mem; uint32_t type; VkDeviceSize size; VkDeviceSize *free_off, *free_len; int nfree, capfree; } block_t;
static block_t *g_blocks; static int g_nblocks; static pthread_mutex_t g_mem_mu = PTHREAD_MUTEX_INITIALIZER;
static int block_take(block_t *b, VkDeviceSize size, VkDeviceSize align, VkDeviceSize *off) {
    for (int i = 0; i < b->nfree; i++) {
        VkDeviceSize o = (b->free_off[i] + align - 1) & ~(align - 1), end = b->free_off[i] + b->free_len[i];
        if (o + size > end) continue;
        VkDeviceSize lo = b->free_off[i];
        if (o + size < end) { b->free_off[i] = o + size; b->free_len[i] = end - (o + size); } else { b->free_off[i] = b->free_off[--b->nfree]; b->free_len[i] = b->free_len[b->nfree]; }
        if (o > lo) {   /* the alignment gap stays free */
            if (b->nfree == b->capfree) { b->capfree = b->capfree ? b->capfree * 2 : 16; b->free_off = realloc(b->free_off, b->capfree * 8); b->free_len = realloc(b->free_len, b->capfree * 8); }
            b->free_off[b->nfree] = lo; b->free_len[b->nfree++] = o - lo;
        }
        *off = o; return 1;
    }
    return 0;
}
int vk_alloc_mem(VkMemoryRequirements req, vk_alloc *out) {
    uint32_t type = vk_memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (type == UINT32_MAX) return 0;
    pthread_mutex_lock(&g_mem_mu);
    if (req.size > BLOCK / 2) {   /* big images get their own allocation */
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = type};
        VkResult r = vkAllocateMemory(V.dev, &ai, NULL, &out->mem); pthread_mutex_unlock(&g_mem_mu);
        out->off = 0; out->size = req.size; out->block = -1; return r == VK_SUCCESS;
    }
    for (int i = 0; i < g_nblocks; i++) {
        block_t *b = &g_blocks[i]; VkDeviceSize off;
        if (b->type == type && block_take(b, req.size, req.alignment, &off)) { *out = (vk_alloc){b->mem, off, req.size, i}; pthread_mutex_unlock(&g_mem_mu); return 1; }
    }
    g_blocks = realloc(g_blocks, (g_nblocks + 1) * sizeof *g_blocks);
    block_t *b = &g_blocks[g_nblocks]; memset(b, 0, sizeof *b); b->type = type; b->size = BLOCK;
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = BLOCK, .memoryTypeIndex = type};
    if (vkAllocateMemory(V.dev, &ai, NULL, &b->mem) != VK_SUCCESS) { pthread_mutex_unlock(&g_mem_mu); return 0; }
    b->capfree = 16; b->free_off = malloc(16 * 8); b->free_len = malloc(16 * 8); b->free_off[0] = 0; b->free_len[0] = BLOCK; b->nfree = 1;
    VkDeviceSize off; block_take(b, req.size, req.alignment, &off);
    *out = (vk_alloc){b->mem, off, req.size, g_nblocks++};
    pthread_mutex_unlock(&g_mem_mu); return 1;
}
void vk_free_mem(vk_alloc *a) {
    pthread_mutex_lock(&g_mem_mu);
    if (a->block < 0) vkFreeMemory(V.dev, a->mem, NULL);
    else {   /* merged with a neighbour when one touches it */
        block_t *b = &g_blocks[a->block]; int merged = 0;
        for (int i = 0; i < b->nfree && !merged; i++) {
            if (b->free_off[i] + b->free_len[i] == a->off) { b->free_len[i] += a->size; merged = 1; }
            else if (a->off + a->size == b->free_off[i]) { b->free_off[i] = a->off; b->free_len[i] += a->size; merged = 1; }
        }
        if (!merged) {
            if (b->nfree == b->capfree) { b->capfree *= 2; b->free_off = realloc(b->free_off, b->capfree * 8); b->free_len = realloc(b->free_len, b->capfree * 8); }
            b->free_off[b->nfree] = a->off; b->free_len[b->nfree++] = a->size;
        }
    }
    pthread_mutex_unlock(&g_mem_mu);
    memset(a, 0, sizeof *a);
}

/* ---------- guest memory pools ---------- */
static vk_pool *g_pools; static int g_npool, g_cappool; static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
static _Thread_local int g_pool_hint;
static void pool_destroy(void *p) { vk_pool *o = p; vkDestroyBuffer(V.dev, o->buf, NULL); vkFreeMemory(V.dev, o->mem, NULL); free(o); }
void mtl_pool_add(const void *mem, uint64_t size) {
    vk_init();
    uintptr_t lo = (uintptr_t)mem & ~(uintptr_t)(V.host_align - 1), hi = ((uintptr_t)mem + size + V.host_align - 1) & ~(uintptr_t)(V.host_align - 1);
    VkMemoryHostPointerPropertiesEXT hp = {.sType = VK_STRUCTURE_TYPE_MEMORY_HOST_POINTER_PROPERTIES_EXT};
    VKCHECK(V.GetMemoryHostPointerPropertiesEXT(V.dev, VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, (void *)lo, &hp));
    VkExternalMemoryBufferCreateInfo eb = {.sType = VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, .handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT};
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .pNext = &eb, .size = hi - lo,
        .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT |
                 VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDIRECT_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_TEXEL_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_TEXEL_BUFFER_BIT};
    vk_pool *o = calloc(1, sizeof *o); o->lo = (uintptr_t)mem; o->hi = (uintptr_t)mem + size; o->base = lo;
    VKCHECK(vkCreateBuffer(V.dev, &bi, NULL, &o->buf));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, o->buf, &req);
    VkImportMemoryHostPointerInfoEXT imp = {.sType = VK_STRUCTURE_TYPE_IMPORT_MEMORY_HOST_POINTER_INFO_EXT, .handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_HOST_ALLOCATION_BIT_EXT, .pHostPointer = (void *)lo};
    uint32_t type = vk_memtype(req.memoryTypeBits & hp.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (type == UINT32_MAX) type = vk_memtype(req.memoryTypeBits & hp.memoryTypeBits, 0);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .pNext = &imp, .allocationSize = hi - lo, .memoryTypeIndex = type};
    VKCHECK(vkAllocateMemory(V.dev, &ai, NULL, &o->mem));
    VKCHECK(vkBindBufferMemory(V.dev, o->buf, o->mem, 0));
    pthread_mutex_lock(&g_pool_mu);
    if (g_npool == g_cappool) { g_cappool = g_cappool ? g_cappool * 2 : 256; g_pools = realloc(g_pools, g_cappool * sizeof *g_pools); }
    g_pools[g_npool++] = *o; free(o);
    pthread_mutex_unlock(&g_pool_mu);
}
void mtl_pool_del(const void *mem) {
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) if (g_pools[i].lo == (uintptr_t)mem) {
        void vk_tex_release_range(uintptr_t lo, uintptr_t hi); vk_tex_release_range(g_pools[i].lo, g_pools[i].hi);
        vk_pool *o = malloc(sizeof *o); *o = g_pools[i]; g_pools[i] = g_pools[--g_npool];
        pthread_mutex_unlock(&g_pool_mu); vk_defer_free(pool_destroy, o); return;
    }
    pthread_mutex_unlock(&g_pool_mu);
}
int vk_pool_find(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    pthread_mutex_lock(&g_pool_mu);
    int h = g_pool_hint;
    if (h < g_npool && addr >= g_pools[h].lo && addr + len <= g_pools[h].hi) { *buf = g_pools[h].buf; *off = addr - g_pools[h].base; pthread_mutex_unlock(&g_pool_mu); return 1; }
    for (int i = 0; i < g_npool; i++) if (addr >= g_pools[i].lo && addr + len <= g_pools[i].hi) {
        g_pool_hint = i; *buf = g_pools[i].buf; *off = addr - g_pools[i].base; pthread_mutex_unlock(&g_pool_mu); return 1; }
    pthread_mutex_unlock(&g_pool_mu); return 0;
}
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) { VkBuffer b; VkDeviceSize o; if (!vk_pool_find(addr, len, &b, &o)) return 0; *buf = (void *)b; *off = o; return 1; }
/* nvnMemoryPool{Flush,Invalidate}MappedRange: the CPU is about to read the range; rendered images over it go back to memory */
void mtl_pool_invalidate(const pool_t *p, uint64_t off, uint64_t n) {
    void vk_tex_writeback_range(uintptr_t lo, uintptr_t hi);
    if (p && p->mem) vk_tex_writeback_range((uintptr_t)p->mem + off, (uintptr_t)p->mem + off + n);
}

/* ---------- submissions ---------- */
#define RING_CHUNK (16ull << 20)
typedef struct { VkBuffer buf; VkDeviceMemory mem; uint8_t *cpu; VkDeviceSize size, used; } chunk_t;
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; } post_t;
typedef struct { void (*fn)(void *); void *p; } dfree_t;
struct sub {
    uint64_t fence; VkCommandBuffer up, main; int up_used;
    chunk_t *chunks; int nchunks;
    post_t *posts; int nposts, capposts;
    dfree_t *frees; int nfrees, capfrees;
    struct sub *next;
};
static sub_t *g_open, *g_inflight_head, *g_inflight_tail, *g_free_subs;
static chunk_t *g_free_chunks; static int g_nfree_chunks;
static pthread_mutex_t g_sub_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_sub_cv = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t g_fence_next = 1, g_applied, g_submitted;
static pthread_t g_done_thread;
/* GPU time: a timestamp at the start and the end of every submission's main command buffer */
#define TS_RING 512
static VkQueryPool g_ts; static _Atomic uint64_t g_gpu_ns;
uint64_t vk_gpu_ns(void) { return atomic_load(&g_gpu_ns); }

static void chunk_new(chunk_t *c, VkDeviceSize size) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
    VKCHECK(vkCreateBuffer(V.dev, &bi, NULL, &c->buf));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, c->buf, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = vk_memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VKCHECK(vkAllocateMemory(V.dev, &ai, NULL, &c->mem)); VKCHECK(vkBindBufferMemory(V.dev, c->buf, c->mem, 0));
    VKCHECK(vkMapMemory(V.dev, c->mem, 0, VK_WHOLE_SIZE, 0, (void **)&c->cpu)); c->size = size; c->used = 0;
}
vk_span vk_ring(VkDeviceSize size, VkDeviceSize align) {
    sub_t *s = g_open; if (!s) VKDIE("ring allocation outside a submission");
    if (align < 16) align = 16;
    chunk_t *c = s->nchunks ? &s->chunks[s->nchunks - 1] : NULL;
    VkDeviceSize o = c ? (c->used + align - 1) & ~(align - 1) : 0;
    if (!c || o + size > c->size) {
        s->chunks = realloc(s->chunks, (s->nchunks + 1) * sizeof *s->chunks); c = &s->chunks[s->nchunks++];
        pthread_mutex_lock(&g_sub_mu);
        if (size <= RING_CHUNK && g_nfree_chunks) { *c = g_free_chunks[--g_nfree_chunks]; pthread_mutex_unlock(&g_sub_mu); c->used = 0; }
        else { pthread_mutex_unlock(&g_sub_mu); chunk_new(c, size > RING_CHUNK ? size : RING_CHUNK); }
        o = 0;
    }
    c->used = o + size;
    return (vk_span){c->buf, c->cpu + o, o};
}
static VkCommandBuffer cb_new(void) {
    VkCommandBuffer cb; VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = V.cmdpool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VKCHECK(vkAllocateCommandBuffers(V.dev, &ai, &cb)); return cb;
}
static void cb_begin(VkCommandBuffer cb) { VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; VKCHECK(vkBeginCommandBuffer(cb, &bi)); }
static pthread_mutex_t g_cmdpool_mu = PTHREAD_MUTEX_INITIALIZER;
void vk_sub_open(void) {
    if (g_open) return;
    vk_init();
    pthread_mutex_lock(&g_sub_mu);
    sub_t *s = g_free_subs; if (s) g_free_subs = s->next; pthread_mutex_unlock(&g_sub_mu);
    pthread_mutex_lock(&g_cmdpool_mu);
    if (!s) { s = calloc(1, sizeof *s); s->up = cb_new(); s->main = cb_new(); }
    else { vkResetCommandBuffer(s->up, 0); vkResetCommandBuffer(s->main, 0); }
    pthread_mutex_unlock(&g_cmdpool_mu);
    s->fence = atomic_fetch_add(&g_fence_next, 1); s->nchunks = 0; s->nposts = 0; s->nfrees = 0; s->up_used = 0; s->next = NULL;
    cb_begin(s->up); cb_begin(s->main);
    if (!g_ts) { VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * TS_RING}; VKCHECK(vkCreateQueryPool(V.dev, &qi, NULL, &g_ts)); vkResetQueryPool(V.dev, g_ts, 0, 2 * TS_RING); }
    vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, g_ts, (uint32_t)(s->fence % TS_RING) * 2);
    g_open = s;
}
VkCommandBuffer vk_cb(void) { vk_sub_open(); return g_open->main; }
VkCommandBuffer vk_upload_cb(void) { vk_sub_open(); g_open->up_used = 1; return g_open->up; }
uint64_t vk_next_fence(void) { vk_sub_open(); return g_open->fence; }
uint64_t vk_done(void) { return atomic_load(&g_applied); }
void vk_sub_post(int kind, volatile uint32_t *val, uint32_t mode, uint32_t v, uint64_t addr) {
    sub_t *s = g_open; vk_sub_open(); s = g_open;
    if (s->nposts == s->capposts) { s->capposts = s->capposts ? s->capposts * 2 : 64; s->posts = realloc(s->posts, s->capposts * sizeof *s->posts); }
    s->posts[s->nposts++] = (post_t){kind, val, mode, v, addr};
}
void vk_defer_free(void (*fn)(void *), void *p) {
    vk_sub_open(); sub_t *s = g_open;
    if (s->nfrees == s->capfrees) { s->capfrees = s->capfrees ? s->capfrees * 2 : 64; s->frees = realloc(s->frees, s->capfrees * sizeof *s->frees); }
    s->frees[s->nfrees++] = (dfree_t){fn, p};
}
static uint64_t gpu_ticks(void) { return (uint64_t)((__uint128_t)mach_absolute_time() * 12 / 625); }
static void *done_thread(void *arg) {
    (void)arg; pthread_setname_np("vk-complete");
    for (;;) {
        pthread_mutex_lock(&g_sub_mu);
        while (!g_inflight_head) pthread_cond_wait(&g_sub_cv, &g_sub_mu);
        sub_t *s = g_inflight_head; pthread_mutex_unlock(&g_sub_mu);
        VkSemaphoreWaitInfo wi = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_WAIT_INFO, .semaphoreCount = 1, .pSemaphores = &V.timeline, .pValues = &s->fence};
        VkResult r = vkWaitSemaphores(V.dev, &wi, 10ull * 1000000000);
        if (r == VK_TIMEOUT) { VKLOG("【红】submission %llu not finished after 10 s", (unsigned long long)s->fence); continue; }
        if (r != VK_SUCCESS) VKDIE("wait for submission %llu: %d (device lost?)", (unsigned long long)s->fence, r);
        for (int i = 0; i < s->nposts; i++) {
            post_t *o = &s->posts[i];
            if (o->kind == 1) { if (o->mode == NVN_EVENT_SIGNAL_MODE_ADD) *o->val += o->v; else *o->val = o->v; }
            else { uint64_t rec[2] = {o->kind == 3 ? 1u << 20 : 0, gpu_ticks()}; memcpy((void *)(uintptr_t)o->addr, rec, sizeof rec); }   /* counters: samples passed reports visible (no queries yet) */
        }
        { uint64_t t[2]; uint32_t q = (uint32_t)(s->fence % TS_RING) * 2;
          if (vkGetQueryPoolResults(V.dev, g_ts, q, 2, sizeof t, t, 8, VK_QUERY_RESULT_64_BIT) == VK_SUCCESS && t[1] > t[0]) atomic_fetch_add(&g_gpu_ns, (uint64_t)((t[1] - t[0]) * V.props.limits.timestampPeriod));
          vkResetQueryPool(V.dev, g_ts, q, 2); }
        for (int i = 0; i < s->nfrees; i++) s->frees[i].fn(s->frees[i].p);
        pthread_mutex_lock(&g_sub_mu);
        for (int i = 0; i < s->nchunks; i++) {
            if (s->chunks[i].size == RING_CHUNK && g_nfree_chunks < 64) { g_free_chunks = realloc(g_free_chunks, (g_nfree_chunks + 1) * sizeof *g_free_chunks); g_free_chunks[g_nfree_chunks++] = s->chunks[i]; }
            else { vkDestroyBuffer(V.dev, s->chunks[i].buf, NULL); vkFreeMemory(V.dev, s->chunks[i].mem, NULL); }
        }
        g_inflight_head = s->next; if (!g_inflight_head) g_inflight_tail = NULL;
        atomic_store(&g_applied, s->fence);
        s->next = g_free_subs; g_free_subs = s;
        pthread_cond_broadcast(&g_sub_cv);
        pthread_mutex_unlock(&g_sub_mu);
    }
    return NULL;
}
void vk_replay_end_pass(void);   /* vk_replay.c */
uint64_t vk_sub_submit(void) {
    if (!g_open) vk_sub_open();
    vk_replay_end_pass();
    sub_t *s = g_open; g_open = NULL;
    if (s->up_used) {   /* uploads first, visible to everything after */
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(s->up, &di);
    }
    { void vk_tex_stage_rendered(VkCommandBuffer cb, uint64_t fence); vk_tex_stage_rendered(s->main, s->fence); }
    vkCmdWriteTimestamp2(s->main, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, g_ts, (uint32_t)(s->fence % TS_RING) * 2 + 1);
    VKCHECK(vkEndCommandBuffer(s->up)); VKCHECK(vkEndCommandBuffer(s->main));
    VkCommandBufferSubmitInfo cbs[2] = {{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->up}, {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->main}};
    VkSemaphoreSubmitInfo sig = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = V.timeline, .value = s->fence, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = s->up_used ? 2 : 1, .pCommandBufferInfos = s->up_used ? cbs : cbs + 1,
        .signalSemaphoreInfoCount = 1, .pSignalSemaphoreInfos = &sig};
    VKCHECK(vkQueueSubmit2(V.queue, 1, &si, VK_NULL_HANDLE));
    atomic_store(&g_submitted, s->fence);
    pthread_mutex_lock(&g_sub_mu);
    if (!g_done_thread) pthread_create(&g_done_thread, NULL, done_thread, NULL);
    if (g_inflight_tail) g_inflight_tail->next = s; else g_inflight_head = s;
    g_inflight_tail = s; pthread_cond_broadcast(&g_sub_cv);
    pthread_mutex_unlock(&g_sub_mu);
    return s->fence;
}
/* a command buffer run at once, outside the submissions (write-backs) */
VkCommandBuffer vk_oneshot_begin(void) { pthread_mutex_lock(&g_cmdpool_mu); VkCommandBuffer cb = cb_new(); pthread_mutex_unlock(&g_cmdpool_mu); cb_begin(cb); return cb; }
void vk_oneshot_end(VkCommandBuffer cb) {
    VKCHECK(vkEndCommandBuffer(cb));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence f; VKCHECK(vkCreateFence(V.dev, &fi, NULL, &f));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    VKCHECK(vkQueueSubmit(V.queue, 1, &si, f)); VKCHECK(vkWaitForFences(V.dev, 1, &f, 1, UINT64_MAX)); vkDestroyFence(V.dev, f, NULL);
    pthread_mutex_lock(&g_cmdpool_mu); vkFreeCommandBuffers(V.dev, V.cmdpool, 1, &cb); pthread_mutex_unlock(&g_cmdpool_mu);
}
uint64_t mtl_last_fence(void) { return atomic_load(&g_submitted); }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    if (atomic_load(&g_applied) >= fence) return 1;
    if (!timeout_ns) return 0;
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t t = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec + (timeout_ns > (1ull << 62) ? (1ull << 62) : timeout_ns);
    ts.tv_sec = t / 1000000000ull; ts.tv_nsec = t % 1000000000ull;
    pthread_mutex_lock(&g_sub_mu);
    while (atomic_load(&g_applied) < fence) if (pthread_cond_timedwait(&g_sub_cv, &g_sub_mu, &ts)) break;
    pthread_mutex_unlock(&g_sub_mu);
    return atomic_load(&g_applied) >= fence;
}
void mtl_finish(void) { uint64_t f = atomic_load(&g_submitted); if (f) mtl_wait(f, UINT64_MAX); }
void mtl_drain(void) { mtl_finish(); }

/* ---------- window, present, screenshots ---------- */
void vk_window_open(void) {
    VkSurfaceCapabilitiesKHR caps; VKCHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.phys, g_surface, &caps));
    uint32_t nf = 32; VkSurfaceFormatKHR fm[32]; VKCHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(V.phys, g_surface, &nf, fm));
    VkSurfaceFormatKHR f = fm[0]; for (uint32_t i = 0; i < nf; i++) if (fm[i].format == VK_FORMAT_B8G8R8A8_UNORM || fm[i].format == VK_FORMAT_R8G8B8A8_UNORM) { f = fm[i]; break; }
    uint32_t npm = 8; VkPresentModeKHR pm[8]; vkGetPhysicalDeviceSurfacePresentModesKHR(V.phys, g_surface, &npm, pm);
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR; const char *want = getenv("TOTK_VK_PRESENT");   /* uncapped by default: mailbox, else immediate */
    for (uint32_t i = 0; i < npm; i++) if (pm[i] == VK_PRESENT_MODE_MAILBOX_KHR && !(want && !strcmp(want, "fifo"))) mode = pm[i];
    if (mode == VK_PRESENT_MODE_FIFO_KHR && !(want && !strcmp(want, "fifo"))) for (uint32_t i = 0; i < npm; i++) if (pm[i] == VK_PRESENT_MODE_IMMEDIATE_KHR) mode = pm[i];
    int w = 1920, h = 1080; SDL_GetWindowSizeInPixels(vk_sdl_window, &w, &h);
    g_sw = caps.currentExtent.width != UINT32_MAX ? caps.currentExtent.width : (uint32_t)w; g_sh = caps.currentExtent.height != UINT32_MAX ? caps.currentExtent.height : (uint32_t)h;
    VkSwapchainCreateInfoKHR sc = {.sType = VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR, .surface = g_surface, .minImageCount = caps.minImageCount + 1 > 3 ? caps.minImageCount + 1 : 3,
        .imageFormat = f.format, .imageColorSpace = f.colorSpace, .imageExtent = {g_sw, g_sh}, .imageArrayLayers = 1,
        .imageUsage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT, .preTransform = caps.currentTransform,
        .compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, .presentMode = mode, .clipped = 1, .oldSwapchain = g_swap};
    if (caps.maxImageCount && sc.minImageCount > caps.maxImageCount) sc.minImageCount = caps.maxImageCount;
    VkSwapchainKHR old = g_swap;
    VKCHECK(vkCreateSwapchainKHR(V.dev, &sc, NULL, &g_swap));
    if (old) vkDestroySwapchainKHR(V.dev, old, NULL);
    g_nswap = 8; VKCHECK(vkGetSwapchainImagesKHR(V.dev, g_swap, &g_nswap, g_swap_img)); g_swap_fmt = f.format;
    for (int i = 0; i < 8; i++) if (!g_acq[i]) {
        VkSemaphoreCreateInfo si = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO}; VKCHECK(vkCreateSemaphore(V.dev, &si, NULL, &g_acq[i])); VKCHECK(vkCreateSemaphore(V.dev, &si, NULL, &g_rel[i]));
        VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO, .flags = VK_FENCE_CREATE_SIGNALED_BIT}; VKCHECK(vkCreateFence(V.dev, &fi, NULL, &g_pfence[i]));
        pthread_mutex_lock(&g_cmdpool_mu); g_pcb[i] = cb_new(); pthread_mutex_unlock(&g_cmdpool_mu);
    }
    VKLOG("swapchain %ux%u format %d, %u images, present mode %d", g_sw, g_sh, f.format, g_nswap, mode);
}
static void barrier_img(VkCommandBuffer cb, VkImage img, VkImageLayout from, VkImageLayout to) {
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT, .oldLayout = from, .newLayout = to,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img, .subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
}
/* TOTK_VK_SHOT=<path>:<present number>: the presented picture of that present as a binary PPM (8-bit RGB) */
static void shot(VkImage img, uint32_t w, uint32_t h, VkFormat fmt, uint64_t n) {
    static const char *spec; static long at = -1; static int init;
    if (!init) { init = 1; spec = getenv("TOTK_VK_SHOT"); if (spec && strrchr(spec, ':')) at = atol(strrchr(spec, ':') + 1); }
    if (at < 0 || (long)n != at) return;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)w * h * 8, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer b; VKCHECK(vkCreateBuffer(V.dev, &bi, NULL, &b)); VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, b, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = vk_memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory m; VKCHECK(vkAllocateMemory(V.dev, &ai, NULL, &m)); VKCHECK(vkBindBufferMemory(V.dev, b, m, 0));
    pthread_mutex_lock(&g_cmdpool_mu); VkCommandBuffer cb = cb_new(); pthread_mutex_unlock(&g_cmdpool_mu);
    cb_begin(cb);
    VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &r);
    VKCHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    VKCHECK(vkQueueSubmit(V.queue, 1, &si, VK_NULL_HANDLE)); VKCHECK(vkQueueWaitIdle(V.queue));
    uint8_t *p; VKCHECK(vkMapMemory(V.dev, m, 0, VK_WHOLE_SIZE, 0, (void **)&p));
    char path[4096]; snprintf(path, sizeof path, "%.*s", (int)(strrchr(spec, ':') - spec), spec);
    FILE *f = fopen(path, "wb");
    if (f) {
        fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (uint32_t i = 0; i < w * h; i++) {
            uint8_t px[3]; uint32_t v; memcpy(&v, p + 4 * i, 4);
            if (fmt == VK_FORMAT_A2B10G10R10_UNORM_PACK32) { px[0] = (v & 1023) >> 2; px[1] = (v >> 10 & 1023) >> 2; px[2] = (v >> 20 & 1023) >> 2; }
            else if (fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB) { px[0] = p[4 * i + 2]; px[1] = p[4 * i + 1]; px[2] = p[4 * i]; }
            else { px[0] = p[4 * i]; px[1] = p[4 * i + 1]; px[2] = p[4 * i + 2]; }
            fwrite(px, 1, 3, f);
        }
        fclose(f); VKLOG("screenshot of present %llu (%ux%u, format %d) -> %s", (unsigned long long)n, w, h, fmt, path);
    }
    vkUnmapMemory(V.dev, m); vkDestroyBuffer(V.dev, b, NULL); vkFreeMemory(V.dev, m, NULL);
    pthread_mutex_lock(&g_cmdpool_mu); vkFreeCommandBuffers(V.dev, V.cmdpool, 1, &cb); pthread_mutex_unlock(&g_cmdpool_mu);
}
static uint64_t g_presents;
void vk_present_image(VkImage img, uint32_t w, uint32_t h, VkFormat fmt) {
    g_presents++;
    shot(img, w, h, fmt, g_presents);
    if (!g_swap) return;
    unsigned i = g_acq_i++ % 8;
    VKCHECK(vkWaitForFences(V.dev, 1, &g_pfence[i], 1, UINT64_MAX));
    uint32_t idx; VkResult r = vkAcquireNextImageKHR(V.dev, g_swap, UINT64_MAX, g_acq[i], VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { vkDeviceWaitIdle(V.dev); vk_window_open(); return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) VKDIE("acquire: %d", r);
    VKCHECK(vkResetFences(V.dev, 1, &g_pfence[i]));
    VkCommandBuffer cb = g_pcb[i]; vkResetCommandBuffer(cb, 0); cb_begin(cb);
    barrier_img(cb, g_swap_img[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int)w, (int)h, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {(int)g_sw, (int)g_sh, 1}}};
    vkCmdBlitImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, g_swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    barrier_img(cb, g_swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    VKCHECK(vkEndCommandBuffer(cb));
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &g_acq[i], .pWaitDstStageMask = &ws,
        .commandBufferCount = 1, .pCommandBuffers = &cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &g_rel[i]};
    VKCHECK(vkQueueSubmit(V.queue, 1, &si, g_pfence[i]));
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &g_rel[i], .swapchainCount = 1, .pSwapchains = &g_swap, .pImageIndices = &idx};
    r = vkQueuePresentKHR(V.queue, &pi);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) { vkDeviceWaitIdle(V.dev); vk_window_open(); }
    else if (r != VK_SUCCESS) VKDIE("present: %d", r);
}
int mtl_has_window(void) { return vk_sdl_window != NULL; }
