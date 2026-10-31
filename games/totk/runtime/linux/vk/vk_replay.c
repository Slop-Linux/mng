/* Replay of the recorded NVN command stream (nvn_obj.h ops) into the open Vulkan submission. One SetRenderTargets run is
   one dynamic-rendering pass: the pass opens at its first draw or clear and stays open across state changes and clears,
   until the targets change or a copy, dispatch, texture clear, texture-invalidating barrier or the end of the submission
   needs it closed. Draws use shader objects with all state dynamic; descriptors come from per-submission pools. */
#include "vk.h"
#include "../../../third_party/d2r_gfx/host/front.h"

uint64_t vk_sub_submit(void);
void vk_sub_open(void);

/* ---------- state ---------- */
typedef struct {
    p_set_rt rt; int has_rt;
    float vp[16][6]; int nvp; p_scissor sc; int has_sc; float dr[2];
    const prog_t *prog;
    blend_st blend[8]; color_st color; chmask_st chmask; ms_st ms; poly_st poly; ds_st ds;
    uint32_t attr[16]; int nattr; vs_st stream[16]; int nstream; NVNbufferRange vbuf[16];
    p_bind_buf ubo[6][16], ssbo[6][16]; uint64_t tex[6][128], samp[6][128], img[6][32];
    int st_vmask[2], st_wmask[2], st_ref[2]; p_poly_offset po; float blend_color[4];
    int depth_clamp, render_disabled, raster_discard; int64_t copy_stride;
} state_t;
static state_t S;
static void state_reset(void) {
    memset(&S, 0, sizeof S);
    for (int i = 0; i < 8; i++) S.blend[i] = (blend_st){i, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_FUNC_ONE, NVN_BLEND_FUNC_ZERO, NVN_BLEND_EQUATION_ADD, NVN_BLEND_EQUATION_ADD};
    S.color = (color_st){0, NVN_LOGIC_OP_COPY, NVN_ALPHA_FUNC_ALWAYS}; S.chmask.mask = 0xffffffffu;
    S.poly = (poly_st){NVN_FACE_NONE, NVN_FRONT_FACE_CCW, NVN_POLYGON_MODE_FILL, 0};
    S.ds = (ds_st){0, 1, NVN_DEPTH_FUNC_LESS, 0, NVN_STENCIL_FUNC_ALWAYS, NVN_STENCIL_FUNC_ALWAYS, 1, 1, 1, 1, 1, 1};
    S.st_vmask[0] = S.st_vmask[1] = S.st_wmask[0] = S.st_wmask[1] = 0xff; S.dr[1] = 1;
}

/* ---------- statistics ---------- */
static uint64_t g_passes, g_draws, g_dispatches, g_copies, g_skips[8], g_frame_passes, g_frame_draws;

/* ---------- the pass ---------- */
typedef struct { vtex *v; VkImageView view; int level, layer; } att_t;
static int g_pass_open; static att_t g_patt[8], g_pdepth; static int g_pslot[8]; static uint32_t g_pw, g_ph; static int g_pcount;
static int g_dyn_set;   /* every dynamic state set once in this command buffer */
static int g_fb_run;    /* the pass was split for the previous draw, which also sampled its targets: a run of such draws shares that one split */
static int depth_has_stencil(void) { return g_pdepth.v && (vk_tex_aspect(g_pdepth.v) & VK_IMAGE_ASPECT_STENCIL_BIT); }
/* why passes end: 0 submit, 1 targets, 2 buffer copy, 3 texture copy, 4 dispatch, 5 barrier, 6 texture clear, 7 feedback, 8 uniform update flush */
static uint64_t g_end_why[9];
static void end_pass(int why) {
    if (!g_pass_open) return;
    g_end_why[why]++;
    vkCmdEndRendering(vk_cb()); g_pass_open = 0;
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(vk_cb(), &di);
}
void vk_replay_end_pass(void) { end_pass(0); }
static void full_barrier(void) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(vk_cb(), &di);
}
static int pass_begin(void) {
    if (g_pass_open) return 1;
    if (!S.has_rt) { g_skips[0]++; return 0; }
    VkRenderingAttachmentInfo ca[8] = {0}, da = {0}, sa = {0}; int n = 0; uint32_t w = UINT32_MAX, h = UINT32_MAX;
    memset(g_patt, 0, sizeof g_patt); memset(&g_pdepth, 0, sizeof g_pdepth);
    for (int i = 0; i < S.rt.n && i < 8; i++) {
        g_pslot[i] = -1;
        const tex_t *t = S.rt.color[i];
        if (!t) continue;
#ifdef GAME_BOTW
        if (rt_dup_of(&S.rt, i) >= 0) continue;   /* the same image at an earlier slot (nvn_obj.h) */
#endif
        vtex *v = vk_tex_get(t); if (!v) continue;
        VkImageView view = vk_tex_view(v, t, &S.rt.cview[i], S.rt.has_cview[i], VK_IMAGE_VIEW_TYPE_2D, 1); if (!view) continue;
        uint32_t tw, th; VkFormat f; vk_tex_dims(v, &tw, &th, &f);
        int lv = S.rt.has_cview[i] && S.rt.cview[i].set_levels ? S.rt.cview[i].base_level : 0; tw = tw >> lv ? tw >> lv : 1; th = th >> lv ? th >> lv : 1;
        if (tw < w) w = tw; if (th < h) h = th;
        vk_tex_upload_if_stale(v, t); vk_tex_gpu_wrote(v);
        g_patt[i] = (att_t){v, view, lv, S.rt.has_cview[i] && S.rt.cview[i].set_layers ? S.rt.cview[i].min_layer : 0}; g_pslot[i] = i;
        ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
            .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
        n = i + 1;
    }
    for (int i = 0; i < n; i++) if (!ca[i].sType) ca[i] = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    if (S.rt.depth) {
        vtex *v = vk_tex_get(S.rt.depth);
        VkImageView view = v ? vk_tex_view(v, S.rt.depth, &S.rt.dview, S.rt.has_dview, VK_IMAGE_VIEW_TYPE_2D, 1) : VK_NULL_HANDLE;
        if (view) {
            uint32_t tw, th; VkFormat f; vk_tex_dims(v, &tw, &th, &f); if (tw < w) w = tw; if (th < h) h = th;
            vk_tex_gpu_wrote(v); g_pdepth = (att_t){v, view, S.rt.has_dview && S.rt.dview.set_levels ? S.rt.dview.base_level : 0, S.rt.has_dview && S.rt.dview.set_layers ? S.rt.dview.min_layer : 0};
            da = (VkRenderingAttachmentInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO, .imageView = view, .imageLayout = VK_IMAGE_LAYOUT_GENERAL,
                .loadOp = VK_ATTACHMENT_LOAD_OP_LOAD, .storeOp = VK_ATTACHMENT_STORE_OP_STORE};
            if (vk_tex_aspect(v) & VK_IMAGE_ASPECT_STENCIL_BIT) sa = da;
            if (!(vk_tex_aspect(v) & VK_IMAGE_ASPECT_DEPTH_BIT)) da = (VkRenderingAttachmentInfo){0};
        }
    }
    if (w == UINT32_MAX) {   /* no attachment: the viewport's extent */
        w = S.nvp ? (uint32_t)(S.vp[0][0] + S.vp[0][2]) : 1; h = S.nvp ? (uint32_t)(S.vp[0][1] + S.vp[0][3]) : 1; if (!w) w = 1; if (!h) h = 1;
    }
    g_pw = w; g_ph = h; g_pcount = n;
    VkRenderingInfo ri = {.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1, .colorAttachmentCount = (uint32_t)n, .pColorAttachments = ca,
        .pDepthAttachment = da.sType ? &da : NULL, .pStencilAttachment = sa.sType ? &sa : NULL};
    vkCmdBeginRendering(vk_cb(), &ri);
    g_pass_open = 1; g_passes++; g_frame_passes++; g_fb_run = 0;
    return 1;
}

