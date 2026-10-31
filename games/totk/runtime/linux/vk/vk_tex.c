/* Formats, textures and samplers. A texture object (by serial) gets one VkImage; its content comes from guest memory
   (block-linear deswizzled on the CPU, nvn_deswizzle.c) on first use and again whenever the CPU wrote that memory since
   (nvn_dirty.c write tracking), and from the GPU when it is rendered or copied into. All images stay in GENERAL layout. */
#include "vk.h"
#include "../../../third_party/d2r_gfx/gfx/nvn/nvn_dirty.h"
#include "../../../third_party/d2r_gfx/gfx/tex/nvn_deswizzle.h"

/* ---------- formats ---------- */
#define F(v) {v, 0, 0, 0, 1, 1, 0, 0, 0, 0}
#define FI(v) {v, 0, 0, 0, 1, 1, 0, 1, 0, 0}
#define FD(v, s) {v, 1, s, 0, 1, 1, 0, 0, 0, 0}
#define FC(v, w, h) {v, 0, 0, 1, w, h, 0, 0, 0, 0}
static const vk_fmt T[0x9D] = {
    [0x01] = F(VK_FORMAT_R8_UNORM), F(VK_FORMAT_R8_SNORM), FI(VK_FORMAT_R8_UINT), FI(VK_FORMAT_R8_SINT),
    [0x05] = F(VK_FORMAT_R16_SFLOAT), F(VK_FORMAT_R16_UNORM), F(VK_FORMAT_R16_SNORM), FI(VK_FORMAT_R16_UINT), FI(VK_FORMAT_R16_SINT),
    [0x0A] = F(VK_FORMAT_R32_SFLOAT), FI(VK_FORMAT_R32_UINT), FI(VK_FORMAT_R32_SINT),
    [0x0D] = F(VK_FORMAT_R8G8_UNORM), F(VK_FORMAT_R8G8_SNORM), FI(VK_FORMAT_R8G8_UINT), FI(VK_FORMAT_R8G8_SINT),
    [0x11] = F(VK_FORMAT_R16G16_SFLOAT), F(VK_FORMAT_R16G16_UNORM), F(VK_FORMAT_R16G16_SNORM), FI(VK_FORMAT_R16G16_UINT), FI(VK_FORMAT_R16G16_SINT),
    [0x16] = F(VK_FORMAT_R32G32_SFLOAT), FI(VK_FORMAT_R32G32_UINT), FI(VK_FORMAT_R32G32_SINT),
    [0x19] = F(VK_FORMAT_R8G8B8_UNORM), F(VK_FORMAT_R8G8B8_SNORM), FI(VK_FORMAT_R8G8B8_UINT), FI(VK_FORMAT_R8G8B8_SINT),
    [0x1D] = F(VK_FORMAT_R16G16B16_SFLOAT), F(VK_FORMAT_R16G16B16_UNORM), F(VK_FORMAT_R16G16B16_SNORM), FI(VK_FORMAT_R16G16B16_UINT), FI(VK_FORMAT_R16G16B16_SINT),
    [0x22] = F(VK_FORMAT_R32G32B32_SFLOAT), FI(VK_FORMAT_R32G32B32_UINT), FI(VK_FORMAT_R32G32B32_SINT),
    [0x25] = F(VK_FORMAT_R8G8B8A8_UNORM), F(VK_FORMAT_R8G8B8A8_SNORM), FI(VK_FORMAT_R8G8B8A8_UINT), FI(VK_FORMAT_R8G8B8A8_SINT),
    [0x29] = F(VK_FORMAT_R16G16B16A16_SFLOAT), F(VK_FORMAT_R16G16B16A16_UNORM), F(VK_FORMAT_R16G16B16A16_SNORM), FI(VK_FORMAT_R16G16B16A16_UINT), FI(VK_FORMAT_R16G16B16A16_SINT),
    [0x2E] = F(VK_FORMAT_R32G32B32A32_SFLOAT), FI(VK_FORMAT_R32G32B32A32_UINT), FI(VK_FORMAT_R32G32B32A32_SINT),
    [0x31] = {VK_FORMAT_S8_UINT, 0, 1, 0, 1, 1, 0, 1, 0, 0}, FD(VK_FORMAT_D16_UNORM, 0), FD(VK_FORMAT_D32_SFLOAT, 0), FD(VK_FORMAT_D32_SFLOAT, 0),
    [0x35] = FD(VK_FORMAT_D32_SFLOAT_S8_UINT, 1), FD(VK_FORMAT_D32_SFLOAT_S8_UINT, 1),
    [0x37] = F(VK_FORMAT_R8G8B8A8_SRGB), F(VK_FORMAT_R8G8B8A8_SRGB),
    [0x39] = F(VK_FORMAT_A4B4G4R4_UNORM_PACK16), F(VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR), F(VK_FORMAT_A1B5G5R5_UNORM_PACK16_KHR), F(VK_FORMAT_B5G6R5_UNORM_PACK16),
    [0x3D] = F(VK_FORMAT_A2B10G10R10_UNORM_PACK32), FI(VK_FORMAT_A2B10G10R10_UINT_PACK32), F(VK_FORMAT_B10G11R11_UFLOAT_PACK32), F(VK_FORMAT_E5B9G9R9_UFLOAT_PACK32),
    [0x41] = FC(VK_FORMAT_BC1_RGB_UNORM_BLOCK, 4, 4), FC(VK_FORMAT_BC1_RGBA_UNORM_BLOCK, 4, 4), FC(VK_FORMAT_BC2_UNORM_BLOCK, 4, 4), FC(VK_FORMAT_BC3_UNORM_BLOCK, 4, 4),
    FC(VK_FORMAT_BC1_RGB_SRGB_BLOCK, 4, 4), FC(VK_FORMAT_BC1_RGBA_SRGB_BLOCK, 4, 4), FC(VK_FORMAT_BC2_SRGB_BLOCK, 4, 4), FC(VK_FORMAT_BC3_SRGB_BLOCK, 4, 4),
    [0x49] = FC(VK_FORMAT_BC4_UNORM_BLOCK, 4, 4), FC(VK_FORMAT_BC4_SNORM_BLOCK, 4, 4), FC(VK_FORMAT_BC5_UNORM_BLOCK, 4, 4), FC(VK_FORMAT_BC5_SNORM_BLOCK, 4, 4),
    FC(VK_FORMAT_BC7_UNORM_BLOCK, 4, 4), FC(VK_FORMAT_BC7_SRGB_BLOCK, 4, 4), FC(VK_FORMAT_BC6H_SFLOAT_BLOCK, 4, 4), FC(VK_FORMAT_BC6H_UFLOAT_BLOCK, 4, 4),
    /* integer storage read as float without normalization: the scaled formats (vertex input) */
    [0x51] = F(VK_FORMAT_R8_USCALED), F(VK_FORMAT_R8_SSCALED), F(VK_FORMAT_R16_USCALED), F(VK_FORMAT_R16_SSCALED), F(VK_FORMAT_R32_UINT), F(VK_FORMAT_R32_SINT),
    [0x57] = F(VK_FORMAT_R8G8_USCALED), F(VK_FORMAT_R8G8_SSCALED), F(VK_FORMAT_R16G16_USCALED), F(VK_FORMAT_R16G16_SSCALED), F(VK_FORMAT_R32G32_UINT), F(VK_FORMAT_R32G32_SINT),
    [0x5D] = F(VK_FORMAT_R8G8B8_USCALED), F(VK_FORMAT_R8G8B8_SSCALED), F(VK_FORMAT_R16G16B16_USCALED), F(VK_FORMAT_R16G16B16_SSCALED), F(VK_FORMAT_R32G32B32_UINT), F(VK_FORMAT_R32G32B32_SINT),
    [0x63] = F(VK_FORMAT_R8G8B8A8_USCALED), F(VK_FORMAT_R8G8B8A8_SSCALED), F(VK_FORMAT_R16G16B16A16_USCALED), F(VK_FORMAT_R16G16B16A16_SSCALED), F(VK_FORMAT_R32G32B32A32_UINT), F(VK_FORMAT_R32G32B32A32_SINT),
    [0x69] = F(VK_FORMAT_A2B10G10R10_SNORM_PACK32), FI(VK_FORMAT_A2B10G10R10_SINT_PACK32), F(VK_FORMAT_A2B10G10R10_USCALED_PACK32), F(VK_FORMAT_A2B10G10R10_SSCALED_PACK32),
    [0x6D] = F(VK_FORMAT_R8G8B8A8_UNORM), F(VK_FORMAT_R8G8B8A8_SNORM), FI(VK_FORMAT_R8G8B8A8_UINT), FI(VK_FORMAT_R8G8B8A8_SINT),
    [0x71] = F(VK_FORMAT_R16G16B16A16_SFLOAT), F(VK_FORMAT_R16G16B16A16_UNORM), F(VK_FORMAT_R16G16B16A16_SNORM), FI(VK_FORMAT_R16G16B16A16_UINT), FI(VK_FORMAT_R16G16B16A16_SINT),
    [0x76] = F(VK_FORMAT_R32G32B32A32_SFLOAT), FI(VK_FORMAT_R32G32B32A32_UINT), FI(VK_FORMAT_R32G32B32A32_SINT),
    [0x95] = F(VK_FORMAT_R5G6B5_UNORM_PACK16), F(VK_FORMAT_A1R5G5B5_UNORM_PACK16), F(VK_FORMAT_A1R5G5B5_UNORM_PACK16), F(VK_FORMAT_R5G5B5A1_UNORM_PACK16),
    [0x99] = F(VK_FORMAT_B8G8R8A8_UNORM), F(VK_FORMAT_B8G8R8A8_UNORM), F(VK_FORMAT_B8G8R8A8_SRGB), F(VK_FORMAT_B8G8R8A8_SRGB),
};
static vk_fmt g_astc[28];
const vk_fmt *vk_format(int f) {
    if (f >= 0x79 && f <= 0x94) {   /* ASTC: 14 block sizes, LDR then sRGB, in Vulkan's order */
        static const uint8_t bw[14] = {4, 5, 5, 6, 6, 8, 8, 8, 10, 10, 10, 10, 12, 12}, bh[14] = {4, 4, 5, 5, 6, 5, 6, 8, 5, 6, 8, 10, 10, 12};
        int k = f - 0x79, i = k % 14; vk_fmt *e = &g_astc[k];
        if (!e->vk) *e = (vk_fmt){(VkFormat)(VK_FORMAT_ASTC_4x4_UNORM_BLOCK + 2 * i + (k >= 14)), 0, 0, 1, bw[i], bh[i], 16, 0, 0, 0};
        return e;
    }
    if (f <= 0 || f >= (int)(sizeof T / sizeof *T) || !T[f].vk) return NULL;
    return &T[f];
}
/* RGBX formats and the RGB_DXT1 ones read alpha as 1 */
static int fmt_noalpha(int f) { return f == 0x37 || f == 0x41 || f == 0x45 || f == 0x99 || f == 0x9B || (f >= 0x6D && f <= 0x78); }

