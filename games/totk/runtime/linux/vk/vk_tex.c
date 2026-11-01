/* Formats, textures and samplers. A texture (by its guest memory and layout: the game re-creates texture objects over the
   same memory, as nvn_mtl.m's packaged textures are found by memory) gets one VkImage; its content comes from guest memory
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
static _Atomic uint64_t g_reg_gen;
uint64_t vk_tex_registry_gen(void) { return atomic_load_explicit(&g_reg_gen, memory_order_relaxed); }
void vk_tex_registry_set(int id, const tex_t *t, const view_t *v) {
    if (id < 0 || id >= (1 << 20)) return;
    atomic_fetch_add_explicit(&g_reg_gen, 1, memory_order_relaxed);
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
static _Atomic(VkSampler) g_samp_of[4096];   /* the sampler of each sampler pool id, made on first use (vk_sampler_id) */
void vk_samp_registry_set(int id, const samp_par *p) {
    if (id < 0 || id >= 4096) return;
    atomic_fetch_add_explicit(&g_reg_gen, 1, memory_order_relaxed);
    pthread_mutex_lock(&g_reg_mu);
    if (id >= g_nsreg) { int n = 4096; g_sreg = realloc(g_sreg, n * sizeof *g_sreg); g_sok = realloc(g_sok, n); memset(g_sok + g_nsreg, 0, n - g_nsreg); g_nsreg = n; }
    if (p) { g_sreg[id] = *p; g_sok[id] = 1; } else g_sok[id] = 0;
    g_samp_of[id] = VK_NULL_HANDLE;
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
    const uint8_t *gmem; uint64_t gsize; uint64_t stamp; int uploaded, gpu_owned, uploadable, mirrored;   /* mirrored: content from pool memory (tex_mirrored) */
    nvn_texdesc d;
    vview *views; int nviews;
    int gpu_dirty, in_dirty;  /* rendered since the last write-back; in g_dirty */
    uint64_t made, last_read; int store_always; uint64_t pruned;   /* store pruning (vk_tex_store_needed) */
    /* staged write-back (nvn_mtl.m's botw_wb_stage_encode): a texture the CPU has read back is copied into stage at the end of
       every submission that renders it; a write-back then waits for that submission only and swizzles from stage */
    uint64_t render_fence, stage_fence; int wb_count, in_list, in_tiny; VkBuffer stage; VkDeviceMemory stage_mem; uint8_t *stage_cpu; VkDeviceSize stage_size;
    uint64_t key;   /* hash of memory, format, target, size, levels, samples, linearity */
    VkImageCreateInfo ci; VkFormat pair; int mutable_all;   /* creation info; the sRGB / linear pair the image can be viewed as; any format (vk_tex_view upgrades) */
    struct vtex *next;
};
#define TEX_HASH 4096
static vtex *g_tex[TEX_HASH]; static pthread_mutex_t g_tex_mu = PTHREAD_MUTEX_INITIALIZER;
static vtex **g_dirty; static int g_ndirty, g_capdirty;   /* the images with gpu_dirty set at some point (g_tex_mu): what a write-back range searches */
static void dirty_forget(vtex *v) { if (v->in_dirty) for (int i = 0; i < g_ndirty; i++) if (g_dirty[i] == v) { g_dirty[i] = g_dirty[--g_ndirty]; break; } }   /* g_tex_mu held */
static uint64_t g_uploads, g_upload_bytes;
VkImage vk_tex_image(vtex *v) { return v->img; }
void vk_tex_dims(vtex *v, uint32_t *w, uint32_t *h, VkFormat *f) { *w = v->w; *h = v->h; *f = v->fmt; }
int vk_tex_aspect(vtex *v) { return v->vf->depth ? (VK_IMAGE_ASPECT_DEPTH_BIT | (v->vf->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0)) : v->vf->stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : VK_IMAGE_ASPECT_COLOR_BIT; }
static uint32_t mipd(uint32_t s, uint32_t l) { s >>= l; return s ? s : 1; }
static VkFormat fmt_pair(VkFormat f);

static int tex_mirrored(const tex_t *t);
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
    if (ff & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT;   /* decals read it in the tile (vk_fbread.c) */
    if (ff & VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT) u |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
    if ((ff & VK_FORMAT_FEATURE_STORAGE_IMAGE_BIT) && (t->d.flags & NVN_TEXTURE_FLAGS_IMAGE) && v->samples == 1) u |= VK_IMAGE_USAGE_STORAGE_BIT;
    /* no MUTABLE_FORMAT (the driver keeps such images uncompressed, as Metal does for PixelFormatView): an sRGB / linear pair
       gets it with a format list of the two; a view in another format recreates the image (tex_mutable) */
    VkFormat pair = fmt_pair(vf->vk); VkImageFormatListCreateInfo fl = {.sType = VK_STRUCTURE_TYPE_IMAGE_FORMAT_LIST_CREATE_INFO, .viewFormatCount = 2, .pViewFormats = (VkFormat[]){vf->vk, pair}};
    VkImageCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, .pNext = pair ? &fl : NULL, .flags = (pair ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0) | (v->cube ? VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT : 0) |
        (v->type == VK_IMAGE_TYPE_3D ? VK_IMAGE_CREATE_2D_ARRAY_COMPATIBLE_BIT : 0),
        .imageType = v->type, .format = vf->vk, .extent = {v->w, v->h, v->depth}, .mipLevels = v->levels, .arrayLayers = v->layers, .samples = (VkSampleCountFlagBits)v->samples,
        .tiling = VK_IMAGE_TILING_OPTIMAL, .usage = u, .initialLayout = VK_IMAGE_LAYOUT_UNDEFINED};
    if (vkCreateImage(V.dev, &ci, NULL, &v->img) != VK_SUCCESS) { VKLOG("【红】image %ux%ux%u fmt %d levels %u layers %u failed", v->w, v->h, v->depth, vf->vk, v->levels, v->layers); free(v); return NULL; }
    v->ci = ci; v->ci.pNext = NULL; v->pair = pair;
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
    { int bw, bh; v->uploadable = t->mem && !vf->depth && !vf->stencil && v->samples == 1 && nvn_fmt_bpb(t->d.format, &bw, &bh) != 0; v->mirrored = tex_mirrored(t); }
    return v;
}
/* nvn_mtl.m tex_mirrored: a texture whose pool memory holds its content (a layout call, or a CPU-mapped pool without
   display / compressible / zcull flags, single-sampled colour) is uploaded from that memory; the others are GPU-owned and
   start empty (TOTK_TEX_MIRROR=0: every texture as mirrored) */