/* ---------- inline uniform updates in GPU order ---------- */
typedef struct { uint64_t dst, n; VkBuffer src; VkDeviceSize soff; const uint8_t *data; } upd_t;
static upd_t *g_upd; static int g_nupd, g_capupd;
static void upd_add(uint64_t dst, const uint8_t *data, uint64_t n) {
    vk_span s = vk_ring(n, 16); memcpy(s.cpu, data, n);
    if (g_nupd == g_capupd) { g_capupd = g_capupd ? g_capupd * 2 : 256; g_upd = realloc(g_upd, g_capupd * sizeof *g_upd); }
    g_upd[g_nupd++] = (upd_t){dst, n, s.buf, s.off, s.cpu};
}
/* the updates land in memory at the end of the submission (after every draw that used a renamed copy) */
static void upd_flush(void) {
    if (!g_nupd) return;
    end_pass(8);
    for (int i = 0; i < g_nupd; i++) {
        VkBuffer b; VkDeviceSize o;
        if (!vk_pool_find(g_upd[i].dst, g_upd[i].n, &b, &o)) continue;
        VkBufferCopy c = {g_upd[i].soff, o, g_upd[i].n}; vkCmdCopyBuffer(vk_cb(), g_upd[i].src, b, 1, &c);
    }
    full_barrier(); g_nupd = 0;
}
/* renamed copies made in this submission, by range and the number of updates they include */
typedef struct { uint64_t addr, size; int nupd; VkBuffer buf; VkDeviceSize off; } shadow_t;
static shadow_t g_shadow[1024]; static uint64_t g_shadow_epoch, g_shadow_ep[1024];
/* a uniform range as the GPU would read it at this point: memory plus the pending updates that overlap it */
static int ubo_range(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off, VkDeviceSize *range) {
    if (!addr) return 0;
    if (!size || size > 65536) size = 65536;
    int overlap = 0;
    for (int i = 0; i < g_nupd && !overlap; i++) overlap = g_upd[i].dst < addr + size && addr < g_upd[i].dst + g_upd[i].n;
    VkBuffer b; VkDeviceSize o;
    if (!overlap && vk_pool_find(addr, size, &b, &o) && !(o & 255)) { *buf = b; *off = o; *range = size; return 1; }
    if (!vk_pool_find(addr, 1, &b, &o)) return 0;
    unsigned slot = (unsigned)((addr >> 8) ^ size) & 1023;
    shadow_t *sh = &g_shadow[slot];
    if (g_shadow_ep[slot] == g_shadow_epoch && sh->addr == addr && sh->size == size && sh->nupd == g_nupd) { *buf = sh->buf; *off = sh->off; *range = size; return 1; }
    vk_span s = vk_ring(size, 256); memcpy(s.cpu, (const void *)(uintptr_t)addr, size);
    for (int i = 0; i < g_nupd; i++) {
        uint64_t lo = g_upd[i].dst > addr ? g_upd[i].dst : addr, hi = g_upd[i].dst + g_upd[i].n < addr + size ? g_upd[i].dst + g_upd[i].n : addr + size;
        if (lo < hi) memcpy(s.cpu + (lo - addr), g_upd[i].data + (lo - g_upd[i].dst), hi - lo);
    }
    *buf = s.buf; *off = s.off; *range = size;
    *sh = (shadow_t){addr, size, g_nupd, s.buf, s.off}; g_shadow_ep[slot] = g_shadow_epoch;
    return 1;
}

/* ---------- shader pairs and descriptors ---------- */
typedef struct pair_t { vshader *vs, *fs; VkShaderEXT sh[2]; VkPipelineLayout pl; struct pair_t *next; } pair_t;
static pair_t *g_pairs[1024];
typedef struct comp_t { vshader *cs; VkShaderEXT sh; VkPipelineLayout pl; struct comp_t *next; } comp_t;
static comp_t *g_comps[256];
static VkShaderEXT make_shader(vshader *s, VkShaderStageFlagBits stage, VkShaderStageFlags next, VkDescriptorSetLayout *layouts, int nl) {
    VkShaderCreateInfoEXT ci = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT, .stage = stage, .nextStage = next, .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
        .codeSize = s->nwords * 4, .pCode = s->spv, .pName = "main", .setLayoutCount = (uint32_t)nl, .pSetLayouts = layouts};
    VkShaderEXT o; VkResult r = V.CreateShadersEXT(V.dev, 1, &ci, NULL, &o);
    if (r != VK_SUCCESS) { VKLOG("【红】shader %s (stage %d) did not compile: %d", s->key, s->stage, r); return VK_NULL_HANDLE; }
    return o;
}
static pair_t *pair_get(vshader *vs, vshader *fs) {
    unsigned h = (unsigned)(((uintptr_t)vs >> 4) ^ ((uintptr_t)fs >> 2)) & 1023;
    for (pair_t *p = g_pairs[h]; p; p = p->next) if (p->vs == vs && p->fs == fs) return p;
    pair_t *p = calloc(1, sizeof *p); p->vs = vs; p->fs = fs;
    VkDescriptorSetLayout l[2] = {vs->layout, fs ? fs->layout : V.empty_layout};
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 2, .pSetLayouts = l};
    VKCHECK(vkCreatePipelineLayout(V.dev, &pli, NULL, &p->pl));
    p->sh[0] = make_shader(vs, VK_SHADER_STAGE_VERTEX_BIT, fs ? VK_SHADER_STAGE_FRAGMENT_BIT : 0, l, 2);
    if (fs) p->sh[1] = make_shader(fs, VK_SHADER_STAGE_FRAGMENT_BIT, 0, l, 2);
    p->next = g_pairs[h]; g_pairs[h] = p; return p;
}
static comp_t *comp_get(vshader *cs) {
    unsigned h = (unsigned)((uintptr_t)cs >> 4) & 255;
    for (comp_t *c = g_comps[h]; c; c = c->next) if (c->cs == cs) return c;
    comp_t *c = calloc(1, sizeof *c); c->cs = cs;
    VkPipelineLayoutCreateInfo pli = {.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO, .setLayoutCount = 1, .pSetLayouts = &cs->layout};
    VKCHECK(vkCreatePipelineLayout(V.dev, &pli, NULL, &c->pl));
    c->sh = make_shader(cs, VK_SHADER_STAGE_COMPUTE_BIT, 0, &cs->layout, 1);
    c->next = g_comps[h]; g_comps[h] = c; return c;
}
/* descriptor pools: taken by a submission, reset when it completes */
static VkDescriptorPool *g_dpool_free; static int g_ndpool_free; static VkDescriptorPool g_dpool; static pthread_mutex_t g_dpool_mu = PTHREAD_MUTEX_INITIALIZER;
static void dpool_recycle(void *p) {
    VkDescriptorPool d = (VkDescriptorPool)p; vkResetDescriptorPool(V.dev, d, 0);
    pthread_mutex_lock(&g_dpool_mu); g_dpool_free = realloc(g_dpool_free, (g_ndpool_free + 1) * sizeof *g_dpool_free); g_dpool_free[g_ndpool_free++] = d; pthread_mutex_unlock(&g_dpool_mu);
}
static VkDescriptorPool dpool_new(void) {
    pthread_mutex_lock(&g_dpool_mu);
    if (g_ndpool_free) { VkDescriptorPool d = g_dpool_free[--g_ndpool_free]; pthread_mutex_unlock(&g_dpool_mu); return d; }
    pthread_mutex_unlock(&g_dpool_mu);
    VkDescriptorPoolSize sz[] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 32768}, {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 8192}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 32768},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2048}, {VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER, 2048}, {VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER, 1024}};
    VkDescriptorPoolCreateInfo ci = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO, .maxSets = 8192, .poolSizeCount = sizeof sz / sizeof *sz, .pPoolSizes = sz};
    VkDescriptorPool d; VKCHECK(vkCreateDescriptorPool(V.dev, &ci, NULL, &d)); return d;
}
static VkDescriptorSet dset_alloc(VkDescriptorSetLayout l) {
    for (int tries = 0; tries < 2; tries++) {
        if (!g_dpool) { g_dpool = dpool_new(); vk_defer_free(dpool_recycle, (void *)g_dpool); }
        VkDescriptorSetAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO, .descriptorPool = g_dpool, .descriptorSetCount = 1, .pSetLayouts = &l};
        VkDescriptorSet s; if (vkAllocateDescriptorSets(V.dev, &ai, &s) == VK_SUCCESS) return s;
        g_dpool = VK_NULL_HANDLE;
    }
    VKDIE("descriptor set allocation");
}
static VkBuffer g_support; static VkDeviceMemory g_support_mem;
static void support_init(void) {
    if (g_support) return;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 4096, .usage = VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT};
    VKCHECK(vkCreateBuffer(V.dev, &bi, NULL, &g_support)); VkMemoryRequirements req; vkGetBufferMemoryRequirements(V.dev, g_support, &req);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = req.size, .memoryTypeIndex = vk_memtype(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VKCHECK(vkAllocateMemory(V.dev, &ai, NULL, &g_support_mem)); VKCHECK(vkBindBufferMemory(V.dev, g_support, g_support_mem, 0));
    uint8_t *m; VKCHECK(vkMapMemory(V.dev, g_support_mem, 0, VK_WHOLE_SIZE, 0, (void **)&m)); memset(m, 0, 4096);
    for (int i = 0; i < 73; i++) *(float *)(m + 192 + 16 * i) = 1.0f;   /* render_scale: 1 (alpha_test 0, is_bgra 16, viewport_inverse 144, viewport_size 160, frag_scale_count 176) */
}
/* NVN driver constant segments: c0 storage-buffer table (vertex 0x110, fragment 0x510, compute 0x310), compute group counts 0x200
   and uniform table 0x210, vertex point-size limit 0x610; c2 combined handles 0x20, texture handles 0x168, sampler handles 0x568 */