/* ---------- registries: texture / sampler IDs of the descriptor pools ---------- */
typedef struct { tex_t t; view_t v; int has_v, ok; } treg;
static treg *g_treg; static int g_ntreg; static samp_par *g_sreg; static uint8_t *g_sok; static int g_nsreg;
static pthread_mutex_t g_reg_mu = PTHREAD_MUTEX_INITIALIZER;
void vk_tex_registry_set(int id, const tex_t *t, const view_t *v) {
    if (id < 0 || id >= (1 << 20)) return;
    pthread_mutex_lock(&g_reg_mu);
    if (id >= g_ntreg) { int n = g_ntreg ? g_ntreg : 1024; while (n <= id) n *= 2; g_treg = realloc(g_treg, n * sizeof *g_treg); memset(g_treg + g_ntreg, 0, (n - g_ntreg) * sizeof *g_treg); g_ntreg = n; }
    if (t) { g_treg[id].t = *t; g_treg[id].ok = 1; g_treg[id].has_v = v != NULL; if (v) g_treg[id].v = *v; } else g_treg[id].ok = 0;
    pthread_mutex_unlock(&g_reg_mu);
}
int vk_tex_registry_get(int id, tex_t *t, view_t *v, int *has_view) {
    pthread_mutex_lock(&g_reg_mu);
    int ok = id > 0 && id < g_ntreg && g_treg[id].ok;
    if (ok) { *t = g_treg[id].t; *v = g_treg[id].v; *has_view = g_treg[id].has_v; }
    pthread_mutex_unlock(&g_reg_mu); return ok;
}
void vk_samp_registry_set(int id, const samp_par *p) {
    if (id < 0 || id >= 4096) return;
    pthread_mutex_lock(&g_reg_mu);
    if (id >= g_nsreg) { int n = 4096; g_sreg = realloc(g_sreg, n * sizeof *g_sreg); g_sok = realloc(g_sok, n); memset(g_sok + g_nsreg, 0, n - g_nsreg); g_nsreg = n; }
    if (p) { g_sreg[id] = *p; g_sok[id] = 1; } else g_sok[id] = 0;
    pthread_mutex_unlock(&g_reg_mu);
}
int vk_samp_registry_get(int id, samp_par *p) {
    pthread_mutex_lock(&g_reg_mu); int ok = id > 0 && id < g_nsreg && g_sok[id]; if (ok) *p = g_sreg[id]; pthread_mutex_unlock(&g_reg_mu); return ok;
}
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) { vk_tex_registry_set(id, t, v); }
void mtl_reg_samp(int id, const samp_par *p) { vk_samp_registry_set(id, p); }
void mtl_set_tex_pool_n(int n) { (void)n; }