static int tex_mirrored(const tex_t *t) {
    if (!t || !t->mem) return 0;
    if (t->has_layout) return 1;
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_TEX_MIRROR"); on = !e || atoi(e); }
    if (!on) return 1;
    const pool_t *p = t->pool;
    if (!p || p->host || !(p->flags & (NVN_MEMORY_POOL_FLAGS_CPU_UNCACHED | NVN_MEMORY_POOL_FLAGS_CPU_CACHED))) return 0;
    if (t->d.flags & (NVN_TEXTURE_FLAGS_DISPLAY | NVN_TEXTURE_FLAGS_COMPRESSIBLE | NVN_TEXTURE_FLAGS_ADAPTIVE_ZCULL | NVN_TEXTURE_FLAGS_DEPTH16_PREFER_FAST_CLEAR | NVN_TEXTURE_FLAGS_ZCULL_SUPPORT_STENCIL)) return 0;
    if (t->d.samples > 1 || t->d.target == 10) return 0;
    if (t->d.format >= 0x31 && t->d.format <= 0x36) return 0;
    return 1;
}
/* nvn_mtl.m stor_acquire: texture objects over the same memory with the same layout share one image; another format of
   the same texel size reads it through a format view (vk_tex_view makes the image mutable). Compressible storage keeps an
   image per format (the storage changes hands with the format), block-compressed formats one per format
   (TOTK_TEX_SHARE=0: one image per format everywhere) */
static int tex_share(void) { static int on = -1; if (on < 0) { const char *e = getenv("TOTK_TEX_SHARE"); on = !e || atoi(e); } return on; }
static uint64_t tex_key(const tex_t *t) {
    uint64_t fk = (uint64_t)t->d.format; int bw = 1, bh = 1, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    const vk_fmt *vf = vk_format(t->d.format);
    int compressed = (t->d.flags & NVN_TEXTURE_FLAGS_COMPRESSIBLE) && t->pool && (t->pool->flags & NVN_MEMORY_POOL_FLAGS_COMPRESSIBLE);
    if (tex_share() && t->mem && bpb && bw == 1 && bh == 1 && vf && !compressed) fk = 0x10000 | (uint64_t)bpb << 4 | (uint64_t)(vf->depth ? 1 : 0) << 2 | (uint64_t)(vf->stencil ? 1 : 0) << 3;
    uint64_t k[6] = {t->mem ? (uint64_t)(uintptr_t)t->mem : t->serial | (1ull << 63), fk | (uint64_t)t->d.target << 20 | (uint64_t)(t->d.flags & 0x54) << 32,
                     (uint64_t)t->d.width | (uint64_t)t->d.height << 32, (uint64_t)t->d.depth | (uint64_t)t->d.levels << 32, (uint64_t)t->d.samples | (uint64_t)t->d.stride << 32,
                     (uint64_t)(t->d.pkg_block_h & 0xff)};
    uint64_t h = 1469598103934665603ull; const uint8_t *b = (const uint8_t *)k; for (size_t i = 0; i < sizeof k; i++) h = (h ^ b[i]) * 1099511628211ull;
    return h;
}
static vtex *g_last; static uint64_t g_last_key;
vtex *vk_tex_get(const tex_t *t) {
    if (!t || !t->serial) return NULL;
    uint64_t key = tex_key(t);
    pthread_mutex_lock(&g_tex_mu);
    if (g_last && g_last_key == key) { vtex *v = g_last; pthread_mutex_unlock(&g_tex_mu); return v; }
    vtex *v = g_tex[key % TEX_HASH]; while (v && v->key != key) v = v->next;
    if (!v && (v = tex_make(t))) {
        /* TOTK_VK_ALIAS_LOG=N: the first N images made over memory another image (another format / size / levels) already covers */
        static long alog = -1; if (alog < 0) { const char *e = getenv("TOTK_VK_ALIAS_LOG"); alog = e ? atol(e) : 0; }
        if (alog > 0 && t->mem) for (int b = 0; b < TEX_HASH && alog > 0; b++) for (vtex *o = g_tex[b]; o && alog > 0; o = o->next)
            if (o->gmem && o->gmem < (const uint8_t *)t->mem + v->gsize && (const uint8_t *)t->mem < o->gmem + o->gsize) { alog--;
                VKLOG("alias: serial %llu fmt %#x %ux%u lv %u flags %#x tgt %d at %p+%#llx over serial %llu fmt %#x %ux%u lv %u flags %#x tgt %d at %p+%#llx (gpu_owned %d, uploaded %d)", (unsigned long long)t->serial, t->d.format, t->d.width, t->d.height, t->d.levels, t->d.flags, t->d.target, (void *)t->mem, (unsigned long long)v->gsize,
                      (unsigned long long)o->serial, o->d.format, o->d.width, o->d.height, o->d.levels, o->d.flags, o->d.target, (void *)o->gmem, (unsigned long long)o->gsize, o->gpu_owned, o->uploaded); }
        v->key = key; v->next = g_tex[key % TEX_HASH]; g_tex[key % TEX_HASH] = v; }
    g_last = v; g_last_key = key;
    pthread_mutex_unlock(&g_tex_mu);
    return v;
}
static void tex_destroy(void *p) {
    vtex *v = p;
    if (v->stage) { vkDestroyBuffer(V.dev, v->stage, NULL); vkFreeMemory(V.dev, v->stage_mem, NULL); }
    for (int i = 0; i < v->nviews; i++) vkDestroyImageView(V.dev, v->views[i].view, NULL);
    free(v->views); vkDestroyImage(V.dev, v->img, NULL); vk_free_mem(&v->mem); free(v);
}
/* images live as long as their memory: a pool going away takes the images made over it */
static void bview_release_range(uintptr_t lo, uintptr_t hi);
void vk_tex_release_range(uintptr_t lo, uintptr_t hi) {
    bview_release_range(lo, hi);
    pthread_mutex_lock(&g_tex_mu);
    for (int b = 0; b < TEX_HASH; b++) for (vtex **pp = &g_tex[b]; *pp;) {
        vtex *v = *pp;
        if ((uintptr_t)v->gmem >= lo && (uintptr_t)v->gmem < hi) { *pp = v->next; if (g_last == v) g_last = NULL; dirty_forget(v); vk_defer_free(tex_destroy, v); } else pp = &v->next;
    }
    pthread_mutex_unlock(&g_tex_mu);
}
void vk_tex_release_serial(uint64_t serial) { (void)serial; }
void mtl_tex_release(tex_t *t) {   /* a texture without memory is made per object: it goes with it */
    if (!t || t->mem) return;
    uint64_t key = tex_key(t);
    pthread_mutex_lock(&g_tex_mu);
    vtex **pp = &g_tex[key % TEX_HASH]; while (*pp && (*pp)->key != key) pp = &(*pp)->next;
    vtex *v = *pp; if (v) { *pp = v->next; if (g_last == v) g_last = NULL; }
    pthread_mutex_unlock(&g_tex_mu);
    if (v) vk_defer_free(tex_destroy, v);
}
void mtl_dead_forget(const uint64_t *serials, size_t n) { (void)serials; (void)n; }