static uint32_t g_grid[3];
static void driver_bank(uint8_t *d, int bank, int stage) {
    memset(d, 0, 0xA00);
    if (bank == 0) {
        int base = stage == 0 ? 0x110 : stage == 5 ? 0x310 : 0x510;
        for (int r = 0; r < 16; r++) { uint64_t a = S.ssbo[stage][r].addr; uint32_t n = (uint32_t)S.ssbo[stage][r].size; memcpy(d + base + 16 * r, &a, 8); memcpy(d + base + 16 * r + 8, &n, 4); }
        if (stage == 5) { memcpy(d + 0x200, g_grid, 12); for (int r = 0; r < 16; r++) { uint64_t a = S.ubo[5][r].addr; uint32_t n = (uint32_t)S.ubo[5][r].size; memcpy(d + 0x210 + 16 * r, &a, 8); memcpy(d + 0x210 + 16 * r + 8, &n, 4); } }
        else if (stage == 0) { const float ps = 2047.9375f; memcpy(d + 0x610, &ps, 4); }
    } else {
        for (int r = 0; r < 128; r++) { uint32_t th = (uint32_t)S.tex[stage][r], sh = (uint32_t)S.samp[stage][r]; memcpy(d + 0x168 + 8 * r, &th, 4); memcpy(d + 0x568 + 8 * r, &sh, 4); }
        for (int r = 0; r < 32; r++) { uint32_t th = (uint32_t)S.tex[stage][r]; memcpy(d + 0x20 + 8 * r, &th, 4); }
    }
}
/* the NVN handle a texture binding reads: Ryujinx TextureHandle.ReadPackedId over the c2 bank */
static uint32_t packed_handle(const uint8_t *c2, int handle) {
    int tw = handle & 0x3fff, sw = (handle >> 14) & 0x3fff, type = (unsigned)handle >> 28;
    uint32_t h = 0; if (tw * 4 + 4 <= 0xA00) memcpy(&h, c2 + tw * 4, 4);
    if (type) {
        uint32_t s = 0;
        if (type != 3) { if (sw * 4 + 4 <= 0xA00) memcpy(&s, c2 + sw * 4, 4); } else s = (uint32_t)sw;
        if (type == 2 || type == 3) s <<= 20;
        h |= s;
    }
    return h;
}
static VkImageViewType view_type(int stype) {
    int arr = (stype & 0x100) != 0;
    switch (stype & 0xff) {
        case 1: return arr ? VK_IMAGE_VIEW_TYPE_1D_ARRAY : VK_IMAGE_VIEW_TYPE_1D;
        case 4: return VK_IMAGE_VIEW_TYPE_3D;
        case 5: return arr ? VK_IMAGE_VIEW_TYPE_CUBE_ARRAY : VK_IMAGE_VIEW_TYPE_CUBE;
        default: return arr ? VK_IMAGE_VIEW_TYPE_2D_ARRAY : VK_IMAGE_VIEW_TYPE_2D;
    }
}
static samp_par g_default_samp = {.minf = 1, .magf = 1, .ws = 7, .wt = 7, .wr = 7, .lod_max = 1000, .aniso = 1};
static int g_feedback;   /* a draw samples an attachment of the open pass */
static int g_fb_run;     /* the pass was split for the previous draw, which also sampled its targets: a run shares that one split */
#define MAXW 160
static VkDescriptorSet stage_set(vshader *s, int stage, int set, VkDescriptorSetLayout layout) {
    VkWriteDescriptorSet w[MAXW]; VkDescriptorBufferInfo bi[MAXW]; VkDescriptorImageInfo ii[MAXW]; int nw = 0, nb = 0, ni = 0;
    VkDescriptorSet ds = dset_alloc(layout);
    uint8_t c2[0xA00]; int have_c2 = 0;
    if (set == 0) { bi[nb] = (VkDescriptorBufferInfo){g_support, 0, 4096}; w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[nb++]}; }
    for (int i = 0; i < s->nb && nw < MAXW - 1; i++) {
        const vk_rbind *b = &s->b[i];
        if (b->set != set) continue;
        VkWriteDescriptorSet x = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding, .descriptorCount = 1};
        if (b->kind == RB_CBUF) {
            VkBuffer buf = VK_NULL_HANDLE; VkDeviceSize off = 0, range = VK_WHOLE_SIZE;
            if (b->slot == 1) { buf = s->c1buf; range = VK_WHOLE_SIZE; }
            else if (b->slot == 0 || b->slot == 2) { vk_span sp = vk_ring(0xA00, 256); driver_bank(sp.cpu, b->slot, stage); if (b->slot == 2) { memcpy(c2, sp.cpu, 0xA00); have_c2 = 1; } buf = sp.buf; off = sp.off; range = 0xA00; }
            else if (b->slot >= 3 && b->slot <= 18) { const p_bind_buf *u = &S.ubo[stage][b->slot - 3]; if (!ubo_range(u->addr, u->size, &buf, &off, &range)) buf = VK_NULL_HANDLE; }
            bi[nb] = (VkDescriptorBufferInfo){buf, off, buf ? range : VK_WHOLE_SIZE};
            x.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER; x.pBufferInfo = &bi[nb++];
        } else if (b->kind == RB_SBUF) {
            uint64_t addr = 0, size = 0; int off = b->cb_off * 4;
            if (b->cb_slot == 0) {
                int base = stage == 0 ? 0x110 : stage == 5 ? 0x310 : 0x510;
                if (stage == 5 && off >= 0x210 && off < 0x310) { const p_bind_buf *u = &S.ubo[5][(off - 0x210) / 16]; addr = u->addr; size = u->size; }
                else if (off >= base && off < base + 0x100) { const p_bind_buf *u = &S.ssbo[stage][(off - base) / 16]; addr = u->addr; size = u->size; }
            } else if (b->cb_slot >= 3 && b->cb_slot <= 18) {
                const p_bind_buf *u = &S.ubo[stage][b->cb_slot - 3];
                if (u->addr && u->size >= (uint64_t)off + 8) memcpy(&addr, (const void *)(uintptr_t)(u->addr + off), 8);
            }
            VkBuffer buf = VK_NULL_HANDLE; VkDeviceSize o = 0, range = VK_WHOLE_SIZE;
            if (addr) {
                uint64_t lo = addr & ~255ull; VkBuffer pb; VkDeviceSize po;
                if (vk_pool_find(lo, 1, &pb, &po)) { buf = pb; o = po; range = size ? (addr - lo) + size : VK_WHOLE_SIZE; }
            }
            bi[nb] = (VkDescriptorBufferInfo){buf, o, range};
            x.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; x.pBufferInfo = &bi[nb++];
            if (buf && (b->flags & 1)) { /* written by the shader */ }
        } else {
            int is_img = b->kind == RB_IMG;
            if ((b->stype & 0xff) == 2) {   /* texel buffers: not yet */
                static int said; if (!said++) VKLOG("【红】texture / image buffer bindings are not implemented (shader %s)", s->key);
                x.descriptorType = is_img ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
                static VkBufferView none = VK_NULL_HANDLE; x.pTexelBufferView = &none; w[nw++] = x; continue;
            }
            if (!have_c2) { driver_bank(c2, 2, stage); have_c2 = 1; }
            x.descriptorType = is_img ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            int count = b->array; if (ni + count > MAXW) count = MAXW - ni;
            x.descriptorCount = (uint32_t)count; x.pImageInfo = &ii[ni];
            for (int k = 0; k < count; k++) {
                uint32_t h;
                if (is_img) { int tw = b->handle & 0x3fff, reg = tw * 4 >= 0x120 ? (tw * 4 - 0x120) / 8 : -1; h = reg >= 0 && reg < 32 ? (uint32_t)S.img[stage][reg] : 0; }
                else h = packed_handle(c2, b->handle + k * 2);
                int tid = (int)(h & 0xfffff), sid = (int)(h >> 20) & 0xfff;
                tex_t t; view_t vw; int hv; VkImageView view = VK_NULL_HANDLE; VkSampler smp = vk_sampler(&g_default_samp);
                if (tid && vk_tex_registry_get(tid, &t, &vw, &hv)) {
                    vtex *v = vk_tex_get(&t);
                    if (v) {
                        vk_tex_upload_if_stale(v, &t);
                        if (is_img) vk_tex_gpu_wrote(v);
                        view = vk_tex_view(v, &t, &vw, hv, view_type(b->stype), 0);
                        /* the sampled levels / layers include an attachment's own: the pass must land first */
                        int l0 = hv && vw.set_levels ? vw.base_level : 0, nl = hv && vw.set_levels && vw.nlevels ? vw.nlevels : 64;
                        int y0 = hv && vw.set_layers ? vw.min_layer : 0, ny = hv && vw.set_layers && vw.nlayers ? vw.nlayers : 4096;
                        for (int a = 0; a < 9; a++) { const att_t *at = a < 8 ? &g_patt[a] : &g_pdepth;
                            if (at->v == v && at->level >= l0 && at->level < l0 + nl && at->layer >= y0 && at->layer < y0 + ny) g_feedback = 1; }
                    }
                }
                samp_par sp; if (!is_img && sid && vk_samp_registry_get(sid, &sp)) smp = vk_sampler(&sp);
                ii[ni++] = (VkDescriptorImageInfo){is_img ? VK_NULL_HANDLE : smp, view, VK_IMAGE_LAYOUT_GENERAL};
            }
        }
        w[nw++] = x;
    }
    vkUpdateDescriptorSets(V.dev, (uint32_t)nw, w, 0, NULL);
    return ds;
}