/* ---------- images ---------- */
typedef struct { VkImageViewType type; VkFormat fmt; uint32_t level0, nlevels, layer0, nlayers; VkComponentMapping swz; VkImageAspectFlags aspect; VkImageView view; } vview;
struct vtex {
    uint64_t serial; VkImage img; vk_alloc mem; VkFormat fmt; const vk_fmt *vf; int nvn_format;
    uint32_t w, h, depth, layers, levels, samples; VkImageType type; int cube;
    const uint8_t *gmem; uint64_t gsize; uint64_t stamp; int uploaded, gpu_owned, uploadable;
    nvn_texdesc d;
    vview *views; int nviews;
    struct vtex *next;
};
#define TEX_HASH 4096
static vtex *g_tex[TEX_HASH]; static pthread_mutex_t g_tex_mu = PTHREAD_MUTEX_INITIALIZER;
static uint64_t g_uploads, g_upload_bytes;
VkImage vk_tex_image(vtex *v) { return v->img; }
void vk_tex_dims(vtex *v, uint32_t *w, uint32_t *h, VkFormat *f) { *w = v->w; *h = v->h; *f = v->fmt; }
int vk_tex_aspect(vtex *v) { return v->vf->depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (v->vf->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : v->vf->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT; }
static uint32_t mipd(uint32_t s, uint32_t l) { s >>= l; return s ? s : 1; }

static vtex *tex_make(const tex_t *t) {
    const vk_fmt *vf = vk_format(t->d.format);
    if (!vf) { static int said[256]; if (t->d.format < 256 && !said[t->d.format]++) VKLOG("【红】texture format %#x has no Vulkan format", t->d.format); return NULL; }
    if (t->d.target == NVN_TEXTURE_TARGET_BUFFER) return NULL;
    vtex *v = calloc(1, sizeof *v); v->serial = t->serial; v->vf = vf; v->fmt = vf->vk; v->nvn_format = t->d.format; v->d = t->d;
    v->w = t->d.width ? t->d.width : 1; v->h = t->d.height ? t->d.height : 1; v->levels = t->d.levels ? t->d.levels : 1; v->samples = t->d.samples > 1 ? t->d.samples : 1;
    v->depth = 1; v->layers = 1; v->type = VK_IMAGE_TYPE_2D;
    switch (t->d.target) {
        case NVN_TEXTURE_TARGET_1D: v->type = VK_IMAGE_TYPE_1D; v->h = 1; break;
        case NVN_TEXTURE_TARGET_1D_ARRAY: v->type = VK_IMAGE_TYPE_1D; v->layers = v->h; v->h = 1; break;
        case NVN_TEXTURE_TARGET_3D: v->type = VK_IMAGE_TYPE_3D; v->depth = t->d.depth ? t->d.depth : 1; break;
        case NVN_TEXTURE_TARGET_2D_ARRAY: case NVN_TEXTURE_TARGET_2D_MULTISAMPLE_ARRAY: v->layers = t->d.depth ? t->d.depth : 1; break;
        case NVN_TEXTURE_TARGET_CUBEMAP: v->cube = 1; v->layers = 6; break;
        case NVN_TEXTURE_TARGET_CUBEMAP_ARRAY: v->cube = 1; v->layers = t->d.depth ? t->d.depth : 6; break;
        default: break;
    }
    VkFormatProperties fp; vkGetPhysicalDeviceFormatProperties(V.phys, vf->vk, &fp);
    VkFormatFeatureFlags ff = fp.optimalTilingFeatures;
    VkImageUsageFlags u = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (ff & VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT) u |= VK_IMAGE_USAGE_SAMPLED_BIT;
    if (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && (t->d.flags & NVN_TEXTURE_FLAGS_IMAGE) && v->samples == 1) u |= VK_IMAGE_USAGE_STORAGE_BIT;
    if (!(u & VK_IMAGE_USAGE_SAMPLED_BIT)) { VKLOG("【红】format %#x (Vulkan %d) cannot be sampled", t->d.format, vf->vk); }
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .flags = VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT | (v->cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) |
        (v->type == VK_IMAGE_TYPE_3D ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0) | (vf->compressed ? VK_IMAGE_CREATE_BLOCK_TEXEL_VIEW_COMPATIBLE_BIT | VK_IMAGE_CREATE_EXTENDED_USAGE_BIT : 0),
        .imageType = v->type, .format = vf->vk, .extent = {v->w, v->h, v->depth}, .mipLevels = v->levels, .arrayLayers = v->layers, .samples = (VkSampleCountFlagBits)v->samples,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = u, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(V.dev, &ci, NULL, &v->img) != VK_SUCCESS) { VKLOG("【红】image %ux%ux%u fmt %d levels %u layers %u failed", v->w, v->h, v->depth, vf->vk, v->levels, v->layers); free(v); return NULL; }
    VkMemoryRequirements req; vkGetImageMemoryRequirements(V.dev, v->img, &req);
    if (!vk_alloc_mem(req, &v->mem)) VKDIE("out of image memory (%llu bytes)", (unsigned long long)req.size);
    VKCHECK(vkBindImageMemory(V.dev, v->img, v->mem.mem, v->mem.off));
    /* into GENERAL once, cleared (render targets that are read before written read 0, as fresh pool memory) */
    VkCommandBuffer cb = vk_upload_cb();
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
        .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED, .newLayout = VK_IMAGE_LAYOUT_GENERAL,
        .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = v->img,
        .subresourceRange = {vk_tex_aspect(v), 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
    v->gmem = t->mem; v->gsize = t->L.size;
    { int bw, bh; v->uploadable = t->mem && !vf->depth && !vf->stencil && v->samples == 1 && nvn_fmt_bpb(t->d.format, &bw, &bh) != 0; }
    return v;
}
vtex *vk_tex_get(const tex_t *t) {
    if (!t || !t->serial) return NULL;
    pthread_mutex_lock(&g_tex_mu);
    vtex *v = g_tex[t->serial % TEX_HASH]; while (v && v->serial != t->serial) v = v->next;
    if (!v && (v = tex_make(t))) { v->next = g_tex[t->serial % TEX_HASH]; g_tex[t->serial % TEX_HASH] = v; }
    pthread_mutex_unlock(&g_tex_mu);
    return v;
}
static void tex_destroy(void *p) {
    vtex *v = p;
    for (int i = 0; i < v->nviews; i++) vkDestroyImageView(V.dev, v->views[i].view, NULL);
    free(v->views); vkDestroyImage(V.dev, v->img, NULL); vk_free_mem(&v->mem); free(v);
}
void vk_tex_release_serial(uint64_t serial) {
    pthread_mutex_lock(&g_tex_mu);
    vtex **pp = &g_tex[serial % TEX_HASH]; while (*pp && (*pp)->serial != serial) pp = &(*pp)->next;
    vtex *v = *pp; if (v) *pp = v->next;
    pthread_mutex_unlock(&g_tex_mu);
    if (v) vk_defer_free(tex_destroy, v);
}
void mtl_tex_release(tex_t *t) { if (t && t->serial) vk_tex_release_serial(t->serial); }
void mtl_dead_forget(const uint64_t *serials, size_t n) { (void)serials; (void)n; }

static void upload(vtex *v) {
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(v->d.format, &bw, &bh);
    if (!bpb) return;
    uint64_t stamp = nvn_dirty_now();
    VkCommandBuffer cb = vk_upload_cb();
    int one_d = v->type == VK_IMAGE_TYPE_1D, linear = nvn_tex_is_linear(&v->d);
    uint32_t nlev = linear ? 1 : v->levels, nlay = linear ? 1 : v->layers;
    for (uint32_t l = 0; l < nlev; l++) {
        uint32_t w = mipd(v->w, l), h = one_d ? 1 : mipd(v->h, l), dd = v->type == VK_IMAGE_TYPE_3D ? mipd(v->depth, l) : 1;
        size_t row = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh, bytes = row * rows * dd;
        for (uint32_t ly = 0; ly < nlay; ly++) {
            vk_span s = vk_ring(bytes, 16);
            if (nvn_deswizzle(&v->d, l, ly, v->gmem, s.cpu, row)) { static int said; if (!said++) VKLOG("【红】texture %#x %ux%u target %d: no deswizzle path", v->d.format, v->w, v->h, v->d.target); return; }
            VkBufferImageCopy r = {.bufferOffset = s.off, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, ly, 1}, .imageExtent = {w, h, dd}};
            vkCmdCopyBufferToImage(cb, s.buf, v->img, VK_IMAGE_LAYOUT_GENERAL, 1, &r);
            g_upload_bytes += bytes;
        }
    }
    v->stamp = stamp; v->uploaded = 1; v->gpu_owned = 0; g_uploads++;
    nvn_dirty_protect(v->gmem, v->gsize);
}
void vk_tex_upload_if_stale(vtex *v, const tex_t *t) {
    (void)t;
    if (!v || !v->uploadable) return;
    if (!v->uploaded) { if (!v->gpu_owned) upload(v); return; }
    if (nvn_dirty_since(v->gmem, v->gsize, v->stamp)) upload(v);
}
void vk_tex_gpu_wrote(vtex *v) { if (v) { v->gpu_owned = 1; if (!v->uploaded) { v->uploaded = 1; v->stamp = nvn_dirty_now(); if (v->uploadable) nvn_dirty_protect(v->gmem, v->gsize); } } }
void vk_tex_readback(vtex *v, const tex_t *t) { (void)v; (void)t; }

