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
_Atomic int vk_window_resized;   /* frontend.c: SDL_EVENT_WINDOW_PIXEL_SIZE_CHANGED */
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
    float prio[2] = {1, 1}; uint32_t nqueue = 1; for (uint32_t i = 0; i < nq; i++) if (i == V.qfam && qf[i].queueCount > 1) nqueue = 2;   /* the second queue: read-backs that wait only for their image (vk_oneshot_after) */
    VkDeviceQueueCreateInfo qci = {.sType = VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, .queueFamilyIndex = V.qfam, .queueCount = nqueue, .pQueuePriorities = prio};
    const char *dext[] = {VK_EXT_SHADER_OBJECT_EXTENSION_NAME, VK_EXT_EXTENDED_DYNAMIC_STATE_3_EXTENSION_NAME, VK_EXT_VERTEX_INPUT_DYNAMIC_STATE_EXTENSION_NAME,
        VK_EXT_ROBUSTNESS_2_EXTENSION_NAME, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME, VK_EXT_INDEX_TYPE_UINT8_EXTENSION_NAME, VK_EXT_DEPTH_CLIP_ENABLE_EXTENSION_NAME,
        VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME, VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME, VK_EXT_PROVOKING_VERTEX_EXTENSION_NAME, VK_KHR_DYNAMIC_RENDERING_LOCAL_READ_EXTENSION_NAME, VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    uint32_t ndext = sizeof dext / sizeof *dext - (g_surface ? 0 : 1);
    /* every feature the device has in these structures (the queried chain is the enabled one), minus robust buffer / image
       access (a cost the game does not need; null descriptors stay) */
    VkPhysicalDeviceDynamicRenderingLocalReadFeaturesKHR lr = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DYNAMIC_RENDERING_LOCAL_READ_FEATURES_KHR};
    VkPhysicalDeviceProvokingVertexFeaturesEXT pv = {.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROVOKING_VERTEX_FEATURES_EXT, .pNext = &lr};
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
    vkGetDeviceQueue(V.dev, V.qfam, 0, &V.queue); if (nqueue > 1) vkGetDeviceQueue(V.dev, V.qfam, 1, &V.queue2);
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
    int at = g_npool; while (at > 0 && g_pools[at - 1].lo > o->lo) { g_pools[at] = g_pools[at - 1]; at--; }   /* sorted by address */
    g_pools[at] = *o; g_npool++; free(o);
    pthread_mutex_unlock(&g_pool_mu);
}
void mtl_pool_del(const void *mem) {
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) if (g_pools[i].lo == (uintptr_t)mem) {
        void vk_tex_release_range(uintptr_t lo, uintptr_t hi); vk_tex_release_range(g_pools[i].lo, g_pools[i].hi);
        vk_pool *o = malloc(sizeof *o); *o = g_pools[i]; memmove(&g_pools[i], &g_pools[i + 1], (g_npool - i - 1) * sizeof *g_pools); g_npool--;
        pthread_mutex_unlock(&g_pool_mu); vk_defer_free(pool_destroy, o); return;
    }
    pthread_mutex_unlock(&g_pool_mu);
}
int vk_pool_find(uint64_t addr, uint64_t len, VkBuffer *buf, VkDeviceSize *off) {
    pthread_mutex_lock(&g_pool_mu);
    int h = g_pool_hint;
    if (h < g_npool && addr >= g_pools[h].lo && addr + len <= g_pools[h].hi) { *buf = g_pools[h].buf; *off = addr - g_pools[h].base; pthread_mutex_unlock(&g_pool_mu); return 1; }
    /* the last pool starting at or below addr, then the ones before it (pools may nest: a buffer inside a bigger pool) */
    int lo = 0, hi = g_npool; while (lo < hi) { int m = (lo + hi) / 2; if (g_pools[m].lo <= addr) lo = m + 1; else hi = m; }
    for (int i = lo - 1; i >= 0 && i >= lo - 8; i--) if (addr >= g_pools[i].lo && addr + len <= g_pools[i].hi) {
        g_pool_hint = i; *buf = g_pools[i].buf; *off = addr - g_pools[i].base; pthread_mutex_unlock(&g_pool_mu); return 1; }
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
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr; vk_qlist *q; } post_t;
typedef struct { void (*fn)(void *); void *p; } dfree_t;
struct sub {
    uint64_t fence;
    /* command buffers (prologue for uploads, main), each pair with its own pool: recorded on the game thread, ended and
       submitted on the submit thread; a long submission is cut into several (vk_sub_cut) so the GPU starts on the first while
       the next is recorded and encoded */
    struct { VkCommandPool pool; VkCommandBuffer up, main; int up_used; _Atomic int ended; } cb[64]; int ncb;   /* ended: vkEndCommandBuffer done by an end worker */
    uint32_t q_first, q_n;   /* occlusion query slots taken while it was recorded */
    chunk_t *chunks; int nchunks;
    post_t *posts; int nposts, capposts;
    dfree_t *frees; int nfrees, capfrees;
    struct sub *next;
};
static sub_t *g_open, *g_inflight_head, *g_inflight_tail, *g_free_subs;
static chunk_t *g_free_chunks; static int g_nfree_chunks;
static pthread_mutex_t g_sub_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_sub_cv = PTHREAD_COND_INITIALIZER;
static _Atomic uint64_t g_fence_next = 1, g_applied, g_submitted;
static uint64_t g_oneshot2;   /* read-backs on the second queue (vk_oneshot_after) */
static _Atomic uint64_t g_qsub; static pthread_mutex_t g_qsub_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_qsub_cv = PTHREAD_COND_INITIALIZER;   /* the last fence whose signalling submission vk-submit has queued */
static pthread_t g_done_thread;
/* game-thread time (ns) waiting for room in the job queue, in mtl_wait, in one-shot command buffers, and replaying (vk_wait_stats) */
static _Atomic uint64_t g_wt[4];
void vk_wait_stats(uint64_t w[4]) { for (int i = 0; i < 4; i++) w[i] = atomic_load(&g_wt[i]); }
void vk_replay_ns(uint64_t ns) { g_wt[3] += ns; }
/* GPU time: a timestamp at the start and the end of every submission's main command buffer */
#define TS_RING 512   /* timestamps: (fence % TS_RING, command buffer pair) -> begin / end */
#define TS_SLOT(s, i) ((uint32_t)(((s)->fence % TS_RING) * 64 + (i)) * 2)
/* TOTK_VK_GPU_TIMES: 0 none, 1 (default) a timestamp at the start and the end of each submission, 2 around every command
   buffer pair (the ICD makes a timestamp a barrier plus a compute command: per pair it costs kicks and overlapping spans add up) */