/* GPU deswizzle (deswizzle.comp): a level / layer of a block-linear texture read from its pool buffer by a compute dispatch
   into the ring, then copied into the image. Used when the bytes per block are a power of two (rows of 16-byte segments)
   and the level starts 16-byte aligned in the pool; the rest is deswizzled on the CPU. TOTK_VK_CPU_DESWIZZLE=1: CPU only */
#include "deswizzle_spv.h"
typedef struct { uint32_t src_off, row_bytes, rows, slices, th, td, pitch, segs; } dsw_pc;
static VkPipeline g_dsw; static VkPipelineLayout g_dsw_pl; static uint64_t g_gpu_uploads;
static int dsw_init(void) {
    static int state; if (state) return state > 0;
    const char *e = getenv("TOTK_VK_CPU_DESWIZZLE"); if (e && atoi(e)) { state = -1; return 0; }
    VkDescriptorSetLayoutBinding b[2] = {{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}, {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, NULL}};
    VkDescriptorSetLayoutCreateInfo li = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO, .flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR, .bindingCount = 2, .pBindings = b};
    VkDescriptorSetLayout dl; VKCHECK(vkCreateDescriptorSetLayout(V.dev, &li, NULL, &dl));
    VkPushConstantRange pc = {VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(dsw_pc)};
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &dl, .pushConstantRangeCount = 1, .pPushConstantRanges = &pc};
    VKCHECK(vkCreatePipelineLayout(V.dev, &pli, NULL, &g_dsw_pl));
    VkShaderModuleCreateInfo mi = {.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO, .codeSize = sizeof deswizzle_spv, .pCode = deswizzle_spv};
    VkShaderModule m; VKCHECK(vkCreateShaderModule(V.dev, &mi, NULL, &m));
    VkComputePipelineCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO, .stage = {.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, .stage = VK_SHADER_STAGE_COMPUTE_BIT, .module = m, .pName = "main"}, .layout = g_dsw_pl};
    state = vkCreateComputePipelines(V.dev, VK_NULL_HANDLE, 1, &ci, NULL, &g_dsw) == VK_SUCCESS ? 1 : -1;
    vkDestroyShaderModule(V.dev, m, NULL);
    if (state < 0) VKLOG("【红】deswizzle pipeline failed: CPU deswizzle");
    return state > 0;
}
/* records the dispatch of one level / layer into s (pitch: row bytes rounded to 16); 0 when the CPU has to do it */
static int dsw_record(VkCommandBuffer cb, vtex *v, uint32_t l, uint32_t ly, vk_span *s, uint32_t *pitch, uint32_t *rows_out, uint32_t *slices_out) {
    if (!dsw_init() || nvn_tex_is_linear(&v->d)) return 0;
    uint32_t row_bytes, rows, slices, bpb; uint64_t base; unsigned th, td;
    if (nvn_deswizzle_geom(&v->d, l, ly, &row_bytes, &rows, &slices, &base, &th, &td, &bpb) || (bpb & (bpb - 1))) return 0;
    VkBuffer pb; VkDeviceSize po;
    if (!vk_pool_find((uint64_t)(uintptr_t)v->gmem, v->gsize, &pb, &po) || ((po + base) & 15)) return 0;
    uint32_t p = (row_bytes + 15) & ~15u, segs = p / 16; uint64_t n = (uint64_t)segs * rows * slices;
    if ((n + 63) / 64 > V.props.limits.maxComputeWorkGroupCount[0] || (po & 255) + v->gsize >= (1ull << 32)) return 0;
    VkDeviceSize bo = po & ~(VkDeviceSize)(V.props.limits.minStorageBufferOffsetAlignment - 1);
    *s = vk_ring((VkDeviceSize)p * rows * slices, 256);
    VkDescriptorBufferInfo bi[2] = {{pb, bo, ((po - bo) + v->gsize + 15) & ~15ull}, {s->buf, s->off, (VkDeviceSize)p * rows * slices}};
    VkWriteDescriptorSet w[2] = {{.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[0]},
                                 {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstBinding = 1, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, .pBufferInfo = &bi[1]}};
    dsw_pc pc = {(uint32_t)(po - bo + base), row_bytes, rows, slices, th, td, p, segs};
    vkCmdBindPipeline(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_dsw);
    V.CmdPushDescriptorSetKHR(cb, VK_PIPELINE_BIND_POINT_COMPUTE, g_dsw_pl, 0, 2, w);
    vkCmdPushConstants(cb, g_dsw_pl, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof pc, &pc);
    vkCmdDispatch(cb, (uint32_t)((n + 63) / 64), 1, 1);
    *pitch = p; *rows_out = rows; *slices_out = slices; return 1;
}
static void upload(vtex *v) {
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(v->d.format, &bw, &bh);
    if (!bpb) return;
    uint64_t stamp = nvn_dirty_now();
    VkCommandBuffer cb = vk_upload_cb();
    int one_d = v->type == VK_IMAGE_TYPE_1D, linear = nvn_tex_is_linear(&v->d);
    uint32_t nlev = linear ? 1 : v->levels, nlay = linear ? 1 : v->layers;
    VkBufferImageCopy *cp = malloc(sizeof *cp * nlev * nlay); VkBuffer *cbuf = malloc(sizeof *cbuf * nlev * nlay); int ncp = 0, gpu = 0;
    for (uint32_t l = 0; l < nlev; l++) {
        uint32_t w = mipd(v->w, l), h = one_d ? 1 : mipd(v->h, l), dd = v->type == VK_IMAGE_TYPE_3D ? mipd(v->depth, l) : 1;
        size_t row = (size_t)((w + bw - 1) / bw) * bpb, rows = (h + bh - 1) / bh, bytes = row * rows * dd;
        for (uint32_t ly = 0; ly < nlay; ly++) {
            vk_span s; uint32_t pitch = (uint32_t)row, r_, z_;
            if (dsw_record(cb, v, l, ly, &s, &pitch, &r_, &z_)) gpu = 1;
            else {
                s = vk_ring(bytes, 16);
                if (nvn_deswizzle(&v->d, l, ly, v->gmem, s.cpu, row)) { static int said; if (!said++) VKLOG("【红】texture %#x %ux%u target %d: no deswizzle path", v->d.format, v->w, v->h, v->d.target); free(cp); free(cbuf); return; }
            }
            cp[ncp] = (VkBufferImageCopy){.bufferOffset = s.off, .bufferRowLength = pitch / bpb * bw, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, ly, 1}, .imageExtent = {w, h, dd}};
            cbuf[ncp++] = s.buf; g_upload_bytes += bytes;
        }
    }
    if (gpu) {   /* the dispatches' writes before the copies read them */
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, .srcAccessMask = VK_ACCESS_2_SHADER_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT, .dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(cb, &di); g_gpu_uploads++;
    }
    for (int i = 0; i < ncp; i++) vkCmdCopyBufferToImage(cb, cbuf[i], v->img, VK_IMAGE_LAYOUT_GENERAL, 1, &cp[i]);
    free(cp); free(cbuf);
    if (v->uploaded && getenv("TOTK_VK_UPLOAD_LOG")) VKLOG("re-upload serial %llu fmt %#x %ux%ux%u levels %u layers %u at %p (%llu bytes)", (unsigned long long)v->serial, v->d.format, v->w, v->h, v->depth, v->levels, v->layers, (const void *)v->gmem, (unsigned long long)v->gsize);
    v->stamp = stamp; v->uploaded = 1; v->gpu_owned = 0; g_uploads++;
    nvn_dirty_protect(v->gmem, v->gsize);
}
void vk_tex_upload_if_stale(vtex *v, const tex_t *t) {
    (void)t;
    if (!v || !v->uploadable || !v->mirrored) return;
    if (!v->uploaded) { if (!v->gpu_owned) upload(v); return; }
    /* a rendered texture keeps its image: a CPU write near it on the same pages is not a new content (as nvn_mtl.m's owned textures) */
    if (!v->gpu_owned && nvn_dirty_since(v->gmem, v->gsize, v->stamp)) upload(v);
}
static vtex **g_rendered; static int g_nrendered, g_caprendered;
/* tiny targets (16 KiB or less: the 1x1 foot-placement and water-colour renders) are read by the CPU without an invalidate:
   they land in memory when the submission that rendered them completes (nvn_mtl.m botw_tiny_note, upstream #239) */