static VkComponentSwizzle swz1(int s, VkComponentSwizzle self) {
    switch (s) { case NVN_TEXTURE_SWIZZLE_ZERO: return VK_COMPONENT_SWIZZLE_ZERO; case NVN_TEXTURE_SWIZZLE_ONE: return VK_COMPONENT_SWIZZLE_ONE;
                 case NVN_TEXTURE_SWIZZLE_R: return VK_COMPONENT_SWIZZLE_R; case NVN_TEXTURE_SWIZZLE_G: return VK_COMPONENT_SWIZZLE_G;
                 case NVN_TEXTURE_SWIZZLE_B: return VK_COMPONENT_SWIZZLE_B; case NVN_TEXTURE_SWIZZLE_A: return VK_COMPONENT_SWIZZLE_A; default: return self; }
}
VkImageView vk_tex_view(vtex *v, const tex_t *t, const view_t *vw, int has_view, VkImageViewType type, int attach) {
    vview k = {0};
    int fmt = has_view && vw->set_format ? vw->format : t->d.format;
    const vk_fmt *vf = vk_format(fmt); if (!vf) vf = v->vf;
    if (vf->compressed != v->vf->compressed || vf->depth != v->vf->depth) vf = v->vf;   /* not a compatible view: the image's own format */
    k.fmt = vf->vk; k.type = type;
    k.level0 = has_view && vw->set_levels ? vw->base_level : 0; k.nlevels = has_view && vw->set_levels && vw->nlevels ? vw->nlevels : v->levels - k.level0;
    k.layer0 = has_view && vw->set_layers ? vw->min_layer : 0; k.nlayers = has_view && vw->set_layers && vw->nlayers ? vw->nlayers : v->layers - k.layer0;
    if (k.level0 >= v->levels) k.level0 = v->levels - 1;
    if (k.level0 + k.nlevels > v->levels) k.nlevels = v->levels - k.level0;
    if (k.layer0 >= v->layers) k.layer0 = v->layers - 1;
    if (k.layer0 + k.nlayers > v->layers) k.nlayers = v->layers - k.layer0;
    if (attach) k.nlevels = 1;
    switch (type) {   /* the shader's declared dimension decides; the image supplies what it has */
        case VK_IMAGE_VIEW_TYPE_1D: case VK_IMAGE_VIEW_TYPE_2D: k.nlayers = 1; if (v->type == VK_IMAGE_TYPE_3D) k.type = VK_IMAGE_VIEW_TYPE_3D; break;
        case VK_IMAGE_VIEW_TYPE_CUBE: if (!v->cube || k.nlayers < 6) k.type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; else k.nlayers = 6; break;
        case VK_IMAGE_VIEW_TYPE_CUBE_ARRAY: if (!v->cube || k.nlayers % 6) k.type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
        case VK_IMAGE_VIEW_TYPE_3D: if (v->type != VK_IMAGE_TYPE_3D) k.type = VK_IMAGE_VIEW_TYPE_2D_ARRAY; break;
        default: if (v->type == VK_IMAGE_TYPE_3D) k.type = VK_IMAGE_VIEW_TYPE_3D; break;
    }
    if (v->type == VK_IMAGE_TYPE_1D && k.type != VK_IMAGE_VIEW_TYPE_1D_ARRAY) k.type = VK_IMAGE_VIEW_TYPE_1D;
    if (v->type == VK_IMAGE_TYPE_2D && (k.type == VK_IMAGE_VIEW_TYPE_1D || k.type == VK_IMAGE_VIEW_TYPE_1D_ARRAY)) k.type = k.nlayers > 1 ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    if (v->type == VK_IMAGE_TYPE_3D) { k.layer0 = 0; k.nlayers = 1; }
    k.aspect = vf->depth ? VK_IMAGE_ASPECT_DEPTH_BIT : vf->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    if (attach) k.aspect = vk_tex_aspect(v);
    if (!attach) {
        if (has_view && vw->set_swizzle) {
            k.swz.r = swz1(vw->swz[0], VK_COMPONENT_SWIZZLE_IDENTITY); k.swz.g = swz1(vw->swz[1], VK_COMPONENT_SWIZZLE_IDENTITY);
            k.swz.b = swz1(vw->swz[2], VK_COMPONENT_SWIZZLE_IDENTITY); k.swz.a = swz1(vw->swz[3], VK_COMPONENT_SWIZZLE_IDENTITY);
        } else {
            const nvn_texdesc *d = &t->d;
            k.swz.r = swz1(d->swizzle_r, VK_COMPONENT_SWIZZLE_IDENTITY); k.swz.g = swz1(d->swizzle_g, VK_COMPONENT_SWIZZLE_IDENTITY);
            k.swz.b = swz1(d->swizzle_b, VK_COMPONENT_SWIZZLE_IDENTITY); k.swz.a = swz1(d->swizzle_a, VK_COMPONENT_SWIZZLE_IDENTITY);
        }
        if (fmt_noalpha(fmt) && (k.swz.a == VK_COMPONENT_SWIZZLE_IDENTITY || k.swz.a == VK_COMPONENT_SWIZZLE_A)) k.swz.a = VK_COMPONENT_SWIZZLE_ONE;
        if (vf->depth) { k.swz.g = k.swz.b = VK_COMPONENT_SWIZZLE_ZERO; k.swz.a = VK_COMPONENT_SWIZZLE_ONE; k.swz.r = VK_COMPONENT_SWIZZLE_R; }   /* a depth fetch is (d, 0, 0, 1) */
    }
    for (int i = 0; i < v->nviews; i++) {
        vview *o = &v->views[i];
        if (o->type == k.type && o->fmt == k.fmt && o->level0 == k.level0 && o->nlevels == k.nlevels && o->layer0 == k.layer0 && o->nlayers == k.nlayers &&
            !memcmp(&o->swz, &k.swz, sizeof k.swz) && o->aspect == k.aspect) return o->view;
    }
    VkImageViewUsageCreateInfo vu = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO,
        .usage = attach ? (v->vf->depth || v->vf->stencil ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT) : VK_IMAGE_USAGE_SAMPLED_BIT};
    VkImageViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, .pNext = &vu, .image = v->img, .viewType = k.type, .format = k.fmt, .components = k.swz,
        .subresourceRange = {k.aspect, k.level0, k.nlevels, k.layer0, k.nlayers}};
    if (vkCreateImageView(V.dev, &ci, NULL, &k.view) != VK_SUCCESS) {
        static int said; if (said++ < 8) VKLOG("【红】image view type %d fmt %d levels %u+%u layers %u+%u failed", k.type, k.fmt, k.level0, k.nlevels, k.layer0, k.nlayers);
        return VK_NULL_HANDLE;
    }
    v->views = realloc(v->views, (v->nviews + 1) * sizeof *v->views); v->views[v->nviews++] = k;
    return k.view;
}

