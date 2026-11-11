/* NVN2 descriptor pools as Vulkan descriptor arrays (Ocarina of Time; nvn_mtl.m descriptor_pool_for_shader, PoolArrays.cs).
   A Switch 2 shader can read the whole texture and sampler pools: the translator declares them as arrays at the sentinel
   handle 0x40000000 of constant buffer 0 (vk.txt "tex <set> <binding> <type> 0 1073741824 <count> ..."), 2^20 separate images
   per dimension and 4096 separate samplers, indexed by the NVN texture ID (handle & 0xfffff) and sampler ID (handle >> 20)
   the shader read from its own constants. Here they live in one descriptor set, set VK_POOL_SET of every pipeline layout
   that has a pool shader: binding 0 the samplers, then one sampled-image array per dimension, each entry the texture of
   that ID viewed in that dimension (left unbound when the texture cannot be). The shader's arrays are moved there and
   shortened to the pools' sizes when it is loaded (vk_pool_spv). The set is filled from the texture / sampler registries
   before the commands that use it (vk_pool_sync). */
#include "vk.h"

#define POOL_TEX (1u << 16)    /* entries of each image array: the game's texture pool has 33,068 descriptors */
#define POOL_SAMP 4096u
static const VkImageViewType g_dims[VK_POOL_DIMS] = {VK_IMAGE_VIEW_TYPE_2D, VK_IMAGE_VIEW_TYPE_2D_ARRAY, VK_IMAGE_VIEW_TYPE_CUBE, VK_IMAGE_VIEW_TYPE_3D};
static VkDescriptorSetLayout g_layout; static VkDescriptorPool g_dpool; static VkDescriptorSet g_set;
static VkImageView *g_view[VK_POOL_DIMS]; static VkSampler *g_samp;   /* what each entry holds */
static uint64_t g_synced_gen = ~0ull, g_synced_present = ~0ull;

VkDescriptorSetLayout vk_pool_layout(void) {
    if (g_layout) return g_layout;
    VkDescriptorSetLayoutBinding b[1 + VK_POOL_DIMS]; VkDescriptorBindingFlags f[1 + VK_POOL_DIMS];
    b[0] = (VkDescriptorSetLayoutBinding){0, VK_DESCRIPTOR_TYPE_SAMPLER, POOL_SAMP, VK_SHADER_STAGE_ALL, NULL};
    for (int i = 0; i < VK_POOL_DIMS; i++) b[1 + i] = (VkDescriptorSetLayoutBinding){(uint32_t)(1 + i), VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, POOL_TEX, VK_SHADER_STAGE_ALL, NULL};
    for (int i = 0; i <= VK_POOL_DIMS; i++) f[i] = VK_DESCRIPTOR_BINDING_PARTIALLY_BOUND_BIT | VK_DESCRIPTOR_BINDING_UPDATE_AFTER_BIND_BIT;
    VkDescriptorSetLayoutBindingFlagsCreateInfo fi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_BINDING_FLAGS_CREATE_INFO, .bindingCount = 1 + VK_POOL_DIMS, .pBindingFlags = f};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .pNext = &fi, .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_UPDATE_AFTER_BIND_POOL_BIT,
        .bindingCount = 1 + VK_POOL_DIMS, .pBindings = b};
    VKCHECK(vkCreateDescriptorSetLayout(V.dev, &li, NULL, &g_layout));
    VkDescriptorPoolSize sz[2] = {{VK_DESCRIPTOR_TYPE_SAMPLER, POOL_SAMP}, {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, POOL_TEX * VK_POOL_DIMS}};
    VkDescriptorPoolCreateInfo pi = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .flags = VK_DESCRIPTOR_POOL_CREATE_UPDATE_AFTER_BIND_BIT, .maxSets = 1, .poolSizeCount = 2, .pPoolSizes = sz};
    VKCHECK(vkCreateDescriptorPool(V.dev, &pi, NULL, &g_dpool));
    VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_dpool, .descriptorSetCount = 1, .pSetLayouts = &g_layout};
    VKCHECK(vkAllocateDescriptorSets(V.dev, &ai, &g_set));
    for (int i = 0; i < VK_POOL_DIMS; i++) g_view[i] = calloc(POOL_TEX, sizeof *g_view[i]);
    g_samp = calloc(POOL_SAMP, sizeof *g_samp);
    VKLOG("descriptor pools: %u samplers, %u textures in each of %d dimensions (set %d)", POOL_SAMP, POOL_TEX, VK_POOL_DIMS, VK_POOL_SET);
    return g_layout;
}

/* the pool array a binding names: 0 samplers, 1.. images by dimension; -1 not a pool array */
int vk_pool_class(const vk_rbind *b) {
    if ((b->kind != RB_TEX && b->kind != RB_IMG) || b->cbuf_slot != 0 || (uint32_t)b->handle != 0x40000000u || b->array < 4096) return -1;
    if (!b->separate) return 0;
    int arr = (b->stype & 0x100) != 0;
    switch (b->stype & 0xff) {
        case 3: return arr ? 2 : 1;
        case 5: if (!arr) return 3; break;
        case 4: return 4;
    }
    VKDIE("descriptor pool array of texture type %#x", b->stype);
}