static int gpu_times(void) { static int m = -1; if (m < 0) { const char *e = getenv("TOTK_VK_GPU_TIMES"); m = e ? atoi(e) : 1; } return m; }
static VkQueryPool g_ts; static _Atomic uint64_t g_gpu_ns, g_submit_ns, g_end_ns, g_begin_ns, g_ncb_sub;   /* wall time inside vkQueueSubmit2 / vkEndCommandBuffer / vkBeginCommandBuffer (both of a pair), command buffer pairs submitted */
void vk_cb_stats(uint64_t *begin_ns, uint64_t *pairs) { *begin_ns = atomic_load(&g_begin_ns); *pairs = atomic_load(&g_ncb_sub); }
uint64_t vk_end_ns(void) { return atomic_load(&g_end_ns); }
uint64_t vk_submit_ns(void) { return atomic_load(&g_submit_ns); }
uint64_t vk_gpu_ns(void) { return atomic_load(&g_gpu_ns); }

static void chunk_new(chunk_t *c, VkDeviceSize size) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT |
        VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_VERTEX_BUFFER_BIT};
    VKCHECK(vkCreateBuffer(V.dev, &bi, NULL, &c->buf));
    VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, c->buf, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size,
        .memoryTypeIndex = vk_memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)};   /* cached: the replay reads back what it wrote (uniform copies) */
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = vk_memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
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
static VkCommandBuffer cb_new_in(VkCommandPool pool) {
    VkCommandBuffer cb; VkCommandBufferAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, .commandPool = pool, .level = VK_COMMAND_BUFFER_LEVEL_PRIMARY, .commandBufferCount = 1};
    VKCHECK(vkAllocateCommandBuffers(V.dev, &ai, &cb)); return cb;
}
static VkCommandBuffer cb_new(void) { return cb_new_in(V.cmdpool); }
static void cb_begin(VkCommandBuffer cb) { VkCommandBufferBeginInfo bi = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, .flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT}; VKCHECK(vkBeginCommandBuffer(cb, &bi)); }
static pthread_mutex_t g_cmdpool_mu = PTHREAD_MUTEX_INITIALIZER;
static void cb_pair_begin(sub_t *s, int i) {
    if (!s->cb[i].pool) {
        VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_TRANSIENT_BIT, .queueFamilyIndex = V.qfam};
        VKCHECK(vkCreateCommandPool(V.dev, &cpi, NULL, &s->cb[i].pool)); s->cb[i].up = cb_new_in(s->cb[i].pool); s->cb[i].main = cb_new_in(s->cb[i].pool);
    } else vkResetCommandPool(V.dev, s->cb[i].pool, 0);
    uint64_t b0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    s->cb[i].up_used = 0; cb_begin(s->cb[i].up); cb_begin(s->cb[i].main); atomic_fetch_add(&g_begin_ns, clock_gettime_nsec_np(CLOCK_MONOTONIC) - b0);
    if (gpu_times() == 2 || (gpu_times() == 1 && i == 0)) vkCmdWriteTimestamp2(s->cb[i].main, VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT, g_ts, TS_SLOT(s, i));
}
static void cb_pair_close(sub_t *s, int i, int last) {   /* uploads first, visible to everything after */
    if (gpu_times() == 2) vkCmdWriteTimestamp2(s->cb[i].main, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, g_ts, TS_SLOT(s, i) + 1);
    else if (gpu_times() == 1 && last) vkCmdWriteTimestamp2(s->cb[i].main, VK_PIPELINE_STAGE_2_BOTTOM_OF_PIPE_BIT, g_ts, TS_SLOT(s, 0) + 1);
    if (!s->cb[i].up_used) return;
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(s->cb[i].up, &di);
}
void vk_sub_open(void) {
    if (g_open) return;
    vk_init();
    pthread_mutex_lock(&g_sub_mu);
    sub_t *s = g_free_subs; if (s) g_free_subs = s->next; pthread_mutex_unlock(&g_sub_mu);
    if (!s) s = calloc(1, sizeof *s);
    s->fence = atomic_fetch_add(&g_fence_next, 1); s->nchunks = 0; s->nposts = 0; s->nfrees = 0; s->next = NULL; s->q_n = 0;
    if (!g_ts) { VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 128 * TS_RING}; VKCHECK(vkCreateQueryPool(V.dev, &qi, NULL, &g_ts)); vkResetQueryPool(V.dev, g_ts, 0, 128 * TS_RING); }
    s->ncb = 1; cb_pair_begin(s, 0);
    g_open = s;
}
VkCommandBuffer vk_cb(void) { vk_sub_open(); return g_open->cb[g_open->ncb - 1].main; }
VkCommandBuffer vk_upload_cb(void) { vk_sub_open(); g_open->cb[g_open->ncb - 1].up_used = 1; return g_open->cb[g_open->ncb - 1].up; }
uint64_t vk_next_fence(void) { vk_sub_open(); return g_open->fence; }
uint64_t vk_done(void) { return atomic_load(&g_applied); }
void vk_sub_post(int kind, volatile uint32_t *val, uint32_t mode, uint32_t v, uint64_t addr) {
    sub_t *s = g_open; vk_sub_open(); s = g_open;
    if (s->nposts == s->capposts) { s->capposts = s->capposts ? s->capposts * 2 : 64; s->posts = realloc(s->posts, s->capposts * sizeof *s->posts); }
    s->posts[s->nposts++] = (post_t){kind, val, mode, v, addr, NULL};
}
#define QN 32768
static VkQueryPool g_qpool; static uint32_t g_qnext; static uint64_t g_qres[QN]; static _Atomic uint64_t g_qrep[2];   /* samples-passed reports of 0 / not 0 */
void vk_query_stats(uint64_t r[2]) { r[0] = atomic_load(&g_qrep[0]); r[1] = atomic_load(&g_qrep[1]); }
VkQueryPool vk_query_pool(void) {
    if (!g_qpool) { VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_OCCLUSION, .queryCount = QN};
                    VKCHECK(vkCreateQueryPool(V.dev, &qi, NULL, &g_qpool)); vkResetQueryPool(V.dev, g_qpool, 0, QN); }
    return g_qpool;
}
uint32_t vk_query_alloc(void) {
    vk_sub_open(); vk_query_pool();
    if (!g_open->q_n) g_open->q_first = g_qnext;
    g_open->q_n++; uint32_t s = g_qnext; g_qnext = (g_qnext + 1) % QN; return s;
}
void vk_sub_post_samples(uint64_t addr, vk_qlist *l) { vk_sub_post(3, NULL, 0, 0, addr); g_open->posts[g_open->nposts - 1].q = l; }
/* completion: a submission's slots into g_qres, then reset for reuse */
static void query_land(sub_t *s) {
    for (uint32_t done = 0; done < s->q_n;) {
        uint32_t first = (s->q_first + done) % QN, n = s->q_n - done; if (first + n > QN) n = QN - first;
        VkResult r = vkGetQueryPoolResults(V.dev, g_qpool, first, n, n * 8, &g_qres[first], 8, VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT);
        if (r != VK_SUCCESS) { static int said; if (said++ < 4) VKLOG("【红】occlusion results %u+%u of submission %llu: %d", first, n, (unsigned long long)s->fence, r); memset(&g_qres[first], 0, n * 8); }
        vkResetQueryPool(V.dev, g_qpool, first, n); done += n;
    }
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
        query_land(s);
        for (int i = 0; i < s->nposts; i++) {
            post_t *o = &s->posts[i];
            if (o->kind == 1) { if (o->mode == NVN_EVENT_SIGNAL_MODE_ADD) *o->val += o->v; else *o->val = o->v; }
            else {   /* NVNcounterData {counter, timestamp}: samples passed = the slots since the reset; other counters 0 (nvn_mtl.m) */
                uint64_t sum = 0;
                if (o->q) { for (int k = 0; k < o->q->n; k++) sum += g_qres[o->q->slot[k]];
                            if (!sum && o->q->skipped) sum = 1;   /* a counted draw was not drawn: visible, as nvn_mtl.m */
                            { static int log = -1, n; if (log < 0) log = getenv("TOTK_VK_QUERY_LOG") != NULL; if (log && n++ < 400) VKLOG("query report %#llx: %d slots, %d draws counted, %llu samples", (unsigned long long)o->addr, o->q->n, o->q->draws, (unsigned long long)sum); }
                            static int vis = -1; if (vis < 0) vis = getenv("TOTK_VK_OCCL_VISIBLE") != NULL;
                            if (vis) sum = 1u << 20;
                            free(o->q); }
                if (o->q || o->kind == 3) atomic_fetch_add(&g_qrep[sum != 0], 1);
                uint64_t rec[2] = {sum, gpu_ticks()}; memcpy((void *)(uintptr_t)o->addr, rec, sizeof rec);
            }
        }
        for (int i = 0; i < (gpu_times() == 2 ? s->ncb : gpu_times() ? 1 : 0); i++) { uint64_t t[2]; uint32_t q = TS_SLOT(s, i);
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
/* the submit thread: the ICD encodes a command buffer for the hardware inside vkQueueSubmit (about 37 % of the game's CPU time
   in the world), so submissions and presents are queued to a thread of their own, in order, and the game thread goes on
   replaying the next frame. Every queue operation runs there or under g_queue_mu after the jobs before it. */
typedef struct { int kind; sub_t *s; int cb, last; VkImage img; uint32_t w, h; VkFormat fmt; } job_t;
#define JOBS 64
static job_t g_jobs[JOBS]; static unsigned g_job_head, g_job_tail; static int g_job_busy;
static pthread_mutex_t g_job_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_job_cv = PTHREAD_COND_INITIALIZER;
static pthread_mutex_t g_queue_mu = PTHREAD_MUTEX_INITIALIZER;
static pthread_t g_submit_thread;
static VkCommandPool g_pool2;   /* command buffers recorded on the submit thread (present, screenshots) */
/* TOTK_VK_END_THREADS=<n> (default 1, on the submit thread: in parallel the ICD mixes occlusion-query results between command buffers, r49d-f): the ICD encodes in vkEndCommandBuffer, so the pairs of a
   submission are ended by n workers in parallel as soon as they are cut; the submit thread still submits them in order,
   each once its end is done */
static int end_threads(void) { static int n = -1; if (n < 0) { const char *e = getenv("TOTK_VK_END_THREADS"); n = e ? atoi(e) : 1; if (n > 8) n = 8; } return n; }
typedef struct { sub_t *s; int i; } end_job;
#define ENDQ 256
static end_job g_endq[ENDQ]; static uint64_t g_endq_head, g_endq_tail; static pthread_mutex_t g_end_mu = PTHREAD_MUTEX_INITIALIZER; static pthread_cond_t g_end_cv = PTHREAD_COND_INITIALIZER, g_ended_cv = PTHREAD_COND_INITIALIZER;
static void end_pair(sub_t *s, int i) {
    uint64_t e0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    VKCHECK(vkEndCommandBuffer(s->cb[i].up)); VKCHECK(vkEndCommandBuffer(s->cb[i].main)); atomic_fetch_add(&g_end_ns, clock_gettime_nsec_np(CLOCK_MONOTONIC) - e0);
}
static void *end_worker(void *arg) {
    (void)arg; pthread_setname_np("vk-end");
    for (;;) {
        pthread_mutex_lock(&g_end_mu); while (g_endq_head == g_endq_tail) pthread_cond_wait(&g_end_cv, &g_end_mu);
        end_job j = g_endq[g_endq_tail++ % ENDQ]; pthread_mutex_unlock(&g_end_mu);
        end_pair(j.s, j.i);
        pthread_mutex_lock(&g_end_mu); atomic_store(&j.s->cb[j.i].ended, 1); pthread_cond_broadcast(&g_ended_cv); pthread_mutex_unlock(&g_end_mu);
    }
    return NULL;
}
static void end_dispatch(sub_t *s, int i) {   /* the pair is closed: a worker ends it */
    atomic_store(&s->cb[i].ended, 0);
    if (end_threads() <= 1) return;
    static int started; pthread_mutex_lock(&g_end_mu);
    if (!started) { started = 1; for (int k = 0; k < end_threads(); k++) { pthread_t t; pthread_create(&t, NULL, end_worker, NULL); pthread_detach(t); } }
    while (g_endq_head - g_endq_tail >= ENDQ) pthread_cond_wait(&g_ended_cv, &g_end_mu);
    g_endq[g_endq_head++ % ENDQ] = (end_job){s, i}; pthread_cond_signal(&g_end_cv); pthread_mutex_unlock(&g_end_mu);
}
static void end_wait(sub_t *s, int i) {   /* the submit thread: the pair's end is done (or it does it itself) */
    if (end_threads() <= 1) { end_pair(s, i); return; }
    if (atomic_load(&s->cb[i].ended)) return;
    pthread_mutex_lock(&g_end_mu); while (!atomic_load(&s->cb[i].ended)) pthread_cond_wait(&g_ended_cv, &g_end_mu); pthread_mutex_unlock(&g_end_mu);
}
static void submit_now(sub_t *s, int i, int last) {
    end_wait(s, i);   /* the ICD encodes at the end of the command buffer: off the game thread (end workers) */
    atomic_fetch_add(&g_ncb_sub, 1);
    VkCommandBufferSubmitInfo cbs[2] = {{.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->cb[i].up}, {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = s->cb[i].main}};
    VkSemaphoreSubmitInfo sig = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = V.timeline, .value = s->fence, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .commandBufferInfoCount = s->cb[i].up_used ? 2 : 1, .pCommandBufferInfos = s->cb[i].up_used ? cbs : cbs + 1,
        .signalSemaphoreInfoCount = last ? 1 : 0, .pSignalSemaphoreInfos = &sig};
    pthread_mutex_lock(&g_queue_mu); uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    VKCHECK(vkQueueSubmit2(V.queue, 1, &si, VK_NULL_HANDLE)); atomic_fetch_add(&g_submit_ns, clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0); pthread_mutex_unlock(&g_queue_mu);
    if (last) { pthread_mutex_lock(&g_qsub_mu); atomic_store(&g_qsub, s->fence); pthread_cond_broadcast(&g_qsub_cv); pthread_mutex_unlock(&g_qsub_mu); }   /* the timeline point is in the queue */
    if (!last) return;
    pthread_mutex_lock(&g_sub_mu);
    if (!g_done_thread) pthread_create(&g_done_thread, NULL, done_thread, NULL);
    if (g_inflight_tail) g_inflight_tail->next = s; else g_inflight_head = s;
    g_inflight_tail = s; pthread_cond_broadcast(&g_sub_cv);
    pthread_mutex_unlock(&g_sub_mu);
}
static void present_now(VkImage img, uint32_t w, uint32_t h, VkFormat fmt);
static void *submit_thread(void *arg) {
    (void)arg; pthread_setname_np("vk-submit");
    VkCommandPoolCreateInfo cpi = {.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, .flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT, .queueFamilyIndex = V.qfam};
    VKCHECK(vkCreateCommandPool(V.dev, &cpi, NULL, &g_pool2));
    for (;;) {
        pthread_mutex_lock(&g_job_mu);
        while (g_job_head == g_job_tail) { g_job_busy = 0; pthread_cond_broadcast(&g_job_cv); pthread_cond_wait(&g_job_cv, &g_job_mu); }
        job_t j = g_jobs[g_job_tail % JOBS]; g_job_busy = 1; pthread_mutex_unlock(&g_job_mu);
        if (j.kind == 1) submit_now(j.s, j.cb, j.last); else present_now(j.img, j.w, j.h, j.fmt);
        pthread_mutex_lock(&g_job_mu); g_job_tail++; pthread_cond_broadcast(&g_job_cv); pthread_mutex_unlock(&g_job_mu);
    }
    return NULL;
}
static void job_push(job_t j) {
    pthread_mutex_lock(&g_job_mu);
    if (!g_submit_thread) pthread_create(&g_submit_thread, NULL, submit_thread, NULL);
    if (g_job_head - g_job_tail >= JOBS) { uint64_t t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC); while (g_job_head - g_job_tail >= JOBS) pthread_cond_wait(&g_job_cv, &g_job_mu); g_wt[0] += clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0; }
    g_jobs[g_job_head % JOBS] = j; g_job_head++; pthread_cond_broadcast(&g_job_cv);
    pthread_mutex_unlock(&g_job_mu);
}
static void jobs_drain(void) {   /* every queued submission and present has reached the queue */
    pthread_mutex_lock(&g_job_mu); while (g_job_head != g_job_tail) pthread_cond_wait(&g_job_cv, &g_job_mu); pthread_mutex_unlock(&g_job_mu);
}
/* a long submission goes to the GPU in parts: the open command buffers are closed and queued (between passes) and recording
   goes on in the next pair; the last part signals the submission's fence */