/* ---------- programs: translation answers from the bound state ---------- */
static const char *fmt_class(int f) {
    switch (f) {
        case 0x03: case 0x08: case 0x0B: case 0x0F: case 0x14: case 0x17: case 0x1B: case 0x20: case 0x23: case 0x27: case 0x2C: case 0x2F: case 0x3E: case 0x6F: case 0x74: case 0x77: return "uint";
        case 0x04: case 0x09: case 0x0C: case 0x10: case 0x15: case 0x18: case 0x1C: case 0x21: case 0x24: case 0x28: case 0x2D: case 0x30: case 0x6A: case 0x70: case 0x75: case 0x78: return "sint";
        default: return "float";
    }
}
static const char *target_name(int t) {
    switch (t) { case 0: return "1d"; case 2: return "3d"; case 3: return "1darray"; case 4: return "2darray"; case 5: return "2dms";
                 case 6: return "2dmsarray"; case 8: return "cube"; case 9: return "cubearray"; case 10: return "buffer"; default: return "2d"; }
}
static const char *image_format(int f) {
    switch (f) {
        case 0x01: return "R8Unorm"; case 0x0D: return "R8G8Unorm"; case 0x25: return "R8G8B8A8Unorm"; case 0x27: return "R8G8B8A8Uint";
        case 0x05: return "R16Float"; case 0x11: return "R16G16Float"; case 0x29: return "R16G16B16A16Float"; case 0x08: return "R16Uint"; case 0x2C: return "R16G16B16A16Uint";
        case 0x0A: return "R32Float"; case 0x16: return "R32G32Float"; case 0x2E: return "R32G32B32A32Float"; case 0x0B: return "R32Uint"; case 0x0C: return "R32Sint";
        case 0x17: return "R32G32Uint"; case 0x2F: return "R32G32B32A32Uint"; case 0x3D: return "R10G10B10A2Unorm"; case 0x3F: return "R11G11B10Float";
        default: return NULL;
    }
}
static void answers(int st, char *out, size_t cap) {
    size_t o = 0; int first;
#define PUT(...) do { int k_ = snprintf(out + o, o < cap ? cap - o : 0, __VA_ARGS__); if (k_ > 0) o += (size_t)k_; } while (0)
    tex_t t; view_t v; int hv;
    PUT("\"tex\": {"); first = 1;
    for (int r = 0; r < 128; r++) if (S.tex[st][r] && vk_tex_registry_get((int)(S.tex[st][r] & 0xfffff), &t, &v, &hv)) {
        int tg = hv && v.set_target ? v.target : t.d.target; PUT("%s\"%d\": \"%s\"", first ? "" : ", ", r, target_name(tg)); first = 0; }
    PUT("}, \"img\": {"); first = 1;
    for (int r = 0; r < 32; r++) if (S.img[st][r] && vk_tex_registry_get((int)(S.img[st][r] & 0xfffff), &t, &v, &hv)) {
        int tg = hv && v.set_target ? v.target : t.d.target, f = hv && v.set_format ? v.format : t.d.format; const char *fn = image_format(f);
        if (fn) PUT("%s\"%d\": {\"type\": \"%s\", \"fmt\": \"%s\"}", first ? "" : ", ", r, target_name(tg), fn); else PUT("%s\"%d\": {\"type\": \"%s\"}", first ? "" : ", ", r, target_name(tg));
        first = 0; }
    PUT("}, \"attr\": {"); first = 1;
    if (st == 0) for (int i = 0; i < S.nattr && i < 16; i++) { PUT("%s\"%d\": \"%s\"", first ? "" : ", ", i, fmt_class(((va_st *)&S.attr[i])->fmt)); first = 0; }
    PUT("}, \"fout\": {"); first = 1;
    if (st == 1 && S.has_rt) for (int i = 0; i < S.rt.n && i < 8; i++) if (S.rt.color[i]) {
        PUT("%s\"%d\": \"%s\"", first ? "" : ", ", i, fmt_class(S.rt.has_cview[i] && S.rt.cview[i].set_format ? S.rt.cview[i].format : S.rt.color[i]->d.format)); first = 0; }
    PUT("}");
#undef PUT
}
/* the variant of a program's stage for the bound state: the answers are built only when a cheap signature of what they
   are made from (texture / image handles, vertex formats, target formats) changed */
typedef struct memo_t { const prog_t *p; uint64_t serial; int stage; uint64_t sig; vshader *s; struct memo_t *next; } memo_t;
static memo_t *g_memo[4096];
static uint64_t state_sig(int st) {
    uint64_t h = 1469598103934665603ull;
#define MIX(ptr, n) do { const uint8_t *q_ = (const uint8_t *)(ptr); for (size_t i_ = 0; i_ < (n); i_++) h = (h ^ q_[i_]) * 1099511628211ull; } while (0)
    MIX(S.tex[st], sizeof S.tex[st]); MIX(S.img[st], sizeof S.img[st]);
    if (st == 0) { MIX(&S.nattr, sizeof S.nattr); for (int i = 0; i < S.nattr && i < 16; i++) { uint8_t f = ((va_st *)&S.attr[i])->fmt; MIX(&f, 1); } }
    if (st == 1 && S.has_rt) for (int i = 0; i < S.rt.n && i < 8; i++) { int f = S.rt.color[i] ? (S.rt.has_cview[i] && S.rt.cview[i].set_format ? S.rt.cview[i].format : S.rt.color[i]->d.format) : 0; MIX(&f, sizeof f); }
#undef MIX
    return h;
}
static vshader *stage_shader_slow(int want);
static vshader *stage_shader(int want) {
    const prog_t *p = S.prog; if (!p) return NULL;
    uint64_t sig = state_sig(want); unsigned h = (unsigned)(((uintptr_t)p >> 4) ^ p->serial ^ (sig >> 7) ^ (uint64_t)want) & 4095;
    for (memo_t *m = g_memo[h]; m; m = m->next) if (m->p == p && m->serial == p->serial && m->stage == want && m->sig == sig) return m->s;
    vshader *s = stage_shader_slow(want);
    memo_t *m = malloc(sizeof *m); *m = (memo_t){p, p->serial, want, sig, s, g_memo[h]}; g_memo[h] = m;
    return s;
}
static vshader *stage_shader_slow(int want) {
    const prog_t *p = S.prog; if (!p) return NULL;
    for (unsigned i = 0; i < p->nstage && i < 6; i++) {
        const uint8_t *control = p->sd[i].control; if (!control || !p->sd[i].data) continue;
        uint32_t st; memcpy(&st, control + 0x714, 4);
        if ((int)st != want) continue;
        char a[8192]; answers(want, a, sizeof a);
        return vk_shader_get(control, (const uint8_t *)(uintptr_t)p->sd[i].data, want, a);
    }
    return NULL;
}