/* the shader's pool arrays moved to set VK_POOL_SET, binding = class, and shortened to the pools' sizes */
void vk_pool_spv(uint32_t **wp, size_t *np, const vk_rbind *b, int nb) {
    uint32_t *w = *wp; size_t n = *np;
    uint32_t uint_t = 0, bound = w[3];
    for (size_t i = 5; i < n; i += w[i] >> 16) if ((w[i] & 0xffff) == 21 && w[i + 2] == 32 && w[i + 3] == 0) { uint_t = w[i + 1]; break; }   /* OpTypeInt 32 0 */
    if (!uint_t) VKDIE("pool shader without a 32-bit unsigned type");
    uint32_t c_tex = bound++, c_samp = bound++;
    /* the variables of the pool bindings: OpDecorate %v Binding <binding> with OpDecorate %v DescriptorSet <set> */
    uint32_t var[64]; int cls[64], nv = 0;
    for (size_t i = 5; i < n; i += w[i] >> 16) {
        if ((w[i] & 0xffff) != 71 || (w[i] >> 16) != 4 || w[i + 2] != 33) continue;   /* OpDecorate Binding */
        for (int k = 0; k < nb; k++) { int c = vk_pool_class(&b[k]); if (c >= 0 && b[k].binding == w[i + 3] && nv < 64) { var[nv] = w[i + 1]; cls[nv++] = c; w[i + 3] = (uint32_t)c; break; } }
    }
    for (size_t i = 5; i < n; i += w[i] >> 16)
        if ((w[i] & 0xffff) == 71 && (w[i] >> 16) == 4 && w[i + 2] == 34) for (int k = 0; k < nv; k++) if (w[i + 1] == var[k]) w[i + 3] = VK_POOL_SET;   /* DescriptorSet */
    /* each variable's pointee array: OpVariable %ptr -> OpTypePointer %ptr UniformConstant %arr -> OpTypeArray %arr %elem %len */
    for (int k = 0; k < nv; k++) {
        uint32_t ptr = 0, arr = 0;
        for (size_t i = 5; i < n && !ptr; i += w[i] >> 16) if ((w[i] & 0xffff) == 59 && w[i + 2] == var[k]) ptr = w[i + 1];
        for (size_t i = 5; i < n && !arr; i += w[i] >> 16) if ((w[i] & 0xffff) == 32 && w[i + 1] == ptr) arr = w[i + 3];
        for (size_t i = 5; i < n; i += w[i] >> 16) if ((w[i] & 0xffff) == 28 && w[i + 1] == arr) w[i + 3] = cls[k] ? c_tex : c_samp;
    }
    /* the two length constants, before the first type that uses them: right after OpTypeInt 32 0 */
    size_t at = 5; for (; at < n; at += w[at] >> 16) if ((w[at] & 0xffff) == 21 && w[at + 1] == uint_t) { at += w[at] >> 16; break; }
    uint32_t *o = malloc((n + 8) * 4); memcpy(o, w, at * 4);
    o[at] = 4u << 16 | 43; o[at + 1] = uint_t; o[at + 2] = c_tex; o[at + 3] = POOL_TEX;    /* OpConstant */
    o[at + 4] = 4u << 16 | 43; o[at + 5] = uint_t; o[at + 6] = c_samp; o[at + 7] = POOL_SAMP;
    memcpy(o + at + 8, w + at, (n - at) * 4); o[3] = bound;
    free(w); *wp = o; *np = n + 8;
}

/* before commands that read the pools: every registered sampler and texture in its entries (once per registry change and
   present: a texture's image or view can change without the registry, and stale textures are uploaded here) */
void vk_pool_sync(void) {
    vk_pool_layout();
    uint64_t gen = vk_tex_registry_gen(), pres = vk_presents();
    if (gen == g_synced_gen && pres == g_synced_present) return;
    g_synced_gen = gen; g_synced_present = pres;
    enum { BATCH = 256 }; VkWriteDescriptorSet w[BATCH]; VkDescriptorImageInfo ii[BATCH]; int nw = 0;
#define FLUSH() do { if (nw) vkUpdateDescriptorSets(V.dev, (uint32_t)nw, w, 0, NULL); nw = 0; } while (0)
    for (uint32_t id = 1; id < POOL_SAMP; id++) {
        samp_par p; VkSampler s = vk_samp_registry_get((int)id, &p) ? vk_sampler_id((int)id) : VK_NULL_HANDLE;
        if (!s || s == g_samp[id]) continue;
        g_samp[id] = s; ii[nw] = (VkDescriptorImageInfo){s, VK_NULL_HANDLE, VK_IMAGE_LAYOUT_UNDEFINED};
        w[nw] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_set, .dstBinding = 0, .dstArrayElement = id, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER, .pImageInfo = &ii[nw]};
        if (++nw == BATCH) FLUSH();
    }
    for (uint32_t id = 1; id < POOL_TEX; id++) {
        tex_t t; view_t vw; int hv;
        if (!vk_tex_registry_get((int)id, &t, &vw, &hv) || t.d.target == NVN_TEXTURE_TARGET_BUFFER) continue;
        /* an entry whose storage is not in a live memory pool: a texture the game released (NVN keeps every texture in a pool) */
        { VkBuffer pb; VkDeviceSize po; if (!t.mem || !vk_pool_find((uint64_t)(uintptr_t)t.mem, t.L.size, &pb, &po)) continue; }
        vtex *v = vk_tex_get(&t); if (!v) continue;
        vk_tex_upload_if_stale(v, &t);
        for (int d = 0; d < VK_POOL_DIMS; d++) {
            VkImageView view = vk_tex_view_fits(v, g_dims[d]) ? vk_tex_view(v, &t, &vw, hv, g_dims[d], 0) : VK_NULL_HANDLE;
            if (!view || view == g_view[d][id]) continue;
            g_view[d][id] = view; ii[nw] = (VkDescriptorImageInfo){VK_NULL_HANDLE, view, VK_IMAGE_LAYOUT_GENERAL};
            w[nw] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = g_set, .dstBinding = (uint32_t)(1 + d), .dstArrayElement = id, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, .pImageInfo = &ii[nw]};
            if (++nw == BATCH) FLUSH();
        }
    }
    FLUSH();
#undef FLUSH
}
VkDescriptorSet vk_pool_set(void) { return g_set; }