void vk_sub_cut(void) {
    sub_t *s = g_open; if (!s || s->ncb == 64) return;
    cb_pair_close(s, s->ncb - 1, 0); end_dispatch(s, s->ncb - 1); job_push((job_t){.kind = 1, .s = s, .cb = s->ncb - 1, .last = 0});
    cb_pair_begin(s, s->ncb++);
}
uint64_t vk_sub_submit(void) {
    if (!g_open) vk_sub_open();
    vk_replay_end_pass();
    { void vk_tex_stage_rendered(VkCommandBuffer cb, uint64_t fence); vk_tex_stage_rendered(vk_cb(), g_open->fence); }
    sub_t *s = g_open; g_open = NULL;
    int i = s->ncb - 1; cb_pair_close(s, i, 1); end_dispatch(s, i);
    atomic_store(&g_submitted, s->fence);
    job_push((job_t){.kind = 1, .s = s, .cb = i, .last = 1});
    return s->fence;
}
/* a command buffer run at once, outside the submissions (write-backs) */
VkCommandBuffer vk_oneshot_begin(void) { pthread_mutex_lock(&g_cmdpool_mu); VkCommandBuffer cb = cb_new(); pthread_mutex_unlock(&g_cmdpool_mu); cb_begin(cb); return cb; }
void vk_oneshot_end(VkCommandBuffer cb) {
    uint64_t o0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    VKCHECK(vkEndCommandBuffer(cb));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence f; VKCHECK(vkCreateFence(V.dev, &fi, NULL, &f));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    jobs_drain();
    pthread_mutex_lock(&g_queue_mu); VKCHECK(vkQueueSubmit(V.queue, 1, &si, f)); pthread_mutex_unlock(&g_queue_mu);
    VKCHECK(vkWaitForFences(V.dev, 1, &f, 1, UINT64_MAX)); vkDestroyFence(V.dev, f, NULL); g_wt[2] += clock_gettime_nsec_np(CLOCK_MONOTONIC) - o0;
    pthread_mutex_lock(&g_cmdpool_mu); vkFreeCommandBuffers(V.dev, V.cmdpool, 1, &cb); pthread_mutex_unlock(&g_cmdpool_mu);
}
/* a read-back that needs only the submission with fence value `after` (the last one that rendered its image): on the second
   queue it waits for that value of the timeline, not for the submit thread's backlog and the GPU work queued behind it
   (nvn_mtl.m wb_wait_gpu waits for the image's own command buffer). 0 when it cannot (one queue, or the submission is not
   committed yet): the caller takes the full path */