#define TINY (16u << 10)
/* late landing (nvn_mtl.m botw_late_note, the grass masks): a target the CPU has flushed 8 times or more, up to 1 MiB, lands in
   memory when its submission completes and a flush no longer waits for it: the CPU reads the last completed rendering, as on
   the console, where the game does not wait for it either */
#define LATE(v) ((v)->wb_count >= 8 && (v)->gsize <= (1u << 20))
static vtex **g_tiny; static int g_ntiny, g_captiny;
void vk_tex_gpu_wrote(vtex *v) { if (v) { v->gpu_owned = 1; v->gpu_dirty = 1; v->render_fence = vk_next_fence();
      if (!v->in_dirty && v->uploadable) { pthread_mutex_lock(&g_tex_mu); if (g_ndirty == g_capdirty) { g_capdirty = g_capdirty ? 2 * g_capdirty : 256; g_dirty = realloc(g_dirty, g_capdirty * sizeof *g_dirty); } g_dirty[g_ndirty++] = v; v->in_dirty = 1; pthread_mutex_unlock(&g_tex_mu); }
      if (v->uploadable && (v->gsize <= TINY || LATE(v)) && !v->in_tiny) { if (g_ntiny == g_captiny) { g_captiny = g_captiny ? 2 * g_captiny : 64; g_tiny = realloc(g_tiny, g_captiny * sizeof *g_tiny); }
          g_tiny[g_ntiny++] = v; v->in_tiny = 1; }
      if (v->wb_count && !v->in_list) { if (g_nrendered == g_caprendered) { g_caprendered = g_caprendered ? 2 * g_caprendered : 64; g_rendered = realloc(g_rendered, g_caprendered * sizeof *g_rendered); }
          g_rendered[g_nrendered++] = v; v->in_list = 1; } if (!v->uploaded) { v->uploaded = 1; v->stamp = nvn_dirty_now(); if (v->uploadable) nvn_dirty_protect(v->gmem, v->gsize); } } }
/* write-back: an image rendered since the last one is copied out and swizzled into its guest memory (the GPU's write on the
   console), not counted as a CPU write */