/* ---------- state mapping ---------- */
static VkBlendFactor blend_factor(int f) {
    switch (f) {
        case NVN_BLEND_FUNC_ZERO: return VK_BLEND_FACTOR_ZERO; case NVN_BLEND_FUNC_ONE: return VK_BLEND_FACTOR_ONE;
        case NVN_BLEND_FUNC_SRC_COLOR: return VK_BLEND_FACTOR_SRC_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_SRC_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case NVN_BLEND_FUNC_SRC_ALPHA: return VK_BLEND_FACTOR_SRC_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_SRC_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case NVN_BLEND_FUNC_DST_ALPHA: return VK_BLEND_FACTOR_DST_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_DST_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case NVN_BLEND_FUNC_DST_COLOR: return VK_BLEND_FACTOR_DST_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_DST_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case NVN_BLEND_FUNC_SRC_ALPHA_SATURATE: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case NVN_BLEND_FUNC_SRC1_COLOR: return VK_BLEND_FACTOR_SRC1_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_SRC1_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_COLOR;
        case NVN_BLEND_FUNC_SRC1_ALPHA: return VK_BLEND_FACTOR_SRC1_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_SRC1_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_SRC1_ALPHA;
        case NVN_BLEND_FUNC_CONSTANT_COLOR: return VK_BLEND_FACTOR_CONSTANT_COLOR; case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_COLOR: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case NVN_BLEND_FUNC_CONSTANT_ALPHA: return VK_BLEND_FACTOR_CONSTANT_ALPHA; case NVN_BLEND_FUNC_ONE_MINUS_CONSTANT_ALPHA: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}
static VkBlendOp blend_op(int e) { switch (e) { case 2: return VK_BLEND_OP_SUBTRACT; case 3: return VK_BLEND_OP_REVERSE_SUBTRACT; case 4: return VK_BLEND_OP_MIN; case 5: return VK_BLEND_OP_MAX; default: return VK_BLEND_OP_ADD; } }
static VkCompareOp cmp(int f) { return f >= 1 && f <= 8 ? (VkCompareOp)(f - 1) : VK_COMPARE_OP_ALWAYS; }
static VkStencilOp sop(int o) {
    switch (o) { case 2: return VK_STENCIL_OP_ZERO; case 3: return VK_STENCIL_OP_REPLACE; case 4: return VK_STENCIL_OP_INCREMENT_AND_CLAMP; case 5: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
                 case 6: return VK_STENCIL_OP_INVERT; case 7: return VK_STENCIL_OP_INCREMENT_AND_WRAP; case 8: return VK_STENCIL_OP_DECREMENT_AND_WRAP; default: return VK_STENCIL_OP_KEEP; }
}
static int winding_flip(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_VK_WINDING_FLIP"); v = e && atoi(e); } return v; }

static void set_dynamic(VkCommandBuffer cb) {
    /* viewports: NVN's upper-left origin with the y axis as Metal's (a negative-height Vulkan viewport) */
    VkViewport vp[16]; int nvp = S.nvp ? S.nvp : 1;
    for (int i = 0; i < nvp; i++) {
        float x = S.nvp ? S.vp[i][0] : 0, y = S.nvp ? S.vp[i][1] : 0, w = S.nvp ? S.vp[i][2] : (float)g_pw, h = S.nvp ? S.vp[i][3] : (float)g_ph;
        if (w == 0) w = 1; if (h == 0) h = 1;
        vp[i] = (VkViewport){x, y + h, w, -h, S.dr[0], S.dr[1]};
        if (vp[i].minDepth < 0) vp[i].minDepth = 0; if (vp[i].minDepth > 1) vp[i].minDepth = 1; if (vp[i].maxDepth < 0) vp[i].maxDepth = 0; if (vp[i].maxDepth > 1) vp[i].maxDepth = 1;
    }
    vkCmdSetViewportWithCount(cb, (uint32_t)nvp, vp);
    VkRect2D sc[16];
    for (int i = 0; i < nvp; i++) {
        int32_t x0 = 0, y0 = 0, x1 = (int32_t)g_pw, y1 = (int32_t)g_ph;
        if (S.has_sc) { x0 = S.sc.x > 0 ? S.sc.x : 0; y0 = S.sc.y > 0 ? S.sc.y : 0; int64_t ex = (int64_t)S.sc.x + S.sc.w, ey = (int64_t)S.sc.y + S.sc.h; if (ex < x1) x1 = (int32_t)ex; if (ey < y1) y1 = (int32_t)ey; }
        if (x1 < x0) x1 = x0; if (y1 < y0) y1 = y0;
        sc[i] = (VkRect2D){{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
    }
    vkCmdSetScissorWithCount(cb, (uint32_t)nvp, sc);
    vkCmdSetRasterizerDiscardEnable(cb, S.raster_discard);
    vkCmdSetCullMode(cb, S.poly.cull == NVN_FACE_FRONT ? VK_CULL_MODE_FRONT_BIT : S.poly.cull == NVN_FACE_BACK ? VK_CULL_MODE_BACK_BIT : S.poly.cull == NVN_FACE_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE);
    vkCmdSetFrontFace(cb, ((S.poly.front == NVN_FRONT_FACE_CCW) != winding_flip()) ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE);
    V.CmdSetPolygonModeEXT(cb, S.poly.mode == NVN_POLYGON_MODE_LINE ? VK_POLYGON_MODE_LINE : S.poly.mode == NVN_POLYGON_MODE_POINT ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL);
    int bias = (S.poly.offset_en & NVN_POLYGON_OFFSET_ENABLE_FILL) != 0;
    vkCmdSetDepthBiasEnable(cb, bias); vkCmdSetDepthBias(cb, S.po.units, S.po.clamp, S.po.factor);
    vkCmdSetLineWidth(cb, 1.0f);
    int has_d = g_pdepth.v && (vk_tex_aspect(g_pdepth.v) & VK_IMAGE_ASPECT_DEPTH_BIT), has_s = depth_has_stencil();
    vkCmdSetDepthTestEnable(cb, has_d && S.ds.dt); vkCmdSetDepthWriteEnable(cb, has_d && S.ds.dw); vkCmdSetDepthCompareOp(cb, cmp(S.ds.df));
    vkCmdSetDepthBoundsTestEnable(cb, 0);
    vkCmdSetStencilTestEnable(cb, has_s && S.ds.st);
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_BIT, sop(S.ds.fail_f), sop(S.ds.zpass_f), sop(S.ds.zfail_f), cmp(S.ds.sf_f));
    vkCmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, sop(S.ds.fail_b), sop(S.ds.zpass_b), sop(S.ds.zfail_b), cmp(S.ds.sf_b));
    vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)S.st_vmask[0]); vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)S.st_vmask[1]);
    vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)S.st_wmask[0]); vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)S.st_wmask[1]);
    vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)S.st_ref[0]); vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)S.st_ref[1]);
    V.CmdSetDepthClampEnableEXT(cb, S.depth_clamp);
    V.CmdSetRasterizationSamplesEXT(cb, VK_SAMPLE_COUNT_1_BIT);
    VkSampleMask sm = ~0u; V.CmdSetSampleMaskEXT(cb, VK_SAMPLE_COUNT_1_BIT, &sm);
    V.CmdSetAlphaToCoverageEnableEXT(cb, S.ms.a2c && S.ms.enable);
    V.CmdSetLogicOpEnableEXT(cb, 0);
    vkCmdSetBlendConstants(cb, S.blend_color);
    vkCmdSetPrimitiveRestartEnable(cb, 0);
    if (V.CmdSetDepthClipEnableEXT) V.CmdSetDepthClipEnableEXT(cb, !S.depth_clamp);
    int n = g_pcount ? g_pcount : 1;
    VkBool32 en[8]; VkColorBlendEquationEXT eq[8]; VkColorComponentFlags wm[8];
    for (int i = 0; i < n; i++) {
        const blend_st *b = &S.blend[i];
        en[i] = (S.color.blend_en >> i) & 1;
        eq[i] = (VkColorBlendEquationEXT){blend_factor(b->src_rgb), blend_factor(b->dst_rgb), blend_op(b->eq_rgb), blend_factor(b->src_a), blend_factor(b->dst_a), blend_op(b->eq_a)};
        wm[i] = (S.chmask.mask >> (4 * i)) & 0xf;
        if (g_pslot[i] < 0 || !g_patt[i].v) { en[i] = 0; wm[i] = 0; }
    }
    V.CmdSetColorBlendEnableEXT(cb, 0, (uint32_t)n, en); V.CmdSetColorBlendEquationEXT(cb, 0, (uint32_t)n, eq); V.CmdSetColorWriteMaskEXT(cb, 0, (uint32_t)n, wm);
}
static VkPrimitiveTopology topology(int prim) {
    switch (prim) {
        case NVN_DRAW_PRIMITIVE_POINTS: return VK_PRIMITIVE_TOPOLOGY_POINT_LIST; case NVN_DRAW_PRIMITIVE_LINES: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST;
        case NVN_DRAW_PRIMITIVE_LINE_LOOP: case NVN_DRAW_PRIMITIVE_LINE_STRIP: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP;
        case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP;
        case NVN_DRAW_PRIMITIVE_TRIANGLE_FAN: case NVN_DRAW_PRIMITIVE_POLYGON: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_FAN;
        case NVN_DRAW_PRIMITIVE_LINES_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_LIST_WITH_ADJACENCY; case NVN_DRAW_PRIMITIVE_LINE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_LINE_STRIP_WITH_ADJACENCY;
        case NVN_DRAW_PRIMITIVE_TRIANGLES_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST_WITH_ADJACENCY; case NVN_DRAW_PRIMITIVE_TRIANGLE_STRIP_ADJACENCY: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP_WITH_ADJACENCY;
        default: return VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    }
}
static void vertex_input(VkCommandBuffer cb, vshader *vs) {
    VkVertexInputBindingDescription2EXT bd[17]; VkVertexInputAttributeDescription2EXT ad[32]; int nbd = 0, nad = 0; uint32_t used_streams = 0;
    VkBuffer bufs[17]; VkDeviceSize offs[17], sizes[17], strides[17];
    for (int loc = 0; loc < 32; loc++) {
        if (!(vs->in_mask >> loc & 1)) continue;
        va_st a = {0}; if (loc < S.nattr) memcpy(&a, &S.attr[loc], 4);
        const vk_fmt *f = a.fmt ? vk_format(a.fmt) : NULL;
        if (!f || a.stream >= 16) { ad[nad++] = (VkVertexInputAttributeDescription2EXT){.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, .location = (uint32_t)loc, .binding = 16, .format = VK_FORMAT_R32G32B32A32_SFLOAT}; used_streams |= 1u << 16; continue; }
        ad[nad++] = (VkVertexInputAttributeDescription2EXT){.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_ATTRIBUTE_DESCRIPTION_2_EXT, .location = (uint32_t)loc, .binding = a.stream, .format = f->vk, .offset = a.offset};
        used_streams |= 1u << a.stream;
    }
    for (int s = 0; s < 17; s++) {
        if (!(used_streams >> s & 1)) continue;
        VkBuffer b = VK_NULL_HANDLE; VkDeviceSize o = 0, sz = VK_WHOLE_SIZE; uint32_t stride = 0, div = 0;
        if (s < 16) {
            stride = S.stream[s].stride; div = S.stream[s].divisor;
            if (S.vbuf[s].address && vk_pool_find(S.vbuf[s].address, 1, &b, &o)) sz = S.vbuf[s].size ? S.vbuf[s].size : VK_WHOLE_SIZE;
        }
        if (!b) { vk_span z = vk_ring(64, 16); memset(z.cpu, 0, 64); b = z.buf; o = z.off; sz = 64; stride = 0; div = 0; }
        bd[nbd++] = (VkVertexInputBindingDescription2EXT){.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, .binding = (uint32_t)s, .stride = stride,
            .inputRate = div ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, .divisor = div ? div : 1};
        bufs[nbd - 1] = b; offs[nbd - 1] = o; sizes[nbd - 1] = sz; strides[nbd - 1] = stride;
        vkCmdBindVertexBuffers2(cb, (uint32_t)s, 1, &bufs[nbd - 1], &offs[nbd - 1], &sizes[nbd - 1], NULL);
    }
    (void)strides;
    V.CmdSetVertexInputEXT(cb, (uint32_t)nbd, bd, (uint32_t)nad, ad);
}
/* quads and quad strips: an index list of triangles */
static int quad_indices(int prim, const p_draw_idx *di, int first, int count, VkBuffer *b, VkDeviceSize *o, int *n) {
    int quads = prim == NVN_DRAW_PRIMITIVE_QUADS ? count / 4 : count >= 4 ? (count - 2) / 2 : 0;
    if (!quads) return 0;
    vk_span s = vk_ring((VkDeviceSize)quads * 6 * 4, 4); uint32_t *ix = (uint32_t *)s.cpu;
    for (int q = 0; q < quads; q++) {
        uint32_t v[4];
        for (int k = 0; k < 4; k++) {
            int e = prim == NVN_DRAW_PRIMITIVE_QUADS ? q * 4 + k : q * 2 + (k == 2 ? 3 : k == 3 ? 2 : k);
            if (di) { int is = di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4; const uint8_t *src = (const uint8_t *)(uintptr_t)di->ibuf + (size_t)e * is;
                      v[k] = is == 1 ? *src : is == 2 ? *(const uint16_t *)src : *(const uint32_t *)src; }
            else v[k] = (uint32_t)(first + e);
        }
        ix[6 * q + 0] = v[0]; ix[6 * q + 1] = v[1]; ix[6 * q + 2] = v[2]; ix[6 * q + 3] = v[0]; ix[6 * q + 4] = v[2]; ix[6 * q + 5] = v[3];
    }
    *b = s.buf; *o = s.off; *n = quads * 6; return 1;
}
static VkBool32 has_tess(void) { static int v = -1; if (v < 0) { VkPhysicalDeviceFeatures f; vkGetPhysicalDeviceFeatures(V.phys, &f); v = f.tessellationShader ? 1 : 0; v |= f.geometryShader ? 2 : 0; } return (VkBool32)v; }
static void draw(const p_draw *d, const p_draw_idx *di) {
    if (S.render_disabled) { g_skips[1]++; return; }
    vshader *vs = stage_shader(0), *fs = stage_shader(1);
    if (!vs) { g_skips[2]++; return; }
    if (!pass_begin()) return;
    g_feedback = 0;
    pair_t *p = pair_get(vs, fs);
    if (!p->sh[0] || (fs && !p->sh[1])) { g_skips[3]++; return; }
    VkDescriptorSet sets[2]; sets[0] = stage_set(vs, 0, 0, vs->layout); sets[1] = fs ? stage_set(fs, 1, 1, fs->layout) : dset_alloc(V.empty_layout);
    /* the draw samples a target of this pass: what the pass rendered so far goes to memory first. Consecutive such draws (decals
       blending into the G-buffer) share one split, as nvn_mtl.m's store before sample (att_unstored_read); they read the
       pass as it was at the first of them */
    if (g_feedback && !g_fb_run) {
        static uint64_t n; if (!n++) VKLOG("a draw samples a render target of its own pass: the pass is split there (once per run of such draws)");
        end_pass(7); if (!pass_begin()) return;
    }
    g_fb_run = g_feedback;
    VkCommandBuffer cb = vk_cb();
    VkShaderStageFlagBits stages[5] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_GEOMETRY_BIT};
    VkShaderEXT sh[5] = {p->sh[0], fs ? p->sh[1] : VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    int ns = 2 + ((has_tess() & 1) ? 2 : 0); if (has_tess() & 2) { stages[ns] = VK_SHADER_STAGE_GEOMETRY_BIT; sh[ns] = VK_NULL_HANDLE; ns++; }
    if (!(has_tess() & 1) && (has_tess() & 2)) { stages[2] = VK_SHADER_STAGE_GEOMETRY_BIT; }
    V.CmdBindShadersEXT(cb, (uint32_t)ns, stages, sh);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pl, 0, 2, sets, 0, NULL);
    set_dynamic(cb);
    vertex_input(cb, vs);
    int prim = d ? d->prim : di->prim, inst = d ? (d->instances ? d->instances : 1) : (di->instances ? di->instances : 1);
    if (prim == NVN_DRAW_PRIMITIVE_QUADS || prim == NVN_DRAW_PRIMITIVE_QUAD_STRIP) {
        VkBuffer qb; VkDeviceSize qo; int n;
        if (!quad_indices(prim, di, d ? d->first : 0, d ? d->count : di->count, &qb, &qo, &n)) return;
        vkCmdSetPrimitiveTopology(cb, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
        vkCmdBindIndexBuffer(cb, qb, qo, VK_INDEX_TYPE_UINT32);
        vkCmdDrawIndexed(cb, (uint32_t)n, (uint32_t)inst, 0, di ? di->base_vertex : 0, (uint32_t)(d ? d->base_instance : di->base_instance));
    } else {
        vkCmdSetPrimitiveTopology(cb, topology(prim));
        if (d) vkCmdDraw(cb, (uint32_t)d->count, (uint32_t)inst, (uint32_t)d->first, (uint32_t)d->base_instance);
        else {
            VkBuffer ib; VkDeviceSize io;
            if (!vk_pool_find(di->ibuf, 1, &ib, &io)) { g_skips[4]++; return; }
            vkCmdBindIndexBuffer(cb, ib, io, di->itype == 0 ? VK_INDEX_TYPE_UINT8_EXT : di->itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cb, (uint32_t)di->count, (uint32_t)inst, 0, di->base_vertex, (uint32_t)di->base_instance);
        }
    }
    g_draws++; g_frame_draws++;
}
static void dispatch(const p_dispatch *d) {
    vshader *cs = stage_shader(5); if (!cs) { g_skips[5]++; return; }
    end_pass(4);
    comp_t *c = comp_get(cs); if (!c->sh) return;
    g_grid[0] = (uint32_t)d->x; g_grid[1] = (uint32_t)d->y; g_grid[2] = (uint32_t)d->z;
    VkDescriptorSet set = stage_set(cs, 5, 0, cs->layout);
    VkCommandBuffer cb = vk_cb();
    VkShaderStageFlagBits st = VK_SHADER_STAGE_COMPUTE_BIT; V.CmdBindShadersEXT(cb, 1, &st, &c->sh);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->pl, 0, 1, &set, 0, NULL);
    vkCmdDispatch(cb, (uint32_t)d->x, (uint32_t)d->y, (uint32_t)d->z);
    full_barrier(); g_dispatches++;
}