int vk_oneshot_after(VkCommandBuffer cb, uint64_t after) {
    if (!V.queue2 || !after || after > atomic_load(&g_submitted)) return 0;   /* never rendered (uploads go in order on the main queue) or not committed */
    static pthread_mutex_t mu = PTHREAD_MUTEX_INITIALIZER;
    uint64_t o0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    /* the ICD feeds both queues into one in-order kernel queue (driver device.rs QUEUES): a wait for a point not queued yet would
       block queue 0 behind it, so wait (CPU) until vk-submit has queued the image's submission, not for the rest of the backlog */
    if (atomic_load(&g_qsub) < after) { pthread_mutex_lock(&g_qsub_mu); while (atomic_load(&g_qsub) < after) pthread_cond_wait(&g_qsub_cv, &g_qsub_mu); pthread_mutex_unlock(&g_qsub_mu); }
    VKCHECK(vkEndCommandBuffer(cb));
    VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VkFence f; VKCHECK(vkCreateFence(V.dev, &fi, NULL, &f));
    VkSemaphoreSubmitInfo w = {.sType = VK_STRUCTURE_TYPE_SEMAPHORE_SUBMIT_INFO, .semaphore = V.timeline, .value = after, .stageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT};
    VkCommandBufferSubmitInfo c = {.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_SUBMIT_INFO, .commandBuffer = cb};
    VkSubmitInfo2 si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO_2, .waitSemaphoreInfoCount = 1, .pWaitSemaphoreInfos = &w, .commandBufferInfoCount = 1, .pCommandBufferInfos = &c};
    pthread_mutex_lock(&mu); VKCHECK(vkQueueSubmit2(V.queue2, 1, &si, f)); pthread_mutex_unlock(&mu);
    VKCHECK(vkWaitForFences(V.dev, 1, &f, 1, UINT64_MAX)); vkDestroyFence(V.dev, f, NULL); g_wt[2] += clock_gettime_nsec_np(CLOCK_MONOTONIC) - o0; g_oneshot2++;
    pthread_mutex_lock(&g_cmdpool_mu); vkFreeCommandBuffers(V.dev, V.cmdpool, 1, &cb); pthread_mutex_unlock(&g_cmdpool_mu);
    return 1;
}
uint64_t mtl_last_fence(void) { return atomic_load(&g_submitted); }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) {
    if (atomic_load(&g_applied) >= fence) return 1;
    if (!timeout_ns) return 0;
    uint64_t w0 = clock_gettime_nsec_np(CLOCK_MONOTONIC);
    struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts);
    uint64_t t = (uint64_t)ts.tv_sec * 1000000000ull + ts.tv_nsec + (timeout_ns > (1ull << 62) ? (1ull << 62) : timeout_ns);
    ts.tv_sec = t / 1000000000ull; ts.tv_nsec = t % 1000000000ull;
    pthread_mutex_lock(&g_sub_mu);
    while (atomic_load(&g_applied) < fence) if (pthread_cond_timedwait(&g_sub_cv, &g_sub_mu, &ts)) break;
    pthread_mutex_unlock(&g_sub_mu);
    g_wt[1] += clock_gettime_nsec_np(CLOCK_MONOTONIC) - w0;
    return atomic_load(&g_applied) >= fence;
}
void mtl_finish(void) { uint64_t f = atomic_load(&g_submitted); if (f) mtl_wait(f, UINT64_MAX); }
void mtl_drain(void) { mtl_finish(); }