/* ---------- samplers (by their parameters) ---------- */
typedef struct samp_ent { samp_par p; VkSampler s; struct samp_ent *next; } samp_ent;
static samp_ent *g_samp[256]; static pthread_mutex_t g_samp_mu = PTHREAD_MUTEX_INITIALIZER;
static VkSamplerAddressMode wrap(int w) {
    switch (w) { case NVN_WRAP_MODE_REPEAT: return VK_SAMPLER_ADDRESS_MODE_REPEAT; case NVN_WRAP_MODE_MIRRORED_REPEAT: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
                 case NVN_WRAP_MODE_MIRROR_CLAMP: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_EDGE: case NVN_WRAP_MODE_MIRROR_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_MIRROR_CLAMP_TO_EDGE;
                 case NVN_WRAP_MODE_CLAMP_TO_BORDER: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER; default: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE; }
}
VkSampler vk_sampler(const samp_par *p) {
    unsigned h = 0; const uint8_t *b = (const uint8_t *)p; for (size_t i = 0; i < sizeof *p; i++) h = h * 31 + b[i]; h &= 255;
    pthread_mutex_lock(&g_samp_mu);
    for (samp_ent *e = g_samp[h]; e; e = e->next) if (!memcmp(&e->p, p, sizeof *p)) { pthread_mutex_unlock(&g_samp_mu); return e->s; }
    int mip = p->minf >= NVN_MIN_FILTER_NEAREST_MIPMAP_NEAREST;
    VkSamplerCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO,
        .magFilter = p->magf == NVN_MAG_FILTER_LINEAR ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .minFilter = (p->minf == NVN_MIN_FILTER_LINEAR || p->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_NEAREST || p->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST,
        .mipmapMode = (p->minf == NVN_MIN_FILTER_NEAREST_MIPMAP_LINEAR || p->minf == NVN_MIN_FILTER_LINEAR_MIPMAP_LINEAR) ? VK_SAMPLER_MIPMAP_MODE_LINEAR : VK_SAMPLER_MIPMAP_MODE_NEAREST,
        .addressModeU = wrap(p->ws), .addressModeV = wrap(p->wt), .addressModeW = wrap(p->wr), .mipLodBias = p->lod_bias,
        .anisotropyEnable = p->aniso > 1, .maxAnisotropy = p->aniso > 16 ? 16 : p->aniso < 1 ? 1 : p->aniso,
        .compareEnable = p->cmp_mode == NVN_COMPARE_MODE_COMPARE_R_TO_TEXTURE, .compareOp = (VkCompareOp)(p->cmp_func ? p->cmp_func - 1 : 7),
        .minLod = p->lod_min, .maxLod = mip ? p->lod_max : p->lod_min + 0.25f};
    if (ci.maxLod < ci.minLod) ci.maxLod = ci.minLod;
    /* the border colour: the nearest standard one (custom colours need a format here) */
    float a = p->border[3], c = (p->border[0] + p->border[1] + p->border[2]) / 3;
    ci.borderColor = a < 0.5f ? VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK : c < 0.5f ? VK_BORDER_COLOR_FLOAT_OPAQUE_BLACK : VK_BORDER_COLOR_FLOAT_OPAQUE_WHITE;
    VkSamplerReductionModeCreateInfo red = {.sType = VK_STRUCTURE_TYPE_SAMPLER_REDUCTION_MODE_CREATE_INFO, .reductionMode = p->reduction == 1 ? VK_SAMPLER_REDUCTION_MODE_MIN : VK_SAMPLER_REDUCTION_MODE_MAX};
    if (p->reduction == 1 || p->reduction == 2) ci.pNext = &red;
    samp_ent *e = calloc(1, sizeof *e); e->p = *p;
    VKCHECK(vkCreateSampler(V.dev, &ci, NULL, &e->s));
    e->next = g_samp[h]; g_samp[h] = e;
    pthread_mutex_unlock(&g_samp_mu);
    return e->s;
}
void vk_tex_stats(uint64_t *uploads, uint64_t *bytes) { *uploads = g_uploads; *bytes = g_upload_bytes; }