/* ---------- clears and copies ---------- */
static VkRect2D clear_rect(void) {
    int32_t x0 = 0, y0 = 0, x1 = (int32_t)g_pw, y1 = (int32_t)g_ph;
    if (S.has_sc) { if (S.sc.x > x0) x0 = S.sc.x; if (S.sc.y > y0) y0 = S.sc.y; if ((int64_t)S.sc.x + S.sc.w < x1) x1 = S.sc.x + S.sc.w; if ((int64_t)S.sc.y + S.sc.h < y1) y1 = S.sc.y + S.sc.h; }
    if (x1 < x0) x1 = x0; if (y1 < y0) y1 = y0;
    return (VkRect2D){{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
}
static void clear_color(const p_clear_color *c) {
    if (!c->mask || c->index < 0 || c->index >= 8) return;
#ifdef GAME_BOTW
    int f = rt_dup_of(&S.rt, c->index); int idx = f >= 0 ? f : c->index;
#else
    int idx = c->index;
#endif
    if (!pass_begin() || !g_patt[idx].v) return;
    VkClearAttachment a = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)idx}; memcpy(a.clearValue.color.float32, c->rgba, 16);
    VkClearRect r = {clear_rect(), 0, 1}; if (!r.rect.extent.width || !r.rect.extent.height) return;
    vkCmdClearAttachments(vk_cb(), 1, &a, 1, &r);
}
static void clear_ds(const p_clear_ds *c) {
    if (!pass_begin() || !g_pdepth.v) return;
    VkImageAspectFlags asp = 0, have = vk_tex_aspect(g_pdepth.v);
    if (c->depth_mask && (have & VK_IMAGE_ASPECT_DEPTH_BIT)) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (c->stencil_mask && (have & VK_IMAGE_ASPECT_STENCIL_BIT)) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!asp) return;
    VkClearAttachment a = {.aspectMask = asp, .clearValue.depthStencil = {c->depth, (uint32_t)c->stencil}};
    VkClearRect r = {clear_rect(), 0, 1}; if (!r.rect.extent.width || !r.rect.extent.height) return;
    vkCmdClearAttachments(vk_cb(), 1, &a, 1, &r);
}
static void copy_b2b(uint64_t src, uint64_t dst, uint64_t n) {
    VkBuffer sb, db; VkDeviceSize so, dof;
    if (!n || !vk_pool_find(src, n, &sb, &so) || !vk_pool_find(dst, n, &db, &dof)) { g_skips[6]++; return; }
    end_pass(2);
    VkCommandBuffer cb = vk_cb();
    if (sb == db && so < dof + n && dof < so + n) {   /* overlapping copy within one buffer: through the ring */
        vk_span t = vk_ring(n, 16); VkBufferCopy a = {so, t.off, n}, b = {t.off, dof, n};
        vkCmdCopyBuffer(cb, sb, t.buf, 1, &a); full_barrier(); vkCmdCopyBuffer(cb, t.buf, db, 1, &b);
    } else { VkBufferCopy c = {so, dof, n}; vkCmdCopyBuffer(cb, sb, db, 1, &c); }
    full_barrier(); g_copies++;
}
static int region_level(const view_t *v, int has) { return has && v->set_levels ? v->base_level : 0; }
static int region_layer(const view_t *v, int has) { return has && v->set_layers ? v->min_layer : 0; }
static void copy_b2t(const p_copy_b2t *c, int to_buffer) {
    const tex_t *t = c->tex; vtex *v = vk_tex_get(t);
    VkBuffer b; VkDeviceSize o;
    if (!v || !vk_pool_find(c->src, 1, &b, &o)) { g_skips[6]++; return; }
    end_pass(3);
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    uint32_t rowlen = S.copy_stride && bpb ? (uint32_t)(S.copy_stride / bpb) * bw : 0;
    int z0 = c->r.zoffset, depth = c->r.depth ? c->r.depth : 1, is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    VkBufferImageCopy r = {.bufferOffset = o, .bufferRowLength = rowlen, .imageSubresource = {(VkImageAspectFlags)vk_tex_aspect(v) & ~VK_IMAGE_ASPECT_STENCIL_BIT ? (VkImageAspectFlags)vk_tex_aspect(v) & ~VK_IMAGE_ASPECT_STENCIL_BIT : (VkImageAspectFlags)vk_tex_aspect(v),
        (uint32_t)region_level(&c->view, c->has_view), (uint32_t)(region_layer(&c->view, c->has_view) + (is3d ? 0 : z0)), is3d ? 1 : (uint32_t)depth},
        .imageOffset = {c->r.xoffset, c->r.yoffset, is3d ? z0 : 0}, .imageExtent = {(uint32_t)c->r.width, (uint32_t)(c->r.height ? c->r.height : 1), is3d ? (uint32_t)depth : 1}};
    if (r.bufferOffset & 3) { static int said; if (!said++) VKLOG("【红】buffer/texture copy at an unaligned buffer offset"); g_skips[6]++; return; }
    if (to_buffer) vkCmdCopyImageToBuffer(vk_cb(), vk_tex_image(v), VK_IMAGE_LAYOUT_GENERAL, b, 1, &r);
    else { vk_tex_upload_if_stale(v, t); vkCmdCopyBufferToImage(vk_cb(), b, vk_tex_image(v), VK_IMAGE_LAYOUT_GENERAL, 1, &r); vk_tex_gpu_wrote(v); }
    full_barrier(); g_copies++;
}
static void copy_t2t(const p_copy_t2t *c) {
    vtex *s = vk_tex_get(c->src), *d = vk_tex_get(c->dst);
    if (!s || !d) { g_skips[6]++; return; }
    end_pass(3);
    vk_tex_upload_if_stale(s, c->src); vk_tex_upload_if_stale(d, c->dst);
    VkImageAspectFlags as = vk_tex_aspect(s), ad = vk_tex_aspect(d);
    int sl = region_layer(&c->sv, c->has_sv), dl = region_layer(&c->dv, c->has_dv);
    VkImageBlit b = {.srcSubresource = {as, (uint32_t)region_level(&c->sv, c->has_sv), (uint32_t)(sl + c->sr.zoffset), 1},
        .srcOffsets = {{c->sr.xoffset, c->sr.yoffset, 0}, {c->sr.xoffset + c->sr.width, c->sr.yoffset + (c->sr.height ? c->sr.height : 1), 1}},
        .dstSubresource = {ad, (uint32_t)region_level(&c->dv, c->has_dv), (uint32_t)(dl + c->dr.zoffset), 1},
        .dstOffsets = {{c->dr.xoffset, c->dr.yoffset, 0}, {c->dr.xoffset + c->dr.width, c->dr.yoffset + (c->dr.height ? c->dr.height : 1), 1}}};
    int same = c->sr.width == c->dr.width && c->sr.height == c->dr.height;
    uint32_t sw, sh, dw, dh; VkFormat sf, df; vk_tex_dims(s, &sw, &sh, &sf); vk_tex_dims(d, &dw, &dh, &df);
    if (same && (sf == df || (as & VK_IMAGE_ASPECT_DEPTH_BIT))) {
        VkImageCopy r = {b.srcSubresource, b.srcOffsets[0], b.dstSubresource, b.dstOffsets[0], {(uint32_t)c->sr.width, (uint32_t)(c->sr.height ? c->sr.height : 1), 1}};
        vkCmdCopyImage(vk_cb(), vk_tex_image(s), VK_IMAGE_LAYOUT_GENERAL, vk_tex_image(d), VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    } else vkCmdBlitImage(vk_cb(), vk_tex_image(s), VK_IMAGE_LAYOUT_GENERAL, vk_tex_image(d), VK_IMAGE_LAYOUT_GENERAL, 1, &b, (c->flags & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    vk_tex_gpu_wrote(d); full_barrier(); g_copies++;
}
static void clear_tex(const p_clear_tex *c) {
    vtex *v = vk_tex_get(c->tex); if (!v) return;
    end_pass(6); vk_tex_upload_if_stale(v, c->tex);
    VkImageSubresourceRange r = {(VkImageAspectFlags)vk_tex_aspect(v), (uint32_t)region_level(&c->view, c->has_view), 1, (uint32_t)region_layer(&c->view, c->has_view), 1};
    if (r.aspectMask & VK_IMAGE_ASPECT_COLOR_BIT) { VkClearColorValue cv; memcpy(&cv, &c->c, 16); vkCmdClearColorImage(vk_cb(), vk_tex_image(v), VK_IMAGE_LAYOUT_GENERAL, &cv, 1, &r); }
    else { VkClearDepthStencilValue dv = {c->c.f[0], c->c.u[1]}; vkCmdClearDepthStencilImage(vk_cb(), vk_tex_image(v), VK_IMAGE_LAYOUT_GENERAL, &dv, 1, &r); }
    vk_tex_gpu_wrote(v); full_barrier();
}

/* ---------- the stream ---------- */
static int g_inited;
/* the same targets: the same images at the same levels / layers / formats in the same slots */
static int same_view(const view_t *a, int ha, const view_t *b, int hb) {
    int la = ha && a->set_levels ? a->base_level : 0, lb = hb && b->set_levels ? b->base_level : 0;
    int ya = ha && a->set_layers ? a->min_layer : 0, yb = hb && b->set_layers ? b->min_layer : 0;
    int fa = ha && a->set_format ? a->format : -1, fb = hb && b->set_format ? b->format : -1;
    return la == lb && ya == yb && fa == fb;
}
static int same_rt(const p_set_rt *a, const p_set_rt *b) {
    int n = a->n > b->n ? a->n : b->n; if (n > 8) n = 8;
    for (int i = 0; i < n; i++) {
        const tex_t *x = i < a->n ? a->color[i] : NULL, *y = i < b->n ? b->color[i] : NULL;
        if ((x ? x->serial : 0) != (y ? y->serial : 0)) return 0;
        if (x && !same_view(&a->cview[i], a->has_cview[i], &b->cview[i], b->has_cview[i])) return 0;
    }
    if ((a->depth ? a->depth->serial : 0) != (b->depth ? b->depth->serial : 0)) return 0;
    return !a->depth || same_view(&a->dview, a->has_dview, &b->dview, b->has_dview);
}
void mtl_exec(const uint8_t *p) {
    if (!g_inited) { g_inited = 1; state_reset(); support_init(); }
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) VKDIE("corrupt command record at %p: op %u size %u", (const void *)p, h.op, h.size);
        const void *a = p + 8;
        switch (h.op) {
            case OP_END: return;
            case OP_BEGIN: break;
            case OP_JUMP: memcpy(&p, a, 8); continue;
            case OP_CALL: { const uint8_t *t; memcpy(&t, a, 8); mtl_exec(t); break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, (const uint8_t *)a + 8, 4); memcpy(&v, (const uint8_t *)a + 12, 4); vk_sub_post(1, val, m, v, 0); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, (const uint8_t *)a + 8, 8); vk_sub_post(t == NVN_COUNTER_TYPE_SAMPLES_PASSED ? 3 : 2, NULL, 0, 0, ad); break; }
            case OP_RESET_COUNTER: break;
            case OP_COPY_B2B: { uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, (const uint8_t *)a + 8, 8); memcpy(&n, (const uint8_t *)a + 16, 8); copy_b2b(s, d, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, (const uint8_t *)a + 8, 8); upd_add(d, (const uint8_t *)a + 16, n); break; }
            case OP_SET_RT: { const p_set_rt *r = a; if (S.has_rt && same_rt(r, &S.rt)) { S.rt = *r; break; } end_pass(1); S.rt = *r; S.has_rt = 1; break; }
            case OP_CLEAR_COLOR: clear_color(a); break;
            case OP_CLEAR_DS: clear_ds(a); break;
            case OP_VIEWPORTS: { const p_viewports *v = a; for (int i = 0; i < v->n && v->first + i < 16; i++) memcpy(S.vp[v->first + i], v->v[i], 24); if (v->first + v->n > S.nvp) S.nvp = v->first + v->n > 16 ? 16 : v->first + v->n; break; }
            case OP_SCISSOR: S.sc = *(const p_scissor *)a; S.has_sc = 1; break;
            case OP_DEPTH_RANGE: { const p_depth_range *d = a; S.dr[0] = d->n; S.dr[1] = d->f; break; }
            case OP_BIND_PROGRAM: S.prog = ((const p_bind_program *)a)->prog; break;
            case OP_BIND_STATE: { const p_bind_state *b = a;
                switch (b->kind) { case 0: { blend_st bs; memcpy(&bs, b->bytes, sizeof bs); if (bs.target < 8) S.blend[bs.target] = bs; break; }
                                   case 1: memcpy(&S.color, b->bytes, sizeof S.color); break; case 2: memcpy(&S.chmask, b->bytes, sizeof S.chmask); break;
                                   case 3: memcpy(&S.ms, b->bytes, sizeof S.ms); break; case 4: memcpy(&S.poly, b->bytes, sizeof S.poly); break; case 5: memcpy(&S.ds, b->bytes, sizeof S.ds); break; }
                break; }
            case OP_BIND_VATTR: { const p_bind_vattr *b = a; for (int i = 0; i < b->n && b->first + i < 16; i++) S.attr[b->first + i] = b->st[i]; S.nattr = b->first + b->n > 16 ? 16 : b->first + b->n; break; }
            case OP_BIND_VSTREAM: { const p_bind_vstream *b = a; for (int i = 0; i < b->n && b->first + i < 16; i++) S.stream[b->first + i] = b->st[i]; S.nstream = b->first + b->n; break; }
            case OP_BIND_VBUF: { const p_bind_vbuf *b = a; for (int i = 0; i < b->n && b->first + i < 16; i++) S.vbuf[b->first + i] = b->r[i]; break; }
            case OP_BIND_UBO: { const p_bind_buf *b = a; if (b->stage < 6 && b->index >= 0 && b->index < 16) S.ubo[b->stage][b->index] = *b; break; }
            case OP_BIND_SSBO: { const p_bind_buf *b = a; if (b->stage < 6 && b->index >= 0 && b->index < 16) S.ssbo[b->stage][b->index] = *b; break; }
            case OP_BIND_TEX: { const p_bind_handles *b = a; if (b->stage < 6 && b->first >= 0 && b->first + b->n <= 128) memcpy(&S.tex[b->stage][b->first], b->h, 8 * b->n); break; }
            case OP_BIND_SAMP: { const p_bind_handles *b = a; if (b->stage < 6 && b->first >= 0 && b->first + b->n <= 128) memcpy(&S.samp[b->stage][b->first], b->h, 8 * b->n); break; }
            case OP_BIND_IMG: { const p_bind_handles *b = a; if (b->stage < 6 && b->first >= 0 && b->first + b->n <= 32) memcpy(&S.img[b->stage][b->first], b->h, 8 * b->n); break; }
            case OP_STENCIL: { const p_stencil *b = a; for (int f = 0; f < 2; f++) if (b->face & (1 << f)) { if (b->which == 0) S.st_vmask[f] = b->value_mask; else if (b->which == 1) S.st_wmask[f] = b->mask; else S.st_ref[f] = b->ref; } break; }
            case OP_POLY_OFFSET: S.po = *(const p_poly_offset *)a; break;
            case OP_DRAW: draw(a, NULL); break;
            case OP_DRAW_IDX: draw(NULL, a); break;
            case OP_DISPATCH: dispatch(a); break;
            case OP_BARRIER: { int bits; memcpy(&bits, a, 4);
                (void)bits; if (!g_pass_open) full_barrier(); break; }   /* inside a pass nothing, as nvn_mtl.m; every pass ends with a full barrier */
            case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: break;
            case OP_COPY_B2T: copy_b2t(a, 0); break;
            case OP_COPY_STRIDE: S.copy_stride = ((const p_copy_stride *)a)->stride; break;
            case OP_BLEND_COLOR: memcpy(S.blend_color, ((const p_blend_color *)a)->rgba, 16); break;
            case OP_COPY_T2B: copy_b2t(a, 1); break;   /* src: the destination buffer address */
            case OP_COPY_T2T: copy_t2t(a); break;
            case OP_CLEAR_TEX: clear_tex(a); break;
            case OP_DEPTH_CLAMP: S.depth_clamp = ((const p_flag *)a)->value != 0; break;
            case OP_RENDER_ENABLE: S.render_disabled = !((const p_flag *)a)->value; break;
            case OP_RASTER_DISCARD: S.raster_discard = ((const p_flag *)a)->value != 0; break;
            case OP_RENDER_COND: break;
            default: break;
        }
        p += h.size;
    }
}
void mtl_exec_rest(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_sub_begin(void) { vk_sub_open(); }
uint64_t mtl_sub_commit(void) { upd_flush(); g_shadow_epoch++; g_dyn_set = 0; g_dpool = VK_NULL_HANDLE; return vk_sub_submit(); }
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) { copy_b2b(src, dst, n); }
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { vk_sub_post(1, val, mode, v, 0); }
void mtl_sub_counter(uint64_t addr, uint32_t type) { vk_sub_post(type == NVN_COUNTER_TYPE_SAMPLES_PASSED ? 3 : 2, NULL, 0, 0, addr); }
void *mtl_sub_capture_ubos(const uint64_t *h, int n) { (void)h; (void)n; return NULL; }
void mtl_sub_snapshot_ubos_cap(const uint64_t *h, int n, void *cap) { (void)h; (void)n; free(cap); }
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }

/* ---------- present ---------- */
static uint64_t g_presents;
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)period_s; (void)crop; g_presents++;
    if (t) {
        vtex *v = vk_tex_get(t);
        if (v) { uint32_t w, h; VkFormat f; vk_tex_dims(v, &w, &h, &f); vk_present_image(vk_tex_image(v), w, h, f); }
        front_frame_presented();
    }
    if (g_presents % 600 == 0) {
        uint64_t up, ub; void vk_tex_stats(uint64_t *, uint64_t *); vk_tex_stats(&up, &ub);
        VKLOG("present %llu: %llu passes / %llu draws (last frame %llu / %llu), %llu dispatches, %llu copies, uploads %llu (%.1f MB); skipped: no target %llu, disabled %llu, no shader %llu, compile %llu, index %llu, compute %llu, copy %llu",
              (unsigned long long)g_presents, (unsigned long long)g_passes, (unsigned long long)g_draws, (unsigned long long)g_frame_passes, (unsigned long long)g_frame_draws,
              (unsigned long long)g_dispatches, (unsigned long long)g_copies, (unsigned long long)up, ub / 1e6, (unsigned long long)g_skips[0], (unsigned long long)g_skips[1],
              (unsigned long long)g_skips[2], (unsigned long long)g_skips[3], (unsigned long long)g_skips[4], (unsigned long long)g_skips[5], (unsigned long long)g_skips[6]);
        { uint64_t vk_gpu_ns(void); static uint64_t pg; uint64_t g = vk_gpu_ns(); VKLOG("GPU busy %.2f ms per present over the last 600", (g - pg) / 1e6 / 600); pg = g; }
        VKLOG("pass ends: submit %llu, targets %llu, buffer copy %llu, texture copy %llu, dispatch %llu, barrier %llu, texture clear %llu, feedback %llu, update flush %llu",
              (unsigned long long)g_end_why[0], (unsigned long long)g_end_why[1], (unsigned long long)g_end_why[2], (unsigned long long)g_end_why[3], (unsigned long long)g_end_why[4],
              (unsigned long long)g_end_why[5], (unsigned long long)g_end_why[6], (unsigned long long)g_end_why[7], (unsigned long long)g_end_why[8]);
    }
    g_frame_passes = g_frame_draws = 0;
    return g_presents;
}
void mtl_init(void) { vk_init(); }
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = g_passes; *draws = g_draws; }
void mtl_wb_fence_done(void) {}
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) { (void)t; (void)level; (void)layer0; (void)r; (void)dst; (void)row; (void)img; }
void mtl_write_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) { (void)t; (void)level; (void)layer0; (void)r; (void)src; (void)row; (void)img; }
void mtl_prog_release(prog_t *p) { (void)p; }
void mtl_prog_prefetch(const prog_t *p) { (void)p; }
void mtl_present_wait(uint64_t ticket) { (void)ticket; }
double mtl_presented_time(uint64_t n) { (void)n; return 0; }
uint32_t mtl_present_qos(void) { return 0; }
void mtl_shot_at_present(const tex_t *t) { (void)t; }
int mtl_display_hz(void) { return 0; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(uint64_t timeout_ns, unsigned frame_ticks) { (void)timeout_ns; (void)frame_ticks; return 0; }
int mtl_warm_wait(int timeout_ms) { (void)timeout_ms; return 0; }
void mtl_set_frame(uint64_t f) { (void)f; }
void mtl_ub_present(double iv_ms) { (void)iv_ms; }
void mtl_note_world(void) {}
int mtl_pipeline_archive_tool(int argc, char **argv) { (void)argc; (void)argv; return 1; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
void mtl_pin_refs(void (*mark)(uint64_t)) { (void)mark; }
void mtl_probe_pin_rt(const tex_t *t) { (void)t; }