/* ---------- window, present, screenshots ---------- */
void vk_window_open(void) {
    VkSurfaceCapabilitiesKHR caps; VKCHECK(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(V.phys, g_surface, &caps));
    uint32_t nf = 0; VKCHECK(vkGetPhysicalDeviceSurfaceFormatsKHR(V.phys, g_surface, &nf, NULL)); VkSurfaceFormatKHR *fm = calloc(nf ? nf : 1, sizeof *fm);
    VkResult fr = vkGetPhysicalDeviceSurfaceFormatsKHR(V.phys, g_surface, &nf, fm); if (fr != VK_SUCCESS && fr != VK_INCOMPLETE) VKDIE("surface formats: %d", fr);
    VkSurfaceFormatKHR f = fm[0]; for (uint32_t i = 0; i < nf; i++) if (fm[i].format == VK_FORMAT_B8G8R8A8_UNORM || fm[i].format == VK_FORMAT_R8G8B8A8_UNORM) { f = fm[i]; break; }
    free(fm);
    uint32_t npm = 8; VkPresentModeKHR pm[8]; vkGetPhysicalDeviceSurfacePresentModesKHR(V.phys, g_surface, &npm, pm);   /* VK_INCOMPLETE keeps the first 8 */
    /* TOTK_VK_PRESENT=fifo | mailbox | immediate; by default mailbox, else immediate, else fifo (uncapped) */
    const char *want = getenv("TOTK_VK_PRESENT"); VkPresentModeKHR order[3] = {VK_PRESENT_MODE_MAILBOX_KHR, VK_PRESENT_MODE_IMMEDIATE_KHR, VK_PRESENT_MODE_FIFO_KHR};
    if (want && !strcmp(want, "fifo")) order[0] = order[1] = VK_PRESENT_MODE_FIFO_KHR; else if (want && !strcmp(want, "immediate")) order[0] = VK_PRESENT_MODE_IMMEDIATE_KHR;
    VkPresentModeKHR mode = VK_PRESENT_MODE_FIFO_KHR;
    for (int k = 2; k >= 0; k--) for (uint32_t i = 0; i < npm; i++) if (pm[i] == order[k]) mode = order[k];
    int w = 1920, h = 1080; SDL_GetWindowSizeInPixels(vk_sdl_window, &w, &h); if (w <= 0 || h <= 0) { w = 1920; h = 1080; }
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
/* dm_gfx_shot_at (TOTK_INPUT_FILE "shot <path>"): the present <frames> after the request */
static pthread_mutex_t g_shot_mu = PTHREAD_MUTEX_INITIALIZER; static char g_shot_path[1024]; static long g_shot_at = -1;
static uint64_t g_presents;
void dm_gfx_shot_at(long frames, const char *path) { pthread_mutex_lock(&g_shot_mu); snprintf(g_shot_path, sizeof g_shot_path, "%s", path); g_shot_at = (long)g_presents + frames; pthread_mutex_unlock(&g_shot_mu); }
void dm_gfx_ashot(const char *path) { dm_gfx_shot_at(1, path); }
void dm_gfx_window_shot(const char *path) { dm_gfx_shot_at(1, path); }
static void shot(VkImage img, uint32_t w, uint32_t h, VkFormat fmt, uint64_t n) {
    static const char *spec; static long at = -1; static int init;
    if (!init) { init = 1; spec = getenv("TOTK_VK_SHOT"); if (spec && strrchr(spec, ':')) at = atol(strrchr(spec, ':') + 1); }
    char path[1024];
    pthread_mutex_lock(&g_shot_mu); int want = g_shot_at >= 0 && (long)n >= g_shot_at; if (want) { snprintf(path, sizeof path, "%s", g_shot_path); g_shot_at = -1; } pthread_mutex_unlock(&g_shot_mu);
    if (!want && (at < 0 || (long)n != at)) return;
    if (!want) snprintf(path, sizeof path, "%.*s", (int)(strrchr(spec, ':') - spec), spec);
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)w * h * 8, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VkBuffer b; VKCHECK(vkCreateBuffer(V.dev, &bi, NULL, &b)); VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, b, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = vk_memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VkDeviceMemory m; VKCHECK(vkAllocateMemory(V.dev, &ai, NULL, &m)); VKCHECK(vkBindBufferMemory(V.dev, b, m, 0));
    VkCommandBuffer cb = cb_new_in(g_pool2);
    cb_begin(cb);
    VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
    vkCmdCopyImageToBuffer(cb, img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &r);
    VKCHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    pthread_mutex_lock(&g_queue_mu); VKCHECK(vkQueueSubmit(V.queue, 1, &si, VK_NULL_HANDLE)); VKCHECK(vkQueueWaitIdle(V.queue)); pthread_mutex_unlock(&g_queue_mu);
    uint8_t *p; VKCHECK(vkMapMemory(V.dev, m, 0, VK_WHOLE_SIZE, 0, (void **)&p));
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
    vkFreeCommandBuffers(V.dev, g_pool2, 1, &cb);
}
/* TOTK_VK_SEQ=<path>:<first present>:<count>: each presented picture of the range, scaled to SEQ_W x SEQ_H on the GPU without
   waiting (a ring of SEQ_N), appended to <path> as raw RGB once its copy completed; the log gets per present the mean
   absolute difference (0-255) to the previous picture (d1) and to the one before it (d2): a flash is d1 high on two
   consecutive presents with d2 low on the second */