VkCommandBuffer vk_oneshot_begin(void); void vk_oneshot_end(VkCommandBuffer cb); int vk_oneshot_after(VkCommandBuffer cb, uint64_t after);
static uint64_t g_wb, g_wb_bytes, g_wb_staged;
static VkDeviceSize wb_size(vtex *v, int bw, int bh, int bpb) {
    int one_d = v->type == VK_IMAGE_TYPE_1D, linear = nvn_tex_is_linear(&v->d); uint32_t nlev = linear ? 1 : v->levels, nlay = linear ? 1 : v->layers; VkDeviceSize total = 0;
    for (uint32_t l = 0; l < nlev; l++) { uint32_t w = mipd(v->w, l), h = one_d ? 1 : mipd(v->h, l), dd = v->type == VK_IMAGE_TYPE_3D ? mipd(v->depth, l) : 1;
        total += (VkDeviceSize)((w + bw - 1) / bw) * bpb * ((h + bh - 1) / bh) * dd * nlay; }
    return total;
}
static void wb_copies_at(vtex *v, VkCommandBuffer cb, VkBuffer b, VkDeviceSize o, int bw, int bh, int bpb) {
    vk_tex_mark_read(v);
    int one_d = v->type == VK_IMAGE_TYPE_1D, linear = nvn_tex_is_linear(&v->d); uint32_t nlev = linear ? 1 : v->levels, nlay = linear ? 1 : v->layers;
    for (uint32_t l = 0; l < nlev; l++) {
        uint32_t w = mipd(v->w, l), h = one_d ? 1 : mipd(v->h, l), dd = v->type == VK_IMAGE_TYPE_3D ? mipd(v->depth, l) : 1;
        VkDeviceSize bytes = (VkDeviceSize)((w + bw - 1) / bw) * bpb * ((h + bh - 1) / bh) * dd;
        for (uint32_t ly = 0; ly < nlay; ly++) { VkBufferImageCopy r = {.bufferOffset = o, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, ly, 1}, .imageExtent = {w, h, dd}}; vkCmdCopyImageToBuffer(cb, v->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &r); o += bytes; }
    }
}
static void wb_copies(vtex *v, VkCommandBuffer cb, VkBuffer b, int bw, int bh, int bpb) { wb_copies_at(v, cb, b, 0, bw, bh, bpb); }
static void wb_swizzle(vtex *v, const uint8_t *src, int bw, int bh, int bpb) {
    int one_d = v->type == VK_IMAGE_TYPE_1D, linear = nvn_tex_is_linear(&v->d); uint32_t nlev = linear ? 1 : v->levels, nlay = linear ? 1 : v->layers; VkDeviceSize o = 0;
    void *tok = nvn_dirty_gpu_write_begin((void *)v->gmem, v->gsize);
    for (uint32_t l = 0; l < nlev; l++) {
        uint32_t w = mipd(v->w, l), h = one_d ? 1 : mipd(v->h, l), dd = v->type == VK_IMAGE_TYPE_3D ? mipd(v->depth, l) : 1;
        size_t row = (size_t)((w + bw - 1) / bw) * bpb; VkDeviceSize bytes = row * ((h + bh - 1) / bh) * dd;
        for (uint32_t ly = 0; ly < nlay; ly++) { nvn_swizzle(&v->d, l, ly, src + o, row, (void *)v->gmem); o += bytes; }
    }
    nvn_dirty_gpu_write_end(tok);
}
static void host_buffer(VkDeviceSize size, VkBuffer *b, VkDeviceMemory *m, uint8_t **cpu) {
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = size, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VKCHECK(vkCreateBuffer(V.dev, &bi, NULL, b)); VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, *b, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = vk_memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT)};
    if (ai.memoryTypeIndex == UINT32_MAX) ai.memoryTypeIndex = vk_memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
    VKCHECK(vkAllocateMemory(V.dev, &ai, NULL, m)); VKCHECK(vkBindBufferMemory(V.dev, *b, *m, 0)); VKCHECK(vkMapMemory(V.dev, *m, 0, VK_WHOLE_SIZE, 0, (void **)cpu));
}
typedef struct { vtex *v; const uint8_t *src; } tiny_t;
static uint64_t g_tiny_landed;
static void tiny_land(void *p) {   /* completion thread: the submission finished */
    tiny_t *t = p; int bw = 1, bh = 1, bpb = nvn_fmt_bpb(t->v->d.format, &bw, &bh);
    wb_swizzle(t->v, t->src, bw, bh, bpb); t->v->gpu_dirty = 0; g_tiny_landed++; free(t);
}
/* end of a submission (vk_dev.c, after its last pass): the rendered textures the CPU reads go to their stages */
void vk_tex_stage_rendered(VkCommandBuffer cb, uint64_t fence) {
    for (int i = 0; i < g_ntiny; i++) {
        vtex *v = g_tiny[i]; v->in_tiny = 0;
        int bw = 1, bh = 1, bpb = nvn_fmt_bpb(v->d.format, &bw, &bh); VkDeviceSize n = wb_size(v, bw, bh, bpb);
        if (!bpb || !n) continue;
        vk_span r = vk_ring(n, 16); wb_copies_at(v, cb, r.buf, r.off, bw, bh, bpb);
        tiny_t *t = malloc(sizeof *t); t->v = v; t->src = r.cpu; vk_defer_free(tiny_land, t);
    }
    g_ntiny = 0;
    for (int i = 0; i < g_nrendered; i++) {
        vtex *v = g_rendered[i]; v->in_list = 0;
        int bw = 1, bh = 1, bpb = nvn_fmt_bpb(v->d.format, &bw, &bh); VkDeviceSize n = wb_size(v, bw, bh, bpb);
        if (!bpb || n > (4u << 20)) continue;
        if (!v->stage || v->stage_size < n) { if (v->stage) { vkDestroyBuffer(V.dev, v->stage, NULL); vkFreeMemory(V.dev, v->stage_mem, NULL); } host_buffer(n, &v->stage, &v->stage_mem, &v->stage_cpu); v->stage_size = n; }
        wb_copies(v, cb, v->stage, bw, bh, bpb); v->stage_fence = fence;
    }
    g_nrendered = 0;
}
static void writeback(vtex *v) {
    int sbw = 1, sbh = 1, sbpb = nvn_fmt_bpb(v->d.format, &sbw, &sbh);
    v->wb_count++;
    if (LATE(v) && v->gpu_dirty) return;   /* lands when its submission completes (tiny_land) */
    static int log = -1; if (log < 0) log = getenv("TOTK_VK_WB_LOG") != NULL;
    if (log) VKLOG("write-back fmt %#x %ux%ux%u levels %u layers %u (%llu bytes) count %d %s", v->d.format, v->w, v->h, v->depth, v->levels, v->layers, (unsigned long long)v->gsize, v->wb_count,
                   v->stage && v->stage_fence && v->stage_fence == v->render_fence ? "staged" : "sync");
    if (sbpb && v->gmem && v->stage && v->stage_fence && v->stage_fence == v->render_fence) {
        { int a_ = runtime_core_away(); mtl_wait(v->stage_fence, UINT64_MAX); runtime_core_back(&a_); }   /* the GPU wait gives the guest core to other workers (nvn_mtl.m wb_wait_gpu, #552) */
        wb_swizzle(v, v->stage_cpu, sbw, sbh, sbpb);
        v->gpu_dirty = 0; g_wb++; g_wb_staged++; g_wb_bytes += v->stage_size; return;
    }
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(v->d.format, &bw, &bh);
    if (!bpb || !v->gmem) return;
    VkDeviceSize total = wb_size(v, bw, bh, bpb); VkBuffer b; VkDeviceMemory m; uint8_t *src;
    host_buffer(total, &b, &m, &src);
    VkCommandBuffer cb = vk_oneshot_begin(); wb_copies(v, cb, b, bw, bh, bpb);
    { int a_ = runtime_core_away();   /* after the image's last rendering only (second queue), else after everything */
      if (!vk_oneshot_after(cb, v->render_fence)) { mtl_finish(); vk_oneshot_end(cb); } runtime_core_back(&a_); }
    wb_swizzle(v, src, bw, bh, bpb);
    vkUnmapMemory(V.dev, m); vkDestroyBuffer(V.dev, b, NULL); vkFreeMemory(V.dev, m, NULL);
    v->gpu_dirty = 0; g_wb++; g_wb_bytes += total;
}
void mtl_finish(void);
void vk_tex_writeback_range(uintptr_t lo, uintptr_t hi) {
    vtex *list[64]; int n = 0;
    pthread_mutex_lock(&g_tex_mu);
    for (int i = 0; i < g_ndirty; i++) { vtex *v = g_dirty[i];
        if (!v->gpu_dirty) { v->in_dirty = 0; g_dirty[i--] = g_dirty[--g_ndirty]; continue; }   /* written back since */
        if (n < 64 && (uintptr_t)v->gmem < hi && (uintptr_t)v->gmem + v->gsize > lo) list[n++] = v; }
    pthread_mutex_unlock(&g_tex_mu);
    if (!n) return;
    for (int i = 0; i < n; i++) writeback(list[i]);
    static uint64_t said; if (g_wb / 1000 != said) { said = g_wb / 1000; VKLOG("write-backs: %llu, %llu from stages (%.1f MB)", (unsigned long long)g_wb, (unsigned long long)g_wb_staged, g_wb_bytes / 1e6); }
}
void vk_tex_readback(vtex *v, const tex_t *t) { (void)t; if (v && v->gpu_dirty) writeback(v); }

static VkComponentSwizzle swz1(int s, VkComponentSwizzle self) {
    switch (s) { case NVN_TEXTURE_SWIZZLE_ZERO: return VK_COMPONENT_SWIZZLE_ZERO; case NVN_TEXTURE_SWIZZLE_ONE: return VK_COMPONENT_SWIZZLE_ONE;
                 case NVN_TEXTURE_SWIZZLE_R: return VK_COMPONENT_SWIZZLE_R; case NVN_TEXTURE_SWIZZLE_G: return VK_COMPONENT_SWIZZLE_G;
                 case NVN_TEXTURE_SWIZZLE_B: return VK_COMPONENT_SWIZZLE_B; case NVN_TEXTURE_SWIZZLE_A: return VK_COMPONENT_SWIZZLE_A; default: return self; }
}
/* the sRGB / linear counterpart of a format, or 0 */
static VkFormat fmt_pair(VkFormat f) {
    switch (f) {
        case VK_FORMAT_R8G8B8A8_UNORM: return VK_FORMAT_R8G8B8A8_SRGB; case VK_FORMAT_R8G8B8A8_SRGB: return VK_FORMAT_R8G8B8A8_UNORM;
        case VK_FORMAT_B8G8R8A8_UNORM: return VK_FORMAT_B8G8R8A8_SRGB; case VK_FORMAT_B8G8R8A8_SRGB: return VK_FORMAT_B8G8R8A8_UNORM;
        case VK_FORMAT_BC1_RGB_UNORM_BLOCK: case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: case VK_FORMAT_BC2_UNORM_BLOCK: case VK_FORMAT_BC3_UNORM_BLOCK: case VK_FORMAT_BC7_UNORM_BLOCK: return (VkFormat)(f + 1);
        case VK_FORMAT_BC1_RGB_SRGB_BLOCK: case VK_FORMAT_BC1_RGBA_SRGB_BLOCK: case VK_FORMAT_BC2_SRGB_BLOCK: case VK_FORMAT_BC3_SRGB_BLOCK: case VK_FORMAT_BC7_SRGB_BLOCK: return (VkFormat)(f - 1);
        default:
            if (f >= VK_FORMAT_ASTC_4x4_UNORM_BLOCK && f <= VK_FORMAT_ASTC_12x12_SRGB_BLOCK) return (VkFormat)((f - VK_FORMAT_ASTC_4x4_UNORM_BLOCK) % 2 ? f - 1 : f + 1);
            return VK_FORMAT_UNDEFINED;
    }
}
/* a view in a format the image was not made for: the image is made again with MUTABLE_FORMAT and its content copied in the
   command stream, outside any pass (nvn_mtl.m tex_upgrade_pfv); views made so far go when the submission completes */
typedef struct { VkImage img; vk_alloc mem; vview *views; int nviews; } old_img;
static void old_destroy(void *p) { old_img *o = p; for (int i = 0; i < o->nviews; i++) vkDestroyImageView(V.dev, o->views[i].view, NULL); free(o->views); vkDestroyImage(V.dev, o->img, NULL); vk_free_mem(&o->mem); free(o); }
static uint64_t g_mutable_upgrades;
static void tex_mutable(vtex *v) {
    void vk_replay_outside_pass(void); vk_replay_outside_pass();
    VkImageCreateInfo ci = v->ci; ci.flags |= VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT; VkImage img; vk_alloc mem;
    if (vkCreateImage(V.dev, &ci, NULL, &img) != VK_SUCCESS) { VKLOG("【红】mutable image %ux%u fmt %d failed", v->w, v->h, v->fmt); v->mutable_all = 1; return; }
    VkMemoryRequirements req; vkGetImageMemoryRequirements(V.dev, img, &req);
    if (!vk_alloc_mem(req, &mem)) VKDIE("out of image memory (%llu bytes)", (unsigned long long)req.size);
    VKCHECK(vkBindImageMemory(V.dev, img, mem.mem, mem.off));
    VkCommandBuffer cb = vk_cb(); VkImageAspectFlags asp = (VkImageAspectFlags)vk_tex_aspect(v);
    VkImageMemoryBarrier2 b = {.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT, .oldLayout = VK_IMAGE_LAYOUT_UNDEFINED,
        .newLayout = VK_IMAGE_LAYOUT_GENERAL, .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED, .image = img,
        .subresourceRange = {asp, 0, VK_REMAINING_MIP_LEVELS, 0, VK_REMAINING_ARRAY_LAYERS}};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .imageMemoryBarrierCount = 1, .pImageMemoryBarriers = &b};
    vkCmdPipelineBarrier2(cb, &di);
    VkImageCopy r[16]; uint32_t n = v->levels < 16 ? v->levels : 16;
    for (uint32_t l = 0; l < n; l++) r[l] = (VkImageCopy){{asp, l, 0, v->layers}, {0, 0, 0}, {asp, l, 0, v->layers}, {0, 0, 0},
        {mipd(v->w, l), v->type == VK_IMAGE_TYPE_1D ? 1 : mipd(v->h, l), v->type == VK_IMAGE_TYPE_3D ? mipd(v->depth, l) : 1}};
    if (v->uploaded || v->gpu_owned) vkCmdCopyImage(cb, v->img, VK_IMAGE_LAYOUT_GENERAL, img, VK_IMAGE_LAYOUT_GENERAL, n, r);
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo d2 = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb}; vkCmdPipelineBarrier2(cb, &d2);
    old_img *o = malloc(sizeof *o); *o = (old_img){v->img, v->mem, v->views, v->nviews}; vk_defer_free(old_destroy, o);
    v->img = img; v->mem = mem; v->views = NULL; v->nviews = 0; v->ci = ci; v->mutable_all = 1; g_mutable_upgrades++;
    static int said; if (said++ < 16) VKLOG("image %ux%u format %d made mutable (a view in another format)", v->w, v->h, v->fmt);
}
VkImageView vk_tex_view(vtex *v, const tex_t *t, const view_t *vw, int has_view, VkImageViewType type, int attach) {
    vview k = {0};
    int fmt = has_view && vw->set_format ? vw->format : t->d.format;
    const vk_fmt *vf = vk_format(fmt); if (!vf) vf = v->vf;
    if (vf->compressed != v->vf->compressed || vf->depth != v->vf->depth) vf = v->vf;   /* not a compatible view: the image's own format */
    k.fmt = vf->vk; k.type = type;
    if (k.fmt != v->fmt && !v->mutable_all && k.fmt != v->pair) tex_mutable(v);
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
    /* a render target in a 3D texture is one depth slice, the view's first layer (nvn_mtl.m: colour attachment depthPlane =
       min_layer): a 2D view of that slice (the image is 2D-array compatible). The 3D colour-grading table of the menu
       backgrounds is drawn into its 8 slices by one pass with 8 targets */
    if (v->type == VK_IMAGE_TYPE_3D && attach) {
        uint32_t dz = v->depth >> k.level0 ? v->depth >> k.level0 : 1; int z = has_view && vw->set_layers ? vw->min_layer : 0;
        k.type = VK_IMAGE_VIEW_TYPE_2D; k.layer0 = z < (int)dz ? z : (int)dz - 1; k.nlayers = 1;
    }
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
        .usage = attach ? (v->vf->depth || v->vf->stencil ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_INPUT_ATTACHMENT_BIT) : VK_IMAGE_USAGE_SAMPLED_BIT};
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
void vk_tex_stats(uint64_t *uploads, uint64_t *bytes, uint64_t *gpu) { *uploads = g_uploads; *bytes = g_upload_bytes; *gpu = g_gpu_uploads; }