#define SEQ_W 320
#define SEQ_H 180
#define SEQ_N 4
static struct { VkImage img; VkDeviceMemory imem, bmem; VkBuffer buf; uint8_t *cpu; VkCommandBuffer cb; VkFence fence; uint64_t n; int busy; } g_seq[SEQ_N];
static void seq_land(int k) {
    static FILE *f; static uint8_t prev[2][SEQ_W * SEQ_H * 3]; static uint64_t seen; static const char *path;
    if (!g_seq[k].busy) return;
    VKCHECK(vkWaitForFences(V.dev, 1, &g_seq[k].fence, 1, UINT64_MAX)); g_seq[k].busy = 0;
    if (!f) { const char *e = getenv("TOTK_VK_SEQ"); char p[1024]; snprintf(p, sizeof p, "%.*s", (int)(strchr(e, ':') - e), e); path = e; f = fopen(p, "wb"); if (!f) return; }
    uint8_t cur[SEQ_W * SEQ_H * 3]; const uint8_t *s = g_seq[k].cpu; double d1 = 0, d2 = 0;
    for (int i = 0; i < SEQ_W * SEQ_H; i++) for (int c = 0; c < 3; c++) { uint8_t v = s[4 * i + c]; cur[3 * i + c] = v; d1 += abs(v - prev[0][3 * i + c]); d2 += abs(v - prev[1][3 * i + c]); }
    fwrite(cur, 1, sizeof cur, f); fflush(f);
    double np = SEQ_W * SEQ_H * 3.0;
    if (seen >= 2) VKLOG("seq present %llu d1 %.2f d2 %.2f", (unsigned long long)g_seq[k].n, d1 / np, d2 / np);
    memcpy(prev[1], prev[0], sizeof cur); memcpy(prev[0], cur, sizeof cur); seen++; (void)path;
}
static void seq_frame(VkImage img, uint32_t w, uint32_t h, uint64_t n) {
    static long first = -1, count; if (first == -1) { const char *e = getenv("TOTK_VK_SEQ"); const char *c = e ? strchr(e, ':') : NULL; first = c ? atol(c + 1) : -2; count = c && strchr(c + 1, ':') ? atol(strchr(c + 1, ':') + 1) : 600; }
    if (first < 0 || (long)n < first) return;
    int k = (int)(n % SEQ_N);
    seq_land(k);
    if ((long)n >= first + count) { for (int i = 0; i < SEQ_N; i++) seq_land((k + 1 + i) % SEQ_N); return; }
    if (!g_seq[k].img) {
        VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .imageType = VK_IMAGE_TYPE_2D, .format = VK_FORMAT_R8G8B8A8_UNORM, .extent = {SEQ_W, SEQ_H, 1}, .mipLevels = 1, .arrayLayers = 1,
            .samples = VK_SAMPLE_COUNT_1_BIT, .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT};
        VKCHECK(vkCreateImage(V.dev, &ci, NULL, &g_seq[k].img)); VkMemoryRequirements rq; vkGetImageMemoryRequirements(V.dev, g_seq[k].img, &rq);
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = vk_memtype(rq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)};
        VKCHECK(vkAllocateMemory(V.dev, &ai, NULL, &g_seq[k].imem)); VKCHECK(vkBindImageMemory(V.dev, g_seq[k].img, g_seq[k].imem, 0));
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = SEQ_W * SEQ_H * 4, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VKCHECK(vkCreateBuffer(V.dev, &bi, NULL, &g_seq[k].buf)); vkGetBufferMemoryRequirements(V.dev, g_seq[k].buf, &rq);
        ai = (VkMemoryAllocateInfo){.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = vk_memtype(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        VKCHECK(vkAllocateMemory(V.dev, &ai, NULL, &g_seq[k].bmem)); VKCHECK(vkBindBufferMemory(V.dev, g_seq[k].buf, g_seq[k].bmem, 0)); VKCHECK(vkMapMemory(V.dev, g_seq[k].bmem, 0, VK_WHOLE_SIZE, 0, (void **)&g_seq[k].cpu));
        g_seq[k].cb = cb_new_in(g_pool2); VkFenceCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO}; VKCHECK(vkCreateFence(V.dev, &fi, NULL, &g_seq[k].fence));
    }
    VKCHECK(vkResetFences(V.dev, 1, &g_seq[k].fence));
    VkCommandBuffer cb = g_seq[k].cb; vkResetCommandBuffer(cb, 0); cb_begin(cb);
    barrier_img(cb, g_seq[k].img, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int)w, (int)h, 1}}, .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{0, 0, 0}, {SEQ_W, SEQ_H, 1}}};
    vkCmdBlitImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, g_seq[k].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    barrier_img(cb, g_seq[k].img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {SEQ_W, SEQ_H, 1}};
    vkCmdCopyImageToBuffer(cb, g_seq[k].img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, g_seq[k].buf, 1, &r);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb}; vkCmdPipelineBarrier2(cb, &di);
    VKCHECK(vkEndCommandBuffer(cb));
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .commandBufferCount = 1, .pCommandBuffers = &cb};
    pthread_mutex_lock(&g_queue_mu); VKCHECK(vkQueueSubmit(V.queue, 1, &si, g_seq[k].fence)); pthread_mutex_unlock(&g_queue_mu);
    g_seq[k].n = n; g_seq[k].busy = 1;
}
void vk_present_image(VkImage img, uint32_t w, uint32_t h, VkFormat fmt) { job_push((job_t){.kind = 2, .img = img, .w = w, .h = h, .fmt = fmt}); }
static void present_now(VkImage img, uint32_t w, uint32_t h, VkFormat fmt) {
    g_presents++;
    shot(img, w, h, fmt, g_presents); seq_frame(img, w, h, g_presents);
    if (!g_swap) return;
    if (atomic_exchange(&vk_window_resized, 0)) { vkDeviceWaitIdle(V.dev); vk_window_open(); }   /* the window's pixel size changed (frontend.c) */
    unsigned i = g_acq_i++ % 8;
    VKCHECK(vkWaitForFences(V.dev, 1, &g_pfence[i], 1, UINT64_MAX));
    uint32_t idx; VkResult r = vkAcquireNextImageKHR(V.dev, g_swap, UINT64_MAX, g_acq[i], VK_NULL_HANDLE, &idx);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) { vkDeviceWaitIdle(V.dev); vk_window_open(); return; }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) VKDIE("acquire: %d", r);
    VKCHECK(vkResetFences(V.dev, 1, &g_pfence[i]));
    if (!g_pcb[i]) g_pcb[i] = cb_new_in(g_pool2);   /* recorded on the submit thread: its own pool */
    VkCommandBuffer cb = g_pcb[i]; vkResetCommandBuffer(cb, 0); cb_begin(cb);
    barrier_img(cb, g_swap_img[idx], VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    /* the picture keeps its aspect: scaled to fit, centred, black bars when the window's aspect differs */
    double sc_ = (double)g_sw / w < (double)g_sh / h ? (double)g_sw / w : (double)g_sh / h;
    int dw = (int)(w * sc_ + 0.5), dh = (int)(h * sc_ + 0.5), x0 = ((int)g_sw - dw) / 2, y0 = ((int)g_sh - dh) / 2;
    if (dw != (int)g_sw || dh != (int)g_sh) {
        VkClearColorValue black = {{0, 0, 0, 1}}; VkImageSubresourceRange rr = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCmdClearColorImage(cb, g_swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &black, 1, &rr);
        barrier_img(cb, g_swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    }
    VkImageBlit bl = {.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .srcOffsets = {{0, 0, 0}, {(int)w, (int)h, 1}},
                      .dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .dstOffsets = {{x0, y0, 0}, {x0 + dw, y0 + dh, 1}}};
    vkCmdBlitImage(cb, img, VK_IMAGE_LAYOUT_GENERAL, g_swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &bl, VK_FILTER_LINEAR);
    barrier_img(cb, g_swap_img[idx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR);
    VKCHECK(vkEndCommandBuffer(cb));
    VkPipelineStageFlags ws = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    VkSubmitInfo si = {.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO, .waitSemaphoreCount = 1, .pWaitSemaphores = &g_acq[i], .pWaitDstStageMask = &ws,
        .commandBufferCount = 1, .pCommandBuffers = &cb, .signalSemaphoreCount = 1, .pSignalSemaphores = &g_rel[i]};
    pthread_mutex_lock(&g_queue_mu); VKCHECK(vkQueueSubmit(V.queue, 1, &si, g_pfence[i]));
    static int waitidle = -1; if (waitidle < 0) { const char *e = getenv("TOTK_VK_PRESENT_WAITIDLE"); waitidle = e && atoi(e) == 1; }
    if (waitidle) VKCHECK(vkQueueWaitIdle(V.queue));   /* TOTK_VK_PRESENT_WAITIDLE=1 (diagnostic): the blit has finished before the present */
    VkPresentInfoKHR pi = {.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR, .waitSemaphoreCount = 1, .pWaitSemaphores = &g_rel[i], .swapchainCount = 1, .pSwapchains = &g_swap, .pImageIndices = &idx};
    r = vkQueuePresentKHR(V.queue, &pi); pthread_mutex_unlock(&g_queue_mu);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) { vkDeviceWaitIdle(V.dev); vk_window_open(); }
    else if (r != VK_SUCCESS) VKDIE("present: %d", r);
}
int mtl_has_window(void) { return vk_sdl_window != NULL; }