/* ---------- buffer textures (NVN_TEXTURE_TARGET_BUFFER): a texel buffer view over the pool memory, read by the GPU at execution
   as on NVN (nvn_mtl.m's texture buffer over the pool buffer) ---------- */
typedef struct bview { uintptr_t mem; VkDeviceSize n; VkFormat fmt; VkBufferView v; struct bview *next; } bview;
static bview *g_bview[256]; static pthread_mutex_t g_bview_mu = PTHREAD_MUTEX_INITIALIZER;
VkBufferView vk_tex_bufview(const tex_t *t, int format) {
    if (t->d.target != NVN_TEXTURE_TARGET_BUFFER || !t->mem) return VK_NULL_HANDLE;
    const vk_fmt *f = vk_format(format); int bw, bh, bpb = nvn_fmt_bpb(format, &bw, &bh);
    if (!f || !bpb) { static int said; if (!said++) VKLOG("【红】buffer texture format %#x has no Vulkan format", format); return VK_NULL_HANDLE; }
    VkDeviceSize n = t->d.width ? t->d.width : 1;
    if (n > V.props.limits.maxTexelBufferElements) { static int said; if (!said++) VKLOG("【红】buffer texture of %llu texels: the first %u are bound", (unsigned long long)n, V.props.limits.maxTexelBufferElements); n = V.props.limits.maxTexelBufferElements; }
    uintptr_t mem = (uintptr_t)t->mem; unsigned h = (unsigned)((mem >> 4) ^ n ^ (unsigned)f->vk) & 255;
    pthread_mutex_lock(&g_bview_mu);
    for (bview *e = g_bview[h]; e; e = e->next) if (e->mem == mem && e->n == n && e->fmt == f->vk) { pthread_mutex_unlock(&g_bview_mu); return e->v; }
    VkBuffer b; VkDeviceSize o; VkBufferView v = VK_NULL_HANDLE;
    if (!vk_pool_find(mem, n * bpb, &b, &o)) { static int said; if (!said++) VKLOG("【红】buffer texture at %#lx is not in a memory pool", (unsigned long)mem); }
    else if (o % V.props.limits.minTexelBufferOffsetAlignment) { static int said; if (!said++) VKLOG("【红】buffer texture at pool offset %#llx: not %llu-byte aligned", (unsigned long long)o, (unsigned long long)V.props.limits.minTexelBufferOffsetAlignment); }
    else {
        VkBufferViewCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_BUFFER_VIEW_CREATE_INFO, .buffer = b, .format = f->vk, .offset = o, .range = n * bpb};
        if (vkCreateBufferView(V.dev, &ci, NULL, &v) != VK_SUCCESS) v = VK_NULL_HANDLE;
        bview *e = malloc(sizeof *e); *e = (bview){mem, n, f->vk, v, g_bview[h]}; g_bview[h] = e;
    }
    pthread_mutex_unlock(&g_bview_mu); return v;
}
static void bview_destroy(void *p) { bview *e = p; if (e->v) vkDestroyBufferView(V.dev, e->v, NULL); free(e); }
static void bview_release_range(uintptr_t lo, uintptr_t hi) {
    pthread_mutex_lock(&g_bview_mu);
    for (int b = 0; b < 256; b++) for (bview **pp = &g_bview[b]; *pp;) { bview *e = *pp; if (e->mem >= lo && e->mem < hi) { *pp = e->next; vk_defer_free(bview_destroy, e); } else pp = &e->next; }
    pthread_mutex_unlock(&g_bview_mu);
}

/* ---------- CPU texel access (nvnTextureReadTexels / WriteTexels, as nvn_mtl.m): a texture the GPU rendered or copied into
   holds its content in the image; any other texture in its pool memory ---------- */
static vtex *tex_find(const tex_t *t) {
    if (!t || !t->serial) return NULL;
    uint64_t key = tex_key(t);
    pthread_mutex_lock(&g_tex_mu); vtex *v = g_tex[key % TEX_HASH]; while (v && v->key != key) v = v->next; pthread_mutex_unlock(&g_tex_mu);
    return v;
}
/* the region of one level through a host buffer: to_image 0 reads the image into lin, 1 writes lin into it */
static void texels_image(vtex *v, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *lin, size_t wb, size_t hb, int nz, int to_image) {
    int is3d = v->type == VK_IMAGE_TYPE_3D; VkDeviceSize n = (VkDeviceSize)wb * hb * nz; VkBuffer b; VkDeviceMemory m; uint8_t *cpu;
    if (!to_image) vk_tex_mark_read(v);
    if (to_image) { int a_ = runtime_core_away(); mtl_finish(); runtime_core_back(&a_); }
    host_buffer(n, &b, &m, &cpu);
    if (to_image) memcpy(cpu, lin, n);
    VkCommandBuffer cb = vk_oneshot_begin();
    VkImageAspectFlags asp = (VkImageAspectFlags)vk_tex_aspect(v); if (asp & VK_IMAGE_ASPECT_DEPTH_BIT) asp = VK_IMAGE_ASPECT_DEPTH_BIT;
    for (int z = 0; z < nz; z++) {
        VkBufferImageCopy c = {.bufferOffset = (VkDeviceSize)z * wb * hb, .imageSubresource = {asp, level, is3d ? 0 : layer0 + r->zoffset + z, 1},
            .imageOffset = {r->xoffset, r->yoffset, is3d ? r->zoffset + z : 0}, .imageExtent = {(uint32_t)r->width, (uint32_t)(r->height ? r->height : 1), 1}};
        if (to_image) vkCmdCopyBufferToImage(cb, b, v->img, VK_IMAGE_LAYOUT_GENERAL, 1, &c); else vkCmdCopyImageToBuffer(cb, v->img, VK_IMAGE_LAYOUT_GENERAL, b, 1, &c);
    }
    { int a_ = runtime_core_away(); if (to_image || !vk_oneshot_after(cb, v->render_fence)) { if (!to_image) mtl_finish(); vk_oneshot_end(cb); } runtime_core_back(&a_); }
    if (!to_image) memcpy(lin, cpu, n);
    vkUnmapMemory(V.dev, m); vkDestroyBuffer(V.dev, b, NULL); vkFreeMemory(V.dev, m, NULL);
}
/* the region of one level in pool memory: write 0 reads it into dst rows, 1 writes src rows into it (swizzled back) */
static void texels_memory(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *rows, size_t row, size_t img, int write) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh), is3d = t->d.target == NVN_TEXTURE_TARGET_3D, nz = r->depth ? r->depth : 1;
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (r->height + bh - 1) / bh, x0 = (size_t)(r->xoffset / bw) * bpb, y0 = r->yoffset / bh;
    uint32_t lw = mipd(t->d.width, level), lh = mipd(t->d.height, level), lz = is3d ? mipd(t->d.depth, level) : 1;
    size_t lp = (size_t)((lw + bw - 1) / bw) * bpb, lr = (lh + bh - 1) / bh; uint8_t *lin = malloc(lp * lr * lz);
    for (int z = 0; z < nz; z++) {
        unsigned layer = is3d ? 0 : layer0 + r->zoffset + z;
        if ((!is3d || !z) && nvn_deswizzle(&t->d, level, layer, t->mem, lin, lp)) { static int said; if (!said++) VKLOG("【红】texels: no layout path for format %#x", t->d.format); break; }
        uint8_t *at = lin + (is3d ? (size_t)(r->zoffset + z) * lp * lr : 0);
        for (size_t y = 0; y < hb; y++) { if (write) memcpy(at + (y0 + y) * lp + x0, rows + z * img + y * row, wb); else memcpy(rows + z * img + y * row, at + (y0 + y) * lp + x0, wb); }
        if (write && (!is3d || z == nz - 1)) nvn_swizzle(&t->d, level, layer, lin, lp, t->mem);
    }
    free(lin);
}
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh); if (!bpb || !r->width || !r->height) return;
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (r->height + bh - 1) / bh; int nz = r->depth ? r->depth : 1;
    vtex *v = tex_find(t);
    if (v && v->gpu_owned) {
        uint8_t *lin = malloc(wb * hb * nz); texels_image(v, level, layer0, r, lin, wb, hb, nz, 0);
        for (int z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memcpy(dst + z * img + y * row, lin + (z * hb + y) * wb, wb);
        free(lin); return;
    }
    if (t->mem) texels_memory(t, level, layer0, r, dst, row, img, 0);
    else for (int z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memset(dst + z * img + y * row, 0, wb);
}
void mtl_write_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) {
    int bw, bh, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh); if (!bpb || !r->width || !r->height) return;
    size_t wb = (size_t)((r->width + bw - 1) / bw) * bpb, hb = (r->height + bh - 1) / bh; int nz = r->depth ? r->depth : 1;
    vtex *v = tex_find(t); int gpu = v && v->gpu_owned;
    if (t->mem) {   /* the pool bytes: a CPU write (re-uploaded on use), or in step with the image the GPU owns */
        void *tok = gpu ? nvn_dirty_gpu_write_begin(t->mem, t->L.size) : NULL;
        if (!gpu) dm_guest_write_prepare(t->mem, t->L.size);
        texels_memory(t, level, layer0, r, (uint8_t *)src, row, img, 1);
        if (gpu) nvn_dirty_gpu_write_end(tok);
    }
    if (!gpu) return;
    uint8_t *lin = malloc(wb * hb * nz);
    for (int z = 0; z < nz; z++) for (size_t y = 0; y < hb; y++) memcpy(lin + (z * hb + y) * wb, src + z * img + y * row, wb);
    texels_image(v, level, layer0, r, lin, wb, hb, nz, 1); free(lin);
}

/* ---------- store pruning: an image nothing has read (sampled, loaded by a later pass, copied from, read back by the CPU or
   presented) for PRUNE_FRAMES frames since it was made is rendered with STORE_OP_DONT_CARE. A read of a pruned image keeps it
   stored from then on (logged: that read saw the last stored content) ---------- */
#define PRUNE_FRAMES 300
static uint64_t g_frame; static uint64_t g_pruned_reads, g_pruned_stores;
void vk_tex_frame(void) { g_frame++; }
void vk_tex_mark_read(vtex *v) {
    if (!v) return;
    if (v->pruned && !v->store_always) { v->store_always = 1; g_pruned_reads++; if (g_pruned_reads <= 16) VKLOG("【红】a pruned image was read (%ux%u format %d): stored from now on", v->w, v->h, v->fmt); }
    v->last_read = g_frame + 1;
}
int vk_tex_store_needed(vtex *v) {
    static int on = -1; if (on < 0) { const char *e = getenv("TOTK_VK_PRUNE"); on = e && !strcmp(e, "1"); }   /* opt-in: ~1 attachment a frame, and an image read only rarely saw its last stored content */
    if (!on || !v || v->store_always || v->last_read) return 1;
    if (!v->made) v->made = g_frame + 1;
    if (g_frame + 1 - v->made < PRUNE_FRAMES) return 1;
    v->pruned++; g_pruned_stores++; return 0;
}
void vk_tex_prune_stats(uint64_t *stores, uint64_t *reads) { *stores = g_pruned_stores; *reads = g_pruned_reads; }
/* the VkSampler of a sampler pool id, or VK_NULL_HANDLE when the id holds none */
VkSampler vk_sampler_id(int id) {
    if (id <= 0 || id >= 4096) return VK_NULL_HANDLE;
    VkSampler s = atomic_load_explicit(&g_samp_of[id], memory_order_relaxed); if (s) return s;
    samp_par p; if (!vk_samp_registry_get(id, &p)) return VK_NULL_HANDLE;
    s = vk_sampler(&p); atomic_store_explicit(&g_samp_of[id], s, memory_order_relaxed); return s;
}
int vk_tex_is3d(vtex *v) { return v && v->type == VK_IMAGE_TYPE_3D; }
