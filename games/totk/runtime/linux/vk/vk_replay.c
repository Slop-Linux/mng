/* Replay of the recorded NVN command stream (nvn_obj.h ops) into the open Vulkan submission. One SetRenderTargets run is
   one dynamic-rendering pass: the pass opens at its first draw or clear and stays open across state changes and clears,
   until the targets change or a copy, dispatch, texture clear, texture-invalidating barrier or the end of the submission
   needs it closed. Draws use shader objects with all state dynamic; descriptors come from per-submission pools. */
#include "vk.h"
#include <math.h>
#include <unistd.h>
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
/* generations: what a draw derives from the state is rebuilt only when its inputs changed (per stage bindings, fixed-function
   state, vertex input); g_sub_gen invalidates everything that lives in one submission (descriptor sets, ring copies) */
static uint64_t g_gen_bind[6], g_gen_state, g_gen_vtx, g_sub_gen = 1, g_cb_gen = 1;   /* g_cb_gen: the command buffer recorded into (vk_sub_cut) */
uint64_t vk_tex_registry_gen(void);
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
static char trig[256]; static uint64_t trig_chk; static long g_dump_first = -2;   /* TOTK_VK_DUMP: the present the dump started at */
static uint64_t g_vi_hash, g_blend_hash, g_rt_hash, g_links;   /* the parts of the ICD link key last set; first-use link keys counted */
static int g_pass_open; static att_t g_patt[8], g_pdepth; static int g_pslot[8]; static uint32_t g_pw, g_ph; static int g_pcount;
static int g_dyn_set;   /* every dynamic state set once in this command buffer */
/* attachments (bit 0-7 colour, 8 depth / stencil) written by a draw or clear of the open pass. A draw sampling one of them
   reads, on the Switch, what the earlier draws wrote (nvn_mtl.m att_fb_take reads its own pixel from the tile). g16 has no
   tile read for input attachments (its local read splits the render at the barrier) and a split costs a store and load of
   the targets, so a run of sampling draws (decals blending into the G-buffer) shares one split: the run reads the pass as
   it was at its first draw. TOTK_VK_FB_DRAW=1 splits before every such draw (exact; ~250 extra passes a frame in the Shrine) */
static unsigned g_pass_written, g_lr_dirty;   /* g_lr_dirty: written since the last local-read barrier (decal variants) */
static int depth_has_stencil(void) { return g_pdepth.v && (vk_tex_aspect(g_pdepth.v) & VK_IMAGE_ASPECT_STENCIL_BIT); }

/* ---------- occlusion queries (NVN_COUNTER_TYPE_SAMPLES_PASSED): a counter ResetCounter zeroes, every draw adds to and
   ReportCounter writes on completion. While counting each pass segment has a query slot; a report sums the slots since the
   reset (nvn_mtl.m's visibility slots) ---------- */
static int g_q_counting, g_q_skipped, g_q_draws, g_q_open; static int64_t g_q_cur = -1;   /* g_q_open: a reset not yet reported */ static uint32_t *g_q_list; static int g_q_n, g_q_cap;
static int g_pass_recorded;   /* the open pass has been begun on the command buffer (pass_record) */
static int g_pass_nodepth, g_force_depth; static uint64_t g_nodepth_passes;   /* the recorded pass left its depth attachment out (pass_record) */
static void q_begin(void) { if (g_q_counting && g_q_cur < 0 && g_pass_recorded) { g_q_cur = vk_query_alloc(); vkCmdBeginQuery(vk_cb(), vk_query_pool(), (uint32_t)g_q_cur, 0); } }
static void q_end(int keep) {
    if (g_q_cur < 0) return;
    vkCmdEndQuery(vk_cb(), vk_query_pool(), (uint32_t)g_q_cur);
    if (keep) { if (g_q_n == g_q_cap) { g_q_cap = g_q_cap ? 2 * g_q_cap : 64; g_q_list = realloc(g_q_list, g_q_cap * sizeof *g_q_list); } g_q_list[g_q_n++] = (uint32_t)g_q_cur; }
    g_q_cur = -1;
}
static void q_reset(int pass_open) { q_end(0); g_q_n = 0; g_q_skipped = 0; g_q_draws = 0; g_q_counting = 1; g_q_open = 1; if (pass_open) q_begin(); }
static void q_report(uint64_t addr, int pass_open) {
    q_end(1);
    vk_qlist *l = malloc(sizeof *l + (g_q_n ? g_q_n : 1) * sizeof(uint32_t)); l->n = g_q_n; l->skipped = g_q_skipped; l->draws = g_q_draws; memcpy(l->slot, g_q_list, g_q_n * sizeof(uint32_t));
    vk_sub_post_samples(addr, l); g_q_open = 0;
    if (pass_open) q_begin();
}

/* why passes end: 0 submit, 1 targets, 2 buffer copy, 3 texture copy, 4 dispatch, 5 depth needed by a pass begun without, 6 texture clear, 7 feedback, 8 uniform update flush */
static uint64_t g_end_why[9];
static uint64_t g_cut_at; static int g_ncut;   /* draws when the submission was last cut (or opened), cuts in it */
/* TOTK_VK_PASS_TIMES=<present>: GPU time of every pass and dispatch of the frame after that present, logged by time with
   its targets, load operations, draws and why it ended (timestamps around each) */
#define PT_MAX 1024
typedef struct { uint32_t w, h; int ncol, depth, draws, why; unsigned load_clear; VkFormat f0; } pt_info;
static VkQueryPool g_pt_pool; static int g_pt_on, g_pt_n; static pt_info g_pt[PT_MAX];
static void pt_mark(int end) { if (g_pt_on && g_pt_n < PT_MAX) vkCmdWriteTimestamp2(vk_cb(), VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, g_pt_pool, (uint32_t)(2 * g_pt_n + end)); }
static void pt_close(int why) { if (g_pt_on && g_pt_n < PT_MAX) { pt_mark(1); g_pt[g_pt_n].why = why; g_pt_n++; } }
static void full_barrier(void) {
    VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT};
    VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
    vkCmdPipelineBarrier2(vk_cb(), &di);
}
/* ---------- pending clears: a clear of a whole subresource (a texture clear, or the clears of a pass that drew nothing) is
   kept until the image is next rendered, where it becomes the attachment's load operation, instead of a clear command or
   a pass of its own. Any other use of the image first runs it (pclr_flush): a draw or dispatch sampling it (g_pclr_hit),
   a copy, a partial render, the end of the submission ---------- */
typedef struct { vtex *v; int level, layer; VkImageAspectFlags aspect; VkClearValue val; } pclr_t;
#define PCLR 64
static pclr_t g_pclr[PCLR]; static int g_npclr, g_pclr_hit; static uint64_t g_pclr_loads, g_pclr_run, g_pass_dropped;
static uint64_t g_presents;
/* TOTK_VK_FMT_LOG=N (diagnostic): the first N distinct (kind, memory, NVN format, view format, size) of render targets,
   sampled textures and texture copies, with the Vulkan image and its format */
static void fmt_log(const char *kind, const tex_t *t, int vfmt, const vtex *v, const char *extra) {
    static long left = -1; if (left < 0) { const char *e = getenv("TOTK_VK_FMT_LOG"); left = e ? atol(e) : 0; }
    if (left <= 0 || !t) return;
    static uint64_t seen[4096]; uint64_t k = ((uint64_t)(uintptr_t)t->mem * 31 + (uint64_t)t->d.format * 7 + (uint64_t)vfmt * 131 + t->d.width * 8191ull + t->d.height) ^ (uint64_t)(uintptr_t)kind;
    for (int i = 0; i < 4096; i++) { if (seen[i] == k) return; if (!seen[i]) { seen[i] = k; break; } }
    left--; uint32_t w = 0, h = 0; VkFormat f = 0; if (v) vk_tex_dims((vtex *)v, &w, &h, &f);
    VKLOG("fmt %s: mem %p serial %llu NVN fmt %#x view %#x %ux%u lv %u flags %#x -> image %p vk fmt %d %ux%u%s%s (present %llu)", kind, (void *)t->mem, (unsigned long long)t->serial, t->d.format, vfmt,
          t->d.width, t->d.height, t->d.levels, t->d.flags, (void *)v, f, w, h, extra ? " " : "", extra ? extra : "", (unsigned long long)g_presents);
}
static void pclr_flush(void) {
    if (!g_npclr) return;
    for (int i = 0; i < g_npclr; i++) { const pclr_t *e = &g_pclr[i]; VkImageSubresourceRange r = {e->aspect, (uint32_t)e->level, 1, (uint32_t)e->layer, 1};
        if (e->aspect & VK_IMAGE_ASPECT_COLOR_BIT) vkCmdClearColorImage(vk_cb(), vk_tex_image(e->v), VK_IMAGE_LAYOUT_GENERAL, &e->val.color, 1, &r);
        else vkCmdClearDepthStencilImage(vk_cb(), vk_tex_image(e->v), VK_IMAGE_LAYOUT_GENERAL, &e->val.depthStencil, 1, &r); }
    g_pclr_run += g_npclr; g_npclr = 0; g_pclr_hit = 0; full_barrier();
}
static void pclr_add(vtex *v, int level, int layer, VkImageAspectFlags aspect, VkClearValue val) {
    if (vk_tex_is3d(v)) layer = 0;   /* a 3D texture has one layer (its clear covers every slice) */
    for (int i = 0; i < g_npclr; i++) if (g_pclr[i].v == v && g_pclr[i].level == level && g_pclr[i].layer == layer) {   /* a later clear replaces the aspects it covers */
        if (!(g_pclr[i].aspect & ~aspect)) { g_pclr[i] = g_pclr[--g_npclr]; i--; continue; }
        pclr_flush(); break;
    }
    if (g_npclr == PCLR) pclr_flush();
    g_pclr[g_npclr++] = (pclr_t){v, level, layer, aspect, val}; vk_tex_gpu_wrote(v);
}
static int pclr_find(vtex *v, int level, int layer) { for (int i = 0; i < g_npclr; i++) if (g_pclr[i].v == v && g_pclr[i].level == level && g_pclr[i].layer == layer) return i; return -1; }

/* the open pass is begun lazily (pass_record): a pass that only clears never reaches the GPU, its clears stay pending */
static VkRenderingAttachmentInfo g_lca[8], g_lda, g_lsa; static VkRenderingInfo g_lri; static unsigned g_pass_full;
static void pass_record(void) {
    if (!g_pass_open || g_pass_recorded) return;
    if (g_pt_on && g_pt_n < PT_MAX) { unsigned lc = 0; for (int i = 0; i < g_pcount; i++) lc |= (g_lca[i].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) << i; if (g_lda.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) lc |= 1u << 8;
        VkFormat f0 = 0; for (int i = 0; i < g_pcount && !f0; i++) if (g_patt[i].v) { uint32_t a_, b_; vk_tex_dims(g_patt[i].v, &a_, &b_, &f0); }
        g_pt[g_pt_n] = (pt_info){g_pw, g_ph, g_pcount, g_pdepth.v != NULL, 0, 0, lc, f0}; pt_mark(0); }
    /* attachments loaded are read; ones nothing ever reads are not stored (vk_tex_store_needed) */
    for (int i = 0; i < g_pcount; i++) if (g_patt[i].v) { if (g_lca[i].loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) vk_tex_mark_read(g_patt[i].v); if (!vk_tex_store_needed(g_patt[i].v)) g_lca[i].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; }
    if (g_pdepth.v) { if (g_lda.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD || g_lsa.loadOp == VK_ATTACHMENT_LOAD_OP_LOAD) vk_tex_mark_read(g_pdepth.v);
                     if (!vk_tex_store_needed(g_pdepth.v)) g_lda.storeOp = g_lsa.storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE; }
    /* a pass begun by a draw without depth / stencil test or write leaves its depth attachment out (no load, no store);
       a later draw of it that uses depth splits it (draw, clear_ds) */
    g_pass_nodepth = 0;
    static int nodepth = -1; if (nodepth < 0) { const char *e = getenv("TOTK_VK_NODEPTH"); nodepth = !(e && !strcmp(e, "0")); }
    if (nodepth && g_pdepth.v && !g_force_depth && !S.ds.dt && !S.ds.dw && !S.ds.st && g_lda.loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR && g_lsa.loadOp != VK_ATTACHMENT_LOAD_OP_CLEAR) {
        g_lri.pDepthAttachment = g_lri.pStencilAttachment = NULL; g_pass_nodepth = 1; g_nodepth_passes++; }
    if (g_dump_first == -4) {   /* TOTK_VK_DUMP_AT=<w>x<h>: an armed dump starts at the first pass with an attachment of that size */
        static unsigned aw_ = 0, ah_ = 0; static int af_ = -1; if (!aw_) sscanf(getenv("TOTK_VK_DUMP_AT"), "%ux%u:%d", &aw_, &ah_, &af_);   /* <w>x<h>[:<Vulkan format>] */
        for (int i = 0; i < g_pcount; i++) if (g_patt[i].v) { uint32_t w_, h_; VkFormat f_; vk_tex_dims(g_patt[i].v, &w_, &h_, &f_); if (w_ == aw_ && h_ == ah_ && (af_ < 0 || (int)f_ == af_)) { g_dump_first = (long)g_presents; VKLOG("dump: armed dump starts at present %llu", (unsigned long long)g_presents); break; } }
    }
    if (g_dump_first >= 0 && (long)g_presents == g_dump_first) VKLOG("pass: %ux%u att0 load %d clear %.3f %.3f %.3f %.3f, %d colour, depth %d load %d clear %.3f", g_pw, g_ph, g_pcount ? (int)g_lca[0].loadOp : -1, g_lca[0].clearValue.color.float32[0], g_lca[0].clearValue.color.float32[1], g_lca[0].clearValue.color.float32[2], g_lca[0].clearValue.color.float32[3], g_pcount, g_pdepth.v != NULL, (int)g_lda.loadOp, g_lda.clearValue.depthStencil.depth);
    { uint64_t h_ = (uint64_t)g_pcount; for (int i = 0; i < g_pcount; i++) { uint32_t w_, h2_; VkFormat f_ = 0; if (g_patt[i].v) vk_tex_dims(g_patt[i].v, &w_, &h2_, &f_); h_ = h_ * 1099511628211ull + (uint64_t)f_; }
      if (g_lri.pDepthAttachment && g_pdepth.v) { uint32_t w_, h2_; VkFormat f_; vk_tex_dims(g_pdepth.v, &w_, &h2_, &f_); h_ = h_ * 1099511628211ull + (uint64_t)f_ + 77; } g_rt_hash = h_; }
    vkCmdBeginRendering(vk_cb(), &g_lri);
    g_pass_recorded = 1; g_passes++; g_frame_passes++; g_gen_state++;
    q_begin();
}
/* TOTK_VK_DUMP=<dir>:<first present>:<count>: from that present on, every colour attachment of a pass is copied out at the
   pass end (in the submission, no wait) and written as <dir>/d<present>-<n>-<w>x<h>-vk<format>.ppm once it completed;
   <count> pictures in all */
typedef struct { VkBuffer b; VkDeviceMemory m; uint32_t w, h; VkFormat f; char path[256]; } dump_t;
static float h2f(uint16_t h) { uint32_t s = h >> 15, e = h >> 10 & 31, m = h & 1023; float v = e ? ldexpf((float)(m | 1024), (int)e - 25) : ldexpf((float)m, -24); return s ? -v : v; }
static uint8_t f2b(float x) { x = x < 0 ? 0 : x > 1 ? 1 : x; return (uint8_t)(x * 255 + 0.5f); }
static void dump_land(void *p) {
    dump_t *d = p; uint8_t *c; FILE *f = fopen(d->path, "wb");
    if (f && vkMapMemory(V.dev, d->m, 0, VK_WHOLE_SIZE, 0, (void **)&c) == VK_SUCCESS) {
        fprintf(f, "P6\n%u %u\n255\n", d->w, d->h);
        int bpp = d->f == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
        for (uint32_t i = 0; i < d->w * d->h; i++) { uint8_t px[3]; uint32_t v; memcpy(&v, c + (size_t)bpp * i, 4);
            if (d->f == VK_FORMAT_A2B10G10R10_UNORM_PACK32) { px[0] = (v & 1023) >> 2; px[1] = (v >> 10 & 1023) >> 2; px[2] = (v >> 20 & 1023) >> 2; }
            else if (d->f == VK_FORMAT_B10G11R11_UFLOAT_PACK32) { uint32_t r = v & 2047, g = v >> 11 & 2047, b = v >> 22 & 1023;
                px[0] = f2b(ldexpf((float)((r & 63) | ((r >> 6) ? 64 : 0)), (int)(r >> 6) - 21)); px[1] = f2b(ldexpf((float)((g & 63) | ((g >> 6) ? 64 : 0)), (int)(g >> 6) - 21)); px[2] = f2b(ldexpf((float)((b & 31) | ((b >> 5) ? 32 : 0)), (int)(b >> 5) - 20)); }
            else if (bpp == 8) { uint16_t h4[4]; memcpy(h4, c + 8 * i, 8); px[0] = f2b(h2f(h4[0])); px[1] = f2b(h2f(h4[1])); px[2] = f2b(h2f(h4[2])); }
            else if (d->f == VK_FORMAT_B8G8R8A8_UNORM || d->f == VK_FORMAT_B8G8R8A8_SRGB) { px[0] = c[4 * i + 2]; px[1] = c[4 * i + 1]; px[2] = c[4 * i]; }
            else { px[0] = c[4 * i]; px[1] = c[4 * i + 1]; px[2] = c[4 * i + 2]; }
            fwrite(px, 1, 3, f); }
        { char ap[270]; snprintf(ap, sizeof ap, "%.*s-alpha.pgm", (int)strlen(d->path) - 4, d->path); FILE *g = fopen(ap, "wb");   /* alpha as grey */
          if (g) { fprintf(g, "P5\n%u %u\n255\n", d->w, d->h); int bpp = d->f == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
            for (uint32_t i = 0; i < d->w * d->h; i++) { uint32_t v; memcpy(&v, c + (size_t)bpp * i, 4); uint8_t a;
                if (d->f == VK_FORMAT_A2B10G10R10_UNORM_PACK32) a = (uint8_t)((v >> 30) * 85); else if (d->f == VK_FORMAT_B10G11R11_UFLOAT_PACK32) a = 255;
                else if (bpp == 8) { uint16_t h4[4]; memcpy(h4, c + 8 * i, 8); a = f2b(h2f(h4[3])); } else a = c[4 * i + 3];
                fputc(a, g); }
            fclose(g); } }
        vkUnmapMemory(V.dev, d->m);
    }
    if (f) fclose(f);
    vkDestroyBuffer(V.dev, d->b, NULL); vkFreeMemory(V.dev, d->m, NULL); free(d);
}
static int g_probe_x = -1, g_probe_y, g_probe_w, g_probe_h, g_probe_pass; static uint64_t g_probe_n;   /* g_probe_pass (TOTK_VK_PROBE=...,pass): probed at each pass end as the frame makes its passes, no split per draw */
static int probe_on(void) { static int init; if (!init) { init = 1; const char *e = getenv("TOTK_VK_PROBE"); if (e) { sscanf(e, "%d,%d,%dx%d", &g_probe_x, &g_probe_y, &g_probe_w, &g_probe_h); g_probe_pass = strstr(e, ",pass") != NULL; } } return g_probe_x >= 0; }
typedef struct { VkBuffer b; VkDeviceMemory m; uint64_t n; int na; VkFormat f[8]; uint32_t w, h; } probe_t;
static void probe_land(void *p) {
    probe_t *q = p; uint32_t *c; if (vkMapMemory(V.dev, q->m, 0, VK_WHOLE_SIZE, 0, (void **)&c) == VK_SUCCESS) {
        char s[400]; int o = 0; for (int i = 0; i < q->na; i++) o += snprintf(s + o, sizeof s - o, " a%d(vk%d) %08x", i, q->f[i], c[i]);
        VKLOG("probe: draw %llu pass %ux%u%s", (unsigned long long)q->n, q->w, q->h, s); vkUnmapMemory(V.dev, q->m); }
    vkDestroyBuffer(V.dev, q->b, NULL); vkFreeMemory(V.dev, q->m, NULL); free(q);
}
static void end_pass(int why);
static void probe_draw(int split) {
    if (g_pw < 64 || g_ph < 36) return;   /* every pass of at least 64x36: the probe point scaled from <w>x<h> to the pass size */
    int px_ = (int)((int64_t)g_probe_x * g_pw / g_probe_w), py_ = (int)((int64_t)g_probe_y * g_ph / g_probe_h);
    uint64_t n = g_probe_n++;
    if (split) end_pass(7);
    probe_t *q = calloc(1, sizeof *q); q->n = n; q->w = g_pw; q->h = g_ph;
    VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = 64, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
    VKCHECK(vkCreateBuffer(V.dev, &bi, NULL, &q->b)); VkMemoryRequirements rq; vkGetBufferMemoryRequirements(V.dev, q->b, &rq);
    VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = vk_memtype(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
    VKCHECK(vkAllocateMemory(V.dev, &ai, NULL, &q->m)); VKCHECK(vkBindBufferMemory(V.dev, q->b, q->m, 0));
    for (int i = 0; i < g_pcount && i < 8; i++) { vtex *v = g_patt[i].v; if (!v) continue; uint32_t w, h; VkFormat f; vk_tex_dims(v, &w, &h, &f);
        if (f != VK_FORMAT_R8G8B8A8_UNORM && f != VK_FORMAT_R8G8B8A8_SRGB && f != VK_FORMAT_B10G11R11_UFLOAT_PACK32 && f != VK_FORMAT_A2B10G10R10_UNORM_PACK32 && f != VK_FORMAT_R32_SFLOAT && f != VK_FORMAT_R16G16_SFLOAT && f != VK_FORMAT_R16G16_UNORM) continue;
        VkBufferImageCopy r = {.bufferOffset = 4 * (VkDeviceSize)i, .imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, (uint32_t)g_patt[i].level, vk_tex_is3d(v) ? 0 : (uint32_t)g_patt[i].layer, 1},
            .imageOffset = {px_, py_, 0}, .imageExtent = {1, 1, 1}};
        vkCmdCopyImageToBuffer(vk_cb(), vk_tex_image(v), VK_IMAGE_LAYOUT_GENERAL, q->b, 1, &r); q->f[i] = f; q->na = i + 1; }
    full_barrier(); vk_defer_free(probe_land, q);
}
static void dump_attachments(void) {
    static const char *dir; static long left; if (g_dump_first == -2) { const char *e = getenv("TOTK_VK_DUMP"); g_dump_first = -1;
        if (e && strchr(e, ':')) { static char d_[200]; snprintf(d_, sizeof d_, "%.*s", (int)(strchr(e, ':') - e), e); dir = d_; g_dump_first = atol(strchr(e, ':') + 1); left = strchr(strchr(e, ':') + 1, ':') ? atol(strchr(strchr(e, ':') + 1, ':') + 1) : 50;
            const char *c = strchr(e, ':') + 1;   /* <dir>:<trigger file>:<count>: dumps start once the file appears (it is removed) */
            if (!(*c >= '0' && *c <= '9')) { snprintf(trig, sizeof trig, "%.*s", (int)(strchr(c, ':') ? strchr(c, ':') - c : (long)strlen(c)), c); g_dump_first = -3; } } }
    if (g_dump_first == -3 && g_presents != trig_chk) { trig_chk = g_presents; if (!access(trig, F_OK)) { unlink(trig); g_dump_first = getenv("TOTK_VK_DUMP_AT") ? -4 : (long)g_presents; VKLOG("dump: triggered at present %llu%s", (unsigned long long)g_presents, g_dump_first == -4 ? ", armed for TOTK_VK_DUMP_AT" : ""); } }
    if (g_dump_first < 0 || (long)g_presents < g_dump_first || left <= 0) return;
    static uint64_t n; int dumped = 0;
    for (int i = 0; i < g_pcount && left > 0; i++) { vtex *v = g_patt[i].v; if (!v || g_patt[i].level || g_patt[i].layer) continue;
        uint32_t w, h; VkFormat f; vk_tex_dims(v, &w, &h, &f); int bpp = f == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
        if (f != VK_FORMAT_A2B10G10R10_UNORM_PACK32 && f != VK_FORMAT_B10G11R11_UFLOAT_PACK32 && f != VK_FORMAT_R8G8B8A8_UNORM && f != VK_FORMAT_R8G8B8A8_SRGB && f != VK_FORMAT_B8G8R8A8_UNORM && f != VK_FORMAT_B8G8R8A8_SRGB && f != VK_FORMAT_R16G16B16A16_SFLOAT) continue;
        dump_t *d = calloc(1, sizeof *d); d->w = w; d->h = h; d->f = f; snprintf(d->path, sizeof d->path, "%s/d%llu-%llu-%ux%u-vk%d.ppm", dir, (unsigned long long)g_presents, (unsigned long long)n++, w, h, f);
        VkBufferCreateInfo bi = {.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, .size = (VkDeviceSize)w * h * bpp, .usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT};
        VKCHECK(vkCreateBuffer(V.dev, &bi, NULL, &d->b)); VkMemoryRequirements rq; vkGetBufferMemoryRequirements(V.dev, d->b, &rq);
        VkMemoryAllocateInfo ai = {.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, .allocationSize = rq.size, .memoryTypeIndex = vk_memtype(rq.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)};
        VKCHECK(vkAllocateMemory(V.dev, &ai, NULL, &d->m)); VKCHECK(vkBindBufferMemory(V.dev, d->b, d->m, 0));
        VkBufferImageCopy r = {.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1}, .imageExtent = {w, h, 1}};
        vkCmdCopyImageToBuffer(vk_cb(), vk_tex_image(v), VK_IMAGE_LAYOUT_GENERAL, d->b, 1, &r);
        vk_defer_free(dump_land, d); left--;
        VKLOG("dump: present %llu attachment %d %ux%u vk %d -> %s", (unsigned long long)g_presents, i, w, h, f, d->path); dumped = 1;
    }
    if (dumped) { VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, .srcAccessMask = VK_ACCESS_2_MEMORY_WRITE_BIT,
        .dstStageMask = VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT | VK_PIPELINE_STAGE_2_HOST_BIT, .dstAccessMask = VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_HOST_READ_BIT};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .memoryBarrierCount = 1, .pMemoryBarriers = &mb}; vkCmdPipelineBarrier2(vk_cb(), &di); }
}
static void end_pass(int why) {
    if (!g_pass_open) return;
    static int pclr = -1; if (pclr < 0) { const char *e = getenv("TOTK_VK_PCLR"); pclr = !(e && !strcmp(e, "0")); }
    if (!g_pass_recorded && !pclr) pass_record();   /* TOTK_VK_PCLR=0: passes that only clear are recorded */
    if (!g_pass_recorded) {   /* nothing drawn: the cleared attachments that cover their whole level go pending */
        int drop = 1;
        for (int i = 0; i < g_pcount; i++) if (g_lca[i].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR && !(g_pass_full >> i & 1)) drop = 0;
        for (int i = 0; i < g_pcount; i++) if (vk_tex_is3d(g_patt[i].v)) drop = 0;   /* a slice of a 3D texture: a pending clear would clear every slice */
        if ((g_lda.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR || g_lsa.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) && !(g_pass_full >> 8 & 1)) drop = 0;
        if (drop) {
            for (int i = 0; i < g_pcount; i++) if (g_lca[i].loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR) pclr_add(g_patt[i].v, g_patt[i].level, g_patt[i].layer, VK_IMAGE_ASPECT_COLOR_BIT, g_lca[i].clearValue);
            VkImageAspectFlags a = (g_lda.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) | (g_lsa.loadOp == VK_ATTACHMENT_LOAD_OP_CLEAR ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
            if (a) { VkClearValue cv = {.depthStencil = {g_lda.clearValue.depthStencil.depth, g_lsa.clearValue.depthStencil.stencil}}; pclr_add(g_pdepth.v, g_pdepth.level, g_pdepth.layer, a, cv); }
            g_pass_open = 0; g_pass_dropped++; return;
        }
        pass_record();
    }
    g_end_why[why]++;
    q_end(1);
    vkCmdEndRendering(vk_cb()); g_pass_open = 0; g_pass_recorded = 0; pt_close(why);
    full_barrier(); dump_attachments(); if (g_probe_pass && g_dump_first >= 0 && (long)g_presents == g_dump_first) probe_draw(0);
    /* TOTK_VK_CHUNK_DRAWS=<first>[/<next>] (default 30/40 as MNG on macOS; 0 off): the submission so far goes to the GPU (vk_sub_cut) once its
       first part has <first> draws, then every <next> */
    static long first = -1, next; if (first < 0) { const char *e = getenv("TOTK_VK_CHUNK_DRAWS"); first = e ? atol(e) : 30; next = e ? first : 40; if (e && strchr(e, '/')) next = atol(strchr(e, '/') + 1); }
    if (why && first && g_draws - g_cut_at >= (uint64_t)(g_ncut ? next : first)) { void vk_sub_cut(void); vk_sub_cut(); g_cut_at = g_draws; g_ncut++; g_cb_gen++; }
}
void vk_replay_end_pass(void) { end_pass(0); pclr_flush(); }
void vk_replay_outside_pass(void) { end_pass(3); for (int k = 0; k < 6; k++) g_gen_bind[k]++; }   /* an image made mutable is copied (vk_tex.c tex_mutable): sets are rebuilt with its new views; a draw then begins the pass again */
/* the clears that open a pass become its load operations: from the op that opens it, the run of clears and state changes
   before its first draw (scissor tracked; a render condition, a draw or any other op ends the run). The first clear of an
   attachment that covers the whole render area with every channel is taken (g_taken); the clear op then does nothing */
static const uint8_t *g_op;   /* the op being replayed */
static const void *g_taken[10]; static int g_ntaken;
static uint64_t g_load_clears;
static int same_rt(const p_set_rt *a, const p_set_rt *b);
static void take_clears(VkRenderingAttachmentInfo *ca, int n, VkRenderingAttachmentInfo *da, VkRenderingAttachmentInfo *sa, uint32_t w, uint32_t h) {
    g_ntaken = 0;
    p_scissor sc = S.sc; int has_sc = S.has_sc; unsigned seen = 0;
    const uint8_t *p = g_op;
    for (int steps = 0; p && steps < 256; steps++) {
        op_hdr hd; memcpy(&hd, p, sizeof hd); const void *a = p + 8;
        int full = !has_sc || (sc.x <= 0 && sc.y <= 0 && (int64_t)sc.x + sc.w >= (int64_t)w && (int64_t)sc.y + sc.h >= (int64_t)h);
        switch (hd.op) {
            case OP_CLEAR_COLOR: { const p_clear_color *c = a; if (c->index < 0 || c->index >= 8) break;
#ifdef GAME_BOTW
                int f = rt_dup_of(&S.rt, c->index); int i = f >= 0 ? f : c->index;
#else
                int i = c->index;
#endif
                if (i < n && ca[i].imageView && !(seen >> i & 1) && full && (c->mask & 0xf) == 0xf) {
                    ca[i].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; memcpy(ca[i].clearValue.color.float32, c->rgba, 16); g_taken[g_ntaken++] = p; g_load_clears++; }
                if (c->mask) seen |= 1u << i;
                break; }
            case OP_CLEAR_DS: { const p_clear_ds *c = a;
                int dt = c->depth_mask && da->sType && !(seen >> 8 & 1), st = c->stencil_mask && sa->sType && !(seen >> 9 & 1);
                if (full && (dt || !c->depth_mask || !da->sType) && (st || !c->stencil_mask || !sa->sType) && (dt || st) && (!st || (c->stencil_mask & 0xff) == 0xff)) {
                    if (dt) { da->loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da->clearValue.depthStencil.depth = c->depth; }
                    if (st) { sa->loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; sa->clearValue.depthStencil.stencil = (uint32_t)c->stencil; }
                    g_taken[g_ntaken++] = p; g_load_clears++;
                }
                seen |= (c->depth_mask ? 1u << 8 : 0) | (c->stencil_mask ? 1u << 9 : 0);
                break; }
            case OP_SCISSOR: sc = *(const p_scissor *)a; has_sc = 1; break;
            case OP_SET_RT: if (!same_rt(a, &S.rt)) return; break;
            case OP_JUMP: memcpy(&p, a, 8); continue;
            case OP_BEGIN: case OP_VIEWPORTS: case OP_DEPTH_RANGE: case OP_BIND_PROGRAM: case OP_BIND_STATE: case OP_BIND_VATTR: case OP_BIND_VSTREAM: case OP_BIND_VBUF:
            case OP_BIND_UBO: case OP_BIND_SSBO: case OP_BIND_TEX: case OP_BIND_SAMP: case OP_BIND_IMG: case OP_STENCIL: case OP_POLY_OFFSET: case OP_BLEND_COLOR:
            case OP_COPY_STRIDE: case OP_DEPTH_CLAMP: case OP_RASTER_DISCARD: case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: break;
            default: return;
        }
        if (g_ntaken == 10) return;
        p += hd.size;
    }
}
static int clear_taken(void) { for (int i = 0; i < g_ntaken; i++) if (g_taken[i] == g_op) return 1; return 0; }
static int pass_begin(void) {
    if (g_pass_open) return 1;
    if (!S.has_rt) { g_skips[0]++; return 0; }
    VkRenderingAttachmentInfo ca[8] = {0}, da = {0}, sa = {0}; int n = 0; uint32_t w = UINT32_MAX, h = UINT32_MAX, aw[9] = {0}, ah[9] = {0};
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
        if (!(S.rt.has_cview[i] && S.rt.cview[i].set_format && S.rt.cview[i].format != t->d.format)) { aw[i] = tw; ah[i] = th; }   /* pending clears: the image's own format only */
        fmt_log("target", t, S.rt.has_cview[i] && S.rt.cview[i].set_format ? S.rt.cview[i].format : t->d.format, v, NULL);
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
            uint32_t tw, th; VkFormat f; vk_tex_dims(v, &tw, &th, &f);
            int dl = S.rt.has_dview && S.rt.dview.set_levels ? S.rt.dview.base_level : 0; aw[8] = tw >> dl ? tw >> dl : 1; ah[8] = th >> dl ? th >> dl : 1;
            if (aw[8] < w) w = aw[8]; if (ah[8] < h) h = ah[8];
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
    /* pending clears of the attachments: their load operations when the pass covers the whole level, else they run first */
    g_pass_full = 0; g_pass_written = 0; g_lr_dirty = 0;
    for (int a = 0; a < 9; a++) if (aw[a] == w && ah[a] == h) g_pass_full |= 1u << a;
    for (int a = 0; a < 9; a++) {
        const att_t *at = a < 8 ? &g_patt[a] : &g_pdepth; int k = at->v ? pclr_find(at->v, at->level, at->layer) : -1;
        if (k < 0) continue;
        const pclr_t *e = &g_pclr[k];
        int ok = g_pass_full >> a & 1;
        if (a == 8) ok &= !((e->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) && !da.sType) && !((e->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) && !sa.sType);
        if (!ok) { pclr_flush(); break; }
        if (a < 8) { ca[a].loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; ca[a].clearValue = e->val; }
        else { if (e->aspect & VK_IMAGE_ASPECT_DEPTH_BIT) { da.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; da.clearValue = e->val; }
               if (e->aspect & VK_IMAGE_ASPECT_STENCIL_BIT) { sa.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR; sa.clearValue = e->val; } }
        g_pass_written |= 1u << a; g_pclr_loads++; g_pclr[k] = g_pclr[--g_npclr];
    }
    take_clears(ca, n, &da, &sa, w, h);
    memcpy(g_lca, ca, sizeof ca); g_lda = da; g_lsa = sa;
    g_lri = (VkRenderingInfo){.sType = VK_STRUCTURE_TYPE_RENDERING_INFO, .renderArea = {{0, 0}, {w, h}}, .layerCount = 1, .colorAttachmentCount = (uint32_t)n, .pColorAttachments = g_lca,
        .pDepthAttachment = da.sType ? &g_lda : NULL, .pStencilAttachment = sa.sType ? &g_lsa : NULL};
    g_pass_open = 1; g_pass_recorded = 0; g_pclr_hit = 0;
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
static void gw_add(uint64_t lo, uint64_t hi);
static void upd_flush(void) {
    if (!g_nupd) return;
    end_pass(8);
    for (int i = 0; i < g_nupd; i++) {
        VkBuffer b; VkDeviceSize o;
        if (!vk_pool_find(g_upd[i].dst, g_upd[i].n, &b, &o)) continue;
        VkBufferCopy c = {g_upd[i].soff, o, g_upd[i].n}; vkCmdCopyBuffer(vk_cb(), g_upd[i].src, b, 1, &c);
        gw_add(g_upd[i].dst, g_upd[i].dst + g_upd[i].n);   /* lands at the end of the submission: the next submissions read it from memory once completed */
    }
    full_barrier(); g_nupd = 0;
}
/* bytes of uniform memory as the GPU would read them here (a storage-buffer address held in a uniform block) */
static void upd_read(uint64_t addr, void *dst, uint64_t n) {
    memcpy(dst, (const void *)(uintptr_t)addr, n);
    for (int i = 0; i < g_nupd; i++) {
        uint64_t lo = g_upd[i].dst > addr ? g_upd[i].dst : addr, hi = g_upd[i].dst + g_upd[i].n < addr + n ? g_upd[i].dst + g_upd[i].n : addr + n;
        if (lo < hi) memcpy((uint8_t *)dst + (lo - addr), g_upd[i].data + (lo - g_upd[i].dst), hi - lo);
    }
}
/* uniform copies (count, bytes) for the long-frame log */
static uint64_t g_ucopy[2], g_ucopy_all[2];
/* renamed copies made in this submission, by range and the number of updates they include */
typedef struct { uint64_t addr, size; int nupd; VkBuffer buf; VkDeviceSize off; const uint8_t *cpu; } shadow_t;
static shadow_t g_shadow[1024]; static uint64_t g_shadow_epoch, g_shadow_ep[1024];
/* a uniform range as the GPU would read it at this point: memory plus the pending updates that overlap it */
/* TOTK_VK_POOL_SHADOW=1 (diagnostic): uniform, vertex, index and read-only storage ranges are read from a copy made at
   replay instead of the game's memory (zero copy), up to 16 MiB a range */
static int pool_shadow(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_VK_POOL_SHADOW"); v = e && atoi(e) == 1; if (v) VKLOG("pool shadow: buffer ranges read from copies"); } return v; }
static uint64_t g_pshadow[2];   /* ranges copied, ranges left in place (too large) */
static int range_copy(uint64_t addr, uint64_t size, int direct, VkBuffer *buf, VkDeviceSize *off, VkDeviceSize *range);
/* uniform snapshot (nvn_mtl.m usnap, TOTK_UBO_SNAPSHOT; TOTK_VK_UBO_SNAPSHOT=0 off): a uniform range is read from a copy of its memory taken at its
   first binding in the submission, so game threads that rebuild constant pools while the GPU is still on the frame (the
   one-submission early release of TOTK_SYNC_LAG) do not reach the GPU. A range the GPU writes (buffer copy, image-to-buffer
   copy, writable storage buffer) in this submission or in one not yet completed is read from memory, and a copy it
   overlaps is dropped from then on (nvn_mtl.m pend table, US_DROPPED) */
static int ubo_snapshot(void) { static int v = -1; if (v < 0) { const char *e = getenv("TOTK_VK_UBO_SNAPSHOT"); v = !(e && !strcmp(e, "0")); if (v) VKLOG("uniform snapshot: uniform ranges read from copies taken at their first binding in a submission"); } return v; }
#define GW_MAX 4096
static struct { uint64_t lo, hi, fence; } g_gw[GW_MAX]; static int g_ngw; static uint64_t g_gw_over, g_usnap[3];   /* g_usnap: copies, reads from memory (GPU-written), copies dropped */
static int gw_hits(uint64_t lo, uint64_t hi) {
    uint64_t done = vk_done(); if (g_gw_over > done) return 1;
    int n = 0; for (int i = 0; i < g_ngw; i++) if (g_gw[i].fence > done) g_gw[n++] = g_gw[i]; g_ngw = n;
    for (int i = 0; i < g_ngw; i++) if (lo < g_gw[i].hi && g_gw[i].lo < hi) return 1;
    return 0;
}
static void gw_add(uint64_t lo, uint64_t hi) {   /* a GPU write of guest memory recorded in the open submission (adjacent writes of one submission merge) */
    if (!(ubo_snapshot() || pool_shadow()) || lo >= hi) return;
    uint64_t f = vk_next_fence();
    if (g_ngw && g_gw[g_ngw - 1].fence == f && lo <= g_gw[g_ngw - 1].hi && g_gw[g_ngw - 1].lo <= hi) {
        if (lo < g_gw[g_ngw - 1].lo) g_gw[g_ngw - 1].lo = lo; if (hi > g_gw[g_ngw - 1].hi) g_gw[g_ngw - 1].hi = hi; return; }
    if (g_ngw == GW_MAX) gw_hits(0, 0);
    if (g_ngw < GW_MAX) g_gw[g_ngw++] = (typeof(g_gw[0])){lo, hi, f}; else g_gw_over = f;
}
static void gw_note(uint64_t lo, uint64_t hi) {   /* and the copies of this submission it overlaps are dropped */
    if (!(ubo_snapshot() || pool_shadow()) || lo >= hi) return;
    gw_add(lo, hi);
    for (int i = 0; i < 1024; i++) if (g_shadow_ep[i] == g_shadow_epoch && g_shadow[i].addr < hi && lo < g_shadow[i].addr + g_shadow[i].size) { g_shadow_ep[i] = 0; g_usnap[2]++; }
}
static int ubo_range(uint64_t addr, uint64_t size, VkBuffer *buf, VkDeviceSize *off, VkDeviceSize *range) {
    if (!addr) return 0;
    if (!size || size > 65536) size = 65536;
    return range_copy(addr, size, !(pool_shadow() || ubo_snapshot()), buf, off, range);
}
/* a buffer range as the GPU would read it here; direct: the game's memory itself when no pending update overlaps it */
static int range_copy(uint64_t addr, uint64_t size, int direct, VkBuffer *buf, VkDeviceSize *off, VkDeviceSize *range) {
    int overlap = 0;   /* 1 + the last pending update that overlaps the range */
    for (int i = g_nupd - 1; i >= 0 && !overlap; i--) if (g_upd[i].dst < addr + size && addr < g_upd[i].dst + g_upd[i].n) overlap = i + 1;
    if (!direct && gw_hits(addr, addr + size)) { direct = 1; g_usnap[1]++; } else if (!direct) g_usnap[0]++;
    VkBuffer b; VkDeviceSize o;
    if (!overlap && direct && vk_pool_find(addr, size, &b, &o) && !(o & 255)) { *buf = b; *off = o; *range = size; return 1; }
    if (!direct) g_pshadow[0]++;
    if (!vk_pool_find(addr, 1, &b, &o)) return 0;
    unsigned slot = (unsigned)((addr >> 8) ^ size) & 1023;
    shadow_t *sh = &g_shadow[slot];
    if (g_shadow_ep[slot] == g_shadow_epoch && sh->addr == addr && sh->size == size && sh->nupd == overlap) { *buf = sh->buf; *off = sh->off; *range = size; return 1; }
    /* the range's previous copy in this submission (fewer updates) is the base: only the newer updates are applied (a ring
       range of 64 KiB gets ~1,000 inline updates in some Shrine submissions) */
    int from = 0; const uint8_t *base = (const uint8_t *)(uintptr_t)addr;
    if (g_shadow_ep[slot] == g_shadow_epoch && sh->addr == addr && sh->size == size && sh->nupd < overlap) { from = sh->nupd; base = sh->cpu; }
    vk_span s = vk_ring(size, 256); memcpy(s.cpu, base, size); g_ucopy[0]++; g_ucopy[1] += size; g_ucopy_all[0]++; g_ucopy_all[1] += size;
    for (int i = from; i < overlap; i++) {
        uint64_t lo = g_upd[i].dst > addr ? g_upd[i].dst : addr, hi = g_upd[i].dst + g_upd[i].n < addr + size ? g_upd[i].dst + g_upd[i].n : addr + size;
        if (lo < hi) memcpy(s.cpu + (lo - addr), g_upd[i].data + (lo - g_upd[i].dst), hi - lo);
    }
    *buf = s.buf; *off = s.off; *range = size;
    *sh = (shadow_t){addr, size, overlap, s.buf, s.off, s.cpu}; g_shadow_ep[slot] = g_shadow_epoch;
    return 1;
}

/* ---------- shader pairs and descriptors ---------- */
typedef struct pair_t { vshader *vs, *fs; VkShaderEXT sh[2]; VkPipelineLayout pl; struct pair_t *next; } pair_t;
static pair_t *g_pairs[1024];
typedef struct comp_t { vshader *cs; VkShaderEXT sh; VkPipelineLayout pl; struct comp_t *next; } comp_t;
static uint64_t g_mk[2];   /* shader objects created, ns (long-frame log) */
static comp_t *g_comps[256];
static VkShaderEXT make_shader(vshader *s, VkShaderStageFlagBits stage, VkShaderStageFlags next, VkDescriptorSetLayout *layouts, int nl) {
    VkShaderCreateInfoEXT ci = {.sType = VK_STRUCTURE_TYPE_SHADER_CREATE_INFO_EXT, .stage = stage, .nextStage = next, .codeType = VK_SHADER_CODE_TYPE_SPIRV_EXT,
        .codeSize = s->nwords * 4, .pCode = s->spv, .pName = "main", .setLayoutCount = (uint32_t)nl, .pSetLayouts = layouts};
    uint64_t t0_ = clock_gettime_nsec_np(CLOCK_MONOTONIC); VkShaderEXT o; VkResult r = V.CreateShadersEXT(V.dev, 1, &ci, NULL, &o); g_mk[0]++; g_mk[1] += clock_gettime_nsec_np(CLOCK_MONOTONIC) - t0_;
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
/* a 32-bit word of the c2 bank (driver_bank) read from the bound state */
static uint32_t c2_word(int stage, int word) {
    int b = word * 4;
    if (b >= 0x20 && b < 0x120 && (b - 0x20) % 8 == 0) return (uint32_t)S.tex[stage][(b - 0x20) / 8];
    if (b >= 0x168 && b < 0x568 && (b - 0x168) % 8 == 0) return (uint32_t)S.tex[stage][(b - 0x168) / 8];
    if (b >= 0x568 && b < 0x968 && (b - 0x568) % 8 == 0) return (uint32_t)S.samp[stage][(b - 0x568) / 8];
    return 0;
}
/* the NVN handle a texture binding reads: Ryujinx TextureHandle.ReadPackedId over the c2 bank */
static uint32_t packed_handle(int stage, int handle) {
    int tw = handle & 0x3fff, sw = (handle >> 14) & 0x3fff, type = (unsigned)handle >> 28;
    uint32_t h = c2_word(stage, tw);
    if (type) {
        uint32_t s = 0;
        if (type != 3) s = c2_word(stage, sw); else s = (uint32_t)sw;
        if (type == 2 || type == 3) s <<= 20;
        h |= s;
    }
    return h;
}
/* an image binding's handle: c2 image handles at 0x120 + 8 * register */
static uint32_t img_handle(int stage, int handle) {
    int tw = handle & 0x3fff, reg = tw * 4 >= 0x120 ? (tw * 4 - 0x120) / 8 : -1;
    return reg >= 0 && reg < 32 ? (uint32_t)S.img[stage][reg] : 0;
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
static unsigned g_feedback;   /* the attachments of the open pass a draw samples (g_pass_written bits) */
static uint64_t g_fb_draws, g_fb_barriers, g_fb_why[8];   /* draws sampling written attachments: no fragment shader, local read off, depth, vertex stage, binding not plain 2D, none, eligible, SPIR-V not rewritable */
static int g_fbp_bind[8], g_fbp_att[8], g_nfbp, g_fb_bad;   /* fragment stage: texture bindings reading colour attachments as decals can (vk_fbread.c); g_fb_bad: one that cannot */
#define MAXW 160
static VkDescriptorSet stage_set(vshader *s, int stage, int set, VkDescriptorSetLayout layout) {
    VkWriteDescriptorSet w[MAXW]; VkDescriptorBufferInfo bi[MAXW]; VkDescriptorImageInfo ii[MAXW]; VkBufferView tb[MAXW]; int nw = 0, nb = 0, ni = 0, nt = 0;
    VkDescriptorSet ds = dset_alloc(layout);
    if (set == 0) { bi[nb] = (VkDescriptorBufferInfo){g_support, 0, 4096}; w[nw++] = (VkWriteDescriptorSet){.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = 0, .descriptorCount = 1, .descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, .pBufferInfo = &bi[nb++]}; }
    for (int i = 0; i < s->nb && nw < MAXW - 1; i++) {
        const vk_rbind *b = &s->b[i];
        if (b->set != set) continue;
        VkWriteDescriptorSet x = {.sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, .dstSet = ds, .dstBinding = b->binding, .descriptorCount = 1};
        if (b->kind == RB_CBUF) {
            VkBuffer buf = VK_NULL_HANDLE; VkDeviceSize off = 0, range = VK_WHOLE_SIZE;
            if (b->slot == 1) { buf = s->c1buf; range = VK_WHOLE_SIZE; }
            else if (b->slot == 0 || b->slot == 2) {   /* the bank is rebuilt when the stage's bindings changed (compute: every dispatch, it holds the grid) */
                static struct { uint64_t gen, sub; VkBuffer buf; VkDeviceSize off; } bc[6][2]; int k = b->slot / 2;
                if (stage == 5 || bc[stage][k].gen != g_gen_bind[stage] || bc[stage][k].sub != g_sub_gen) {
                    vk_span sp = vk_ring(0xA00, 256); driver_bank(sp.cpu, b->slot, stage); bc[stage][k].gen = g_gen_bind[stage]; bc[stage][k].sub = g_sub_gen; bc[stage][k].buf = sp.buf; bc[stage][k].off = sp.off; }
                buf = bc[stage][k].buf; off = bc[stage][k].off; range = 0xA00; }
            else if (b->slot >= 3 && b->slot <= 18) { const p_bind_buf *u = &S.ubo[stage][b->slot - 3];
                uint64_t sz = u->size && u->size <= 65536 ? u->size : 65536; if (b->used && b->used < sz) sz = b->used;   /* what the shader reads (cbuf_used) */
                if (!ubo_range(u->addr, sz, &buf, &off, &range)) buf = VK_NULL_HANDLE; }
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
                if (u->addr && u->size >= (uint64_t)off + 8) upd_read(u->addr + off, &addr, 8);
            }
            VkBuffer buf = VK_NULL_HANDLE; VkDeviceSize o = 0, range = VK_WHOLE_SIZE;
            if (addr) {
                uint64_t lo = addr & ~255ull; VkBuffer pb; VkDeviceSize po;
                if (vk_pool_find(lo, 1, &pb, &po)) { buf = pb; o = po; range = size ? (addr - lo) + size : VK_WHOLE_SIZE; }
                if (buf && size && !(b->flags & 1) && pool_shadow()) { VkDeviceSize n_ = (addr - lo) + size; if (n_ <= (16u << 20)) range_copy(lo, n_, 0, &buf, &o, &range); else g_pshadow[1]++; }
            }
            bi[nb] = (VkDescriptorBufferInfo){buf, o, range};
            x.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER; x.pBufferInfo = &bi[nb++];
            if (buf && (b->flags & 1)) gw_note(addr, addr + (size ? size : (16u << 20)));   /* written by the shader */
        } else if (b->kind == RB_IA) {   /* a decal variant's input attachment: the colour attachment itself */
            ii[ni] = (VkDescriptorImageInfo){VK_NULL_HANDLE, b->slot >= 0 && b->slot < 8 ? g_patt[b->slot].view : VK_NULL_HANDLE, VK_IMAGE_LAYOUT_GENERAL};
            x.descriptorType = VK_DESCRIPTOR_TYPE_INPUT_ATTACHMENT; x.pImageInfo = &ii[ni++];
        } else {
            int is_img = b->kind == RB_IMG;
            if ((b->stype & 0xff) == 2) {   /* texel buffers: a view over the buffer texture's pool memory */
                uint32_t h = is_img ? img_handle(stage, b->handle) : packed_handle(stage, b->handle);
                tex_t t; view_t vw; int hv, tid = (int)(h & 0xfffff);
                tb[nt] = tid && vk_tex_registry_get(tid, &t, &vw, &hv) ? vk_tex_bufview(&t, hv && vw.set_format ? vw.format : t.d.format) : VK_NULL_HANDLE;
                x.descriptorType = is_img ? VK_DESCRIPTOR_TYPE_STORAGE_TEXEL_BUFFER : VK_DESCRIPTOR_TYPE_UNIFORM_TEXEL_BUFFER;
                x.pTexelBufferView = &tb[nt++]; w[nw++] = x; continue;
            }
            x.descriptorType = is_img ? VK_DESCRIPTOR_TYPE_STORAGE_IMAGE : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            int count = b->array; if (ni + count > MAXW) count = MAXW - ni;
            x.descriptorCount = (uint32_t)count; x.pImageInfo = &ii[ni];
            for (int k = 0; k < count; k++) {
                uint32_t h;
                if (is_img) h = img_handle(stage, b->handle);
                else h = packed_handle(stage, b->handle + k * 2);
                int tid = (int)(h & 0xfffff), sid = (int)(h >> 20) & 0xfff;
                tex_t t; view_t vw; int hv; VkImageView view = VK_NULL_HANDLE; static VkSampler dflt; if (!dflt) dflt = vk_sampler(&g_default_samp); VkSampler smp = dflt;
                if (tid && vk_tex_registry_get(tid, &t, &vw, &hv)) {
                    vtex *v = vk_tex_get(&t);
                    if (v) {
                        vk_tex_upload_if_stale(v, &t); vk_tex_mark_read(v);
                        if (g_npclr) for (int i = 0; i < g_npclr; i++) g_pclr_hit |= g_pclr[i].v == v;
                        if (is_img) vk_tex_gpu_wrote(v);
                        view = vk_tex_view(v, &t, &vw, hv, view_type(b->stype), 0);
                        fmt_log(is_img ? "image" : "sampled", &t, hv && vw.set_format ? vw.format : t.d.format, v, stage == 0 ? "vertex" : stage == 5 ? "compute" : NULL);
                        /* the sampled levels / layers include an attachment's own: the pass must land first */
                        int l0 = hv && vw.set_levels ? vw.base_level : 0, nl = hv && vw.set_levels && vw.nlevels ? vw.nlevels : 64;
                        int y0 = hv && vw.set_layers ? vw.min_layer : 0, ny = hv && vw.set_layers && vw.nlayers ? vw.nlayers : 4096;
                        for (int a = 0; a < 9; a++) { const att_t *at = a < 8 ? &g_patt[a] : &g_pdepth;
                            if (at->v == v && at->level >= l0 && at->level < l0 + nl && at->layer >= y0 && at->layer < y0 + ny) { g_feedback |= 1u << a;
                                /* a decal read (vk_fbread.c): a plain 2D colour texture whose view is the attachment itself */
                                int ok = stage == 1 && a < 8 && !is_img && count == 1 && view_type(b->stype) == VK_IMAGE_VIEW_TYPE_2D && !at->level && !at->layer && !l0 && !y0 &&
                                         !(hv && vw.set_format && vw.format != t.d.format) && !(S.rt.has_cview[a] && S.rt.cview[a].set_format && S.rt.cview[a].format != t.d.format);
                                if (ok && g_nfbp < 8) { g_fbp_bind[g_nfbp] = b->binding; g_fbp_att[g_nfbp++] = a; } else g_fb_bad = 1; } }
                    }
                }
                if (!is_img && sid) { VkSampler s_ = vk_sampler_id(sid); if (s_) smp = s_; }
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
static void answers(int st, char *out, size_t cap, vk_answers *A) {
    size_t o = 0; int first; memset(A, 0, sizeof *A);
#define PUT(...) do { int k_ = snprintf(out + o, o < cap ? cap - o : 0, __VA_ARGS__); if (k_ > 0) o += (size_t)k_; } while (0)
    tex_t t; view_t v; int hv;
    PUT("\"tex\": {"); first = 1;
    for (int r = 0; r < 128; r++) if (S.tex[st][r] && vk_tex_registry_get((int)(S.tex[st][r] & 0xfffff), &t, &v, &hv)) {
        int tg = hv && v.set_target ? v.target : t.d.target; PUT("%s\"%d\": \"%s\"", first ? "" : ", ", r, target_name(tg)); A->tex[r] = target_name(tg); first = 0; }
    PUT("}, \"img\": {"); first = 1;
    for (int r = 0; r < 32; r++) if (S.img[st][r] && vk_tex_registry_get((int)(S.img[st][r] & 0xfffff), &t, &v, &hv)) {
        int tg = hv && v.set_target ? v.target : t.d.target, f = hv && v.set_format ? v.format : t.d.format; const char *fn = image_format(f);
        if (fn) PUT("%s\"%d\": {\"type\": \"%s\", \"fmt\": \"%s\"}", first ? "" : ", ", r, target_name(tg), fn); else PUT("%s\"%d\": {\"type\": \"%s\"}", first ? "" : ", ", r, target_name(tg));
        A->img_type[r] = target_name(tg); A->img_fmt[r] = fn;
        first = 0; }
    PUT("}, \"attr\": {"); first = 1;
    if (st == 0) for (int i = 0; i < S.nattr && i < 16; i++) { A->attr[i] = fmt_class(((va_st *)&S.attr[i])->fmt); PUT("%s\"%d\": \"%s\"", first ? "" : ", ", i, A->attr[i]); first = 0; }
    PUT("}, \"fout\": {"); first = 1;
    if (st == 1 && S.has_rt) for (int i = 0; i < S.rt.n && i < 8; i++) if (S.rt.color[i]) {
        A->fout[i] = fmt_class(S.rt.has_cview[i] && S.rt.cview[i].set_format ? S.rt.cview[i].format : S.rt.color[i]->d.format);
        PUT("%s\"%d\": \"%s\"", first ? "" : ", ", i, A->fout[i]); first = 0; }
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
    tex_t t; view_t v; int hv;
    for (int r = 0; r < 128; r++) if (S.tex[st][r]) {   /* a bound slot's target is what the translation depends on, not the handle */
        uint8_t k[2] = {(uint8_t)r, 0xff};
        if (vk_tex_registry_get((int)(S.tex[st][r] & 0xfffff), &t, &v, &hv)) k[1] = (uint8_t)(hv && v.set_target ? v.target : t.d.target);
        MIX(k, 2);
    }
    for (int r = 0; r < 32; r++) if (S.img[st][r]) {
        uint8_t k[3] = {(uint8_t)r, 0xff, 0};
        if (vk_tex_registry_get((int)(S.img[st][r] & 0xfffff), &t, &v, &hv)) { k[1] = (uint8_t)(hv && v.set_target ? v.target : t.d.target); k[2] = (uint8_t)(hv && v.set_format ? v.format : t.d.format); }
        MIX(k, 3);
    }
    if (st == 0) { MIX(&S.nattr, sizeof S.nattr); for (int i = 0; i < S.nattr && i < 16; i++) { uint8_t f = ((va_st *)&S.attr[i])->fmt; MIX(&f, 1); } }
    if (st == 1 && S.has_rt) for (int i = 0; i < S.rt.n && i < 8; i++) { int f = S.rt.color[i] ? (S.rt.has_cview[i] && S.rt.cview[i].set_format ? S.rt.cview[i].format : S.rt.color[i]->d.format) : 0; MIX(&f, sizeof f); }
#undef MIX
    return h;
}
static vshader *stage_shader_slow(int want);
static struct { const prog_t *p; uint64_t serial, gen, reg, rt; vshader *s; } g_last_sh[6];
static uint64_t g_gen_rt;
static vshader *stage_shader(int want) {
    const prog_t *p = S.prog; if (!p) return NULL;
    uint64_t reg = vk_tex_registry_gen();
    if (g_last_sh[want].p == p && g_last_sh[want].serial == p->serial && g_last_sh[want].gen == g_gen_bind[want] + (want == 0 ? g_gen_vtx : 0) && g_last_sh[want].reg == reg && g_last_sh[want].rt == g_gen_rt) return g_last_sh[want].s;
    uint64_t sig = state_sig(want); unsigned h = (unsigned)(((uintptr_t)p >> 4) ^ p->serial ^ (sig >> 7) ^ (uint64_t)want) & 4095;
    vshader *s = NULL;
    for (memo_t *m = g_memo[h]; m && !s; m = m->next) if (m->p == p && m->serial == p->serial && m->stage == want && m->sig == sig) s = m->s;
    if (!s) { s = stage_shader_slow(want); memo_t *m = malloc(sizeof *m); *m = (memo_t){p, p->serial, want, sig, s, g_memo[h]}; g_memo[h] = m; }
    g_last_sh[want].p = p; g_last_sh[want].serial = p->serial; g_last_sh[want].gen = g_gen_bind[want] + (want == 0 ? g_gen_vtx : 0); g_last_sh[want].reg = reg; g_last_sh[want].rt = g_gen_rt; g_last_sh[want].s = s;
    return s;
}
static vshader *stage_shader_slow(int want) {
    const prog_t *p = S.prog; if (!p) return NULL;
    for (unsigned i = 0; i < p->nstage && i < 6; i++) {
        const uint8_t *control = p->sd[i].control; if (!control || !p->sd[i].data) continue;
        uint32_t st; memcpy(&st, control + 0x714, 4);
        if ((int)st != want) continue;
        char a[8192]; vk_answers A; answers(want, a, sizeof a, &A);
        return vk_shader_get(control, (const uint8_t *)(uintptr_t)p->sd[i].data, want, a, &A);
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

/* state set in the command buffer being recorded: a command is emitted only when its arguments differ from the last ones
   (by a hash of them), so the ICD encodes no repeated state */
enum { D_VP, D_SC, D_RD, D_CULL, D_FF, D_PM, D_BIASEN, D_BIAS, D_LW, D_DT, D_DW, D_DCMP, D_DBT, D_ST, D_SOPF, D_SOPB, D_SCM, D_SWM, D_SREF,
       D_CLAMP, D_RS, D_SM, D_A2C, D_LOE, D_BC, D_PR, D_CLIP, D_BEN, D_BEQ, D_WM, D_TOPO, D_SHADERS, D_SETS, D_IB, D_COUNT };
static uint64_t g_dyn_hash[D_COUNT], g_dyn_cb;
static uint64_t g_dyn_emitted, g_dyn_skipped;
static int dyn_changed(int id, const void *p, size_t n) {
    if (g_dyn_cb != g_cb_gen) { memset(g_dyn_hash, 0, sizeof g_dyn_hash); g_dyn_cb = g_cb_gen; }
    uint64_t h = 1469598103934665603ull; const uint8_t *b = p; for (size_t i = 0; i < n; i++) h = (h ^ b[i]) * 1099511628211ull; h |= 1;
    if (g_dyn_hash[id] == h) { g_dyn_skipped++; return 0; }
    g_dyn_hash[id] = h; g_dyn_emitted++; return 1;
}
static void set_dynamic(VkCommandBuffer cb) {
    /* viewports: NVN's upper-left origin with the y axis as Metal's (a negative-height Vulkan viewport) */
    VkViewport vp[16]; int nvp = S.nvp ? S.nvp : 1;
    memset(vp, 0, sizeof vp);
    for (int i = 0; i < nvp; i++) {
        float x = S.nvp ? S.vp[i][0] : 0, y = S.nvp ? S.vp[i][1] : 0, w = S.nvp ? S.vp[i][2] : (float)g_pw, h = S.nvp ? S.vp[i][3] : (float)g_ph;
        if (w == 0) w = 1; if (h == 0) h = 1;
        vp[i] = (VkViewport){x, y + h, w, -h, S.dr[0], S.dr[1]};
        if (vp[i].minDepth < 0) vp[i].minDepth = 0; if (vp[i].minDepth > 1) vp[i].minDepth = 1; if (vp[i].maxDepth < 0) vp[i].maxDepth = 0; if (vp[i].maxDepth > 1) vp[i].maxDepth = 1;
    }
    if (dyn_changed(D_VP, vp, sizeof(VkViewport) * nvp)) vkCmdSetViewportWithCount(cb, (uint32_t)nvp, vp);
    VkRect2D sc[16];
    for (int i = 0; i < nvp; i++) {
        int32_t x0 = 0, y0 = 0, x1 = (int32_t)g_pw, y1 = (int32_t)g_ph;
        if (S.has_sc) { x0 = S.sc.x > 0 ? S.sc.x : 0; y0 = S.sc.y > 0 ? S.sc.y : 0; int64_t ex = (int64_t)S.sc.x + S.sc.w, ey = (int64_t)S.sc.y + S.sc.h; if (ex < x1) x1 = (int32_t)ex; if (ey < y1) y1 = (int32_t)ey; }
        if (x1 < x0) x1 = x0; if (y1 < y0) y1 = y0;
        sc[i] = (VkRect2D){{x0, y0}, {(uint32_t)(x1 - x0), (uint32_t)(y1 - y0)}};
    }
    if (dyn_changed(D_SC, sc, sizeof(VkRect2D) * nvp)) vkCmdSetScissorWithCount(cb, (uint32_t)nvp, sc);
#define ONE(id, T, val, call) do { T v_ = (val); if (dyn_changed(id, &v_, sizeof v_)) call; } while (0)
    ONE(D_RD, VkBool32, S.raster_discard, vkCmdSetRasterizerDiscardEnable(cb, v_));
    ONE(D_CULL, VkCullModeFlags, S.poly.cull == NVN_FACE_FRONT ? VK_CULL_MODE_FRONT_BIT : S.poly.cull == NVN_FACE_BACK ? VK_CULL_MODE_BACK_BIT : S.poly.cull == NVN_FACE_FRONT_AND_BACK ? VK_CULL_MODE_FRONT_AND_BACK : VK_CULL_MODE_NONE, vkCmdSetCullMode(cb, v_));
    ONE(D_FF, VkFrontFace, ((S.poly.front == NVN_FRONT_FACE_CCW) != winding_flip()) ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE, vkCmdSetFrontFace(cb, v_));
    ONE(D_PM, VkPolygonMode, S.poly.mode == NVN_POLYGON_MODE_LINE ? VK_POLYGON_MODE_LINE : S.poly.mode == NVN_POLYGON_MODE_POINT ? VK_POLYGON_MODE_POINT : VK_POLYGON_MODE_FILL, V.CmdSetPolygonModeEXT(cb, v_));
    ONE(D_BIASEN, VkBool32, (S.poly.offset_en & NVN_POLYGON_OFFSET_ENABLE_FILL) != 0, vkCmdSetDepthBiasEnable(cb, v_));
    { float bias[3] = {S.po.units, S.po.clamp, S.po.factor}; if (dyn_changed(D_BIAS, bias, sizeof bias)) vkCmdSetDepthBias(cb, bias[0], bias[1], bias[2]); }
    ONE(D_LW, float, 1.0f, vkCmdSetLineWidth(cb, v_));
    int has_d = !g_pass_nodepth && g_pdepth.v && (vk_tex_aspect(g_pdepth.v) & VK_IMAGE_ASPECT_DEPTH_BIT), has_s = !g_pass_nodepth && depth_has_stencil();
    ONE(D_DT, VkBool32, has_d && S.ds.dt, vkCmdSetDepthTestEnable(cb, v_));
    ONE(D_DW, VkBool32, has_d && S.ds.dw, vkCmdSetDepthWriteEnable(cb, v_));
    ONE(D_DCMP, VkCompareOp, cmp(S.ds.df), vkCmdSetDepthCompareOp(cb, v_));
    ONE(D_DBT, VkBool32, 0, vkCmdSetDepthBoundsTestEnable(cb, v_));
    ONE(D_ST, VkBool32, has_s && S.ds.st, vkCmdSetStencilTestEnable(cb, v_));
    { VkStencilOp o[4] = {sop(S.ds.fail_f), sop(S.ds.zpass_f), sop(S.ds.zfail_f), (VkStencilOp)cmp(S.ds.sf_f)}; if (dyn_changed(D_SOPF, o, sizeof o)) vkCmdSetStencilOp(cb, VK_STENCIL_FACE_FRONT_BIT, o[0], o[1], o[2], (VkCompareOp)o[3]); }
    { VkStencilOp o[4] = {sop(S.ds.fail_b), sop(S.ds.zpass_b), sop(S.ds.zfail_b), (VkStencilOp)cmp(S.ds.sf_b)}; if (dyn_changed(D_SOPB, o, sizeof o)) vkCmdSetStencilOp(cb, VK_STENCIL_FACE_BACK_BIT, o[0], o[1], o[2], (VkCompareOp)o[3]); }
    if (dyn_changed(D_SCM, S.st_vmask, sizeof S.st_vmask)) { vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)S.st_vmask[0]); vkCmdSetStencilCompareMask(cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)S.st_vmask[1]); }
    if (dyn_changed(D_SWM, S.st_wmask, sizeof S.st_wmask)) { vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)S.st_wmask[0]); vkCmdSetStencilWriteMask(cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)S.st_wmask[1]); }
    if (dyn_changed(D_SREF, S.st_ref, sizeof S.st_ref)) { vkCmdSetStencilReference(cb, VK_STENCIL_FACE_FRONT_BIT, (uint32_t)S.st_ref[0]); vkCmdSetStencilReference(cb, VK_STENCIL_FACE_BACK_BIT, (uint32_t)S.st_ref[1]); }
    ONE(D_CLAMP, VkBool32, S.depth_clamp, V.CmdSetDepthClampEnableEXT(cb, v_));
    ONE(D_RS, VkSampleCountFlagBits, VK_SAMPLE_COUNT_1_BIT, V.CmdSetRasterizationSamplesEXT(cb, v_));
    { VkSampleMask sm = ~0u; if (dyn_changed(D_SM, &sm, sizeof sm)) V.CmdSetSampleMaskEXT(cb, VK_SAMPLE_COUNT_1_BIT, &sm); }
    ONE(D_A2C, VkBool32, S.ms.a2c && S.ms.enable, V.CmdSetAlphaToCoverageEnableEXT(cb, v_));
    ONE(D_LOE, VkBool32, 0, V.CmdSetLogicOpEnableEXT(cb, v_));
    if (dyn_changed(D_BC, S.blend_color, sizeof S.blend_color)) vkCmdSetBlendConstants(cb, S.blend_color);
    ONE(D_PR, VkBool32, 0, vkCmdSetPrimitiveRestartEnable(cb, v_));
    if (V.CmdSetDepthClipEnableEXT) ONE(D_CLIP, VkBool32, !S.depth_clamp, V.CmdSetDepthClipEnableEXT(cb, v_));
#undef ONE
    int n = g_pcount ? g_pcount : 1;
    VkBool32 en[8]; VkColorBlendEquationEXT eq[8]; VkColorComponentFlags wm[8];
    for (int i = 0; i < n; i++) {
        const blend_st *b = &S.blend[i];
        en[i] = (S.color.blend_en >> i) & 1;
        eq[i] = (VkColorBlendEquationEXT){blend_factor(b->src_rgb), blend_factor(b->dst_rgb), blend_op(b->eq_rgb), blend_factor(b->src_a), blend_factor(b->dst_a), blend_op(b->eq_a)};
        wm[i] = (S.chmask.mask >> (4 * i)) & 0xf;
        if (g_pslot[i] < 0 || !g_patt[i].v) { en[i] = 0; wm[i] = 0; }
    }
    if (dyn_changed(D_BEN, en, n * sizeof *en)) V.CmdSetColorBlendEnableEXT(cb, 0, (uint32_t)n, en);
    if (dyn_changed(D_BEQ, eq, n * sizeof *eq)) V.CmdSetColorBlendEquationEXT(cb, 0, (uint32_t)n, eq);
    if (dyn_changed(D_WM, wm, n * sizeof *wm)) V.CmdSetColorWriteMaskEXT(cb, 0, (uint32_t)n, wm);
    { uint64_t h_ = 1469598103934665603ull; const uint8_t *q_[3] = {(const uint8_t *)en, (const uint8_t *)eq, (const uint8_t *)wm}; size_t l_[3] = {n * sizeof *en, n * sizeof *eq, n * sizeof *wm};
      for (int j = 0; j < 3; j++) for (size_t i = 0; i < l_[j]; i++) h_ = (h_ ^ q_[j][i]) * 1099511628211ull; for (int i = 0; i < n; i++) if (!en[i]) h_ ^= (uint64_t)i * 0x9e3779b97f4a7c15ull; g_blend_hash = h_; }
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
            if (b && sz != VK_WHOLE_SIZE && pool_shadow()) { if (sz <= (16u << 20)) range_copy(S.vbuf[s].address, sz, 0, &b, &o, &sz); else g_pshadow[1]++; }
        }
        if (!b) { vk_span z = vk_ring(64, 16); memset(z.cpu, 0, 64); b = z.buf; o = z.off; sz = 64; stride = 0; div = 0; }
        bd[nbd++] = (VkVertexInputBindingDescription2EXT){.sType = VK_STRUCTURE_TYPE_VERTEX_INPUT_BINDING_DESCRIPTION_2_EXT, .binding = (uint32_t)s, .stride = stride,
            .inputRate = div ? VK_VERTEX_INPUT_RATE_INSTANCE : VK_VERTEX_INPUT_RATE_VERTEX, .divisor = div ? div : 1};
        bufs[nbd - 1] = b; offs[nbd - 1] = o; sizes[nbd - 1] = sz; strides[nbd - 1] = stride;
        vkCmdBindVertexBuffers2(cb, (uint32_t)s, 1, &bufs[nbd - 1], &offs[nbd - 1], &sizes[nbd - 1], NULL);
    }
    (void)strides;
    V.CmdSetVertexInputEXT(cb, (uint32_t)nbd, bd, (uint32_t)nad, ad);
    { uint64_t h_ = 1469598103934665603ull; for (int i = 0; i < nbd; i++) { uint32_t v_[4] = {bd[i].binding, bd[i].stride, bd[i].inputRate, bd[i].divisor}; for (int b = 0; b < 16; b++) h_ = (h_ ^ ((const uint8_t *)v_)[b]) * 1099511628211ull; }
      for (int i = 0; i < nad; i++) { uint32_t v_[4] = {ad[i].location, ad[i].binding, (uint32_t)ad[i].format, ad[i].offset}; for (int b = 0; b < 16; b++) h_ = (h_ ^ ((const uint8_t *)v_)[b]) * 1099511628211ull; } g_vi_hash = h_; }
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
/* the pass-time frame's pipelines: per pass the (vertex, fragment) shaders with their draw counts; for a pass of one or two
   draws also the fragment textures and the first words of its uniform buffers (pt_frame logs them) */
#define PP_MAX 4096
static struct { int pass; vshader *vs, *fs; int draws; } g_pp[PP_MAX]; static int g_npp;
static void pp_note(vshader *vs, vshader *fs) {
    for (int i = g_npp - 1; i >= 0 && g_pp[i].pass == g_pt_n; i--) if (g_pp[i].vs == vs && g_pp[i].fs == fs) { g_pp[i].draws++; return; }
    if (g_npp == PP_MAX) return;
    g_pp[g_npp++] = (typeof(g_pp[0])){g_pt_n, vs, fs, 1};
    if (!fs || g_pt[g_pt_n].draws > 1) return;
    for (int i = 0; i < fs->nb; i++) { const vk_rbind *b = &fs->b[i];
        if (b->kind == RB_TEX) { uint32_t h = packed_handle(1, b->handle); tex_t t; view_t vw; int hv;
            if ((h & 0xfffff) && vk_tex_registry_get((int)(h & 0xfffff), &t, &vw, &hv)) VKLOG("    pass #%d binding %u: texture fmt %#x %ux%ux%u levels %u target %d (view fmt %#x levels %d+%d)", g_pt_n, b->binding,
                t.d.format, t.d.width, t.d.height, t.d.depth, t.d.levels, t.d.target, hv && vw.set_format ? vw.format : t.d.format, hv && vw.set_levels ? vw.base_level : 0, hv && vw.set_levels ? vw.nlevels : 0); }
        else if (b->kind == RB_CBUF && b->slot >= 3 && b->slot <= 18) { const p_bind_buf *u = &S.ubo[1][b->slot - 3]; uint32_t v[32] = {0};
            if (u->addr) upd_read(u->addr, v, sizeof v);
            char s[512]; int o = 0; for (int k = 0; k < 32; k++) o += snprintf(s + o, sizeof s - o, " %08x", v[k]);
            VKLOG("    pass #%d binding %u: uniform c%d (%llu bytes, shader reads %u):%s", g_pt_n, b->binding, b->slot - 3, (unsigned long long)u->size, b->used, s); } }
}
static void draw(const p_draw *d, const p_draw_idx *di) {
    if (S.render_disabled) { g_skips[1]++; return; }
    vshader *vs = stage_shader(0), *fs = stage_shader(1);
    if (!vs) { g_skips[2]++; g_q_skipped |= g_q_counting; return; }
    if (!pass_begin()) return;
    g_feedback = 0;
    /* a stage's set is reused while its shader, its bindings, the registry, the targets and the submission are the same */
    static struct { vshader *s; uint64_t gen, reg, sub, rt; VkDescriptorSet set; unsigned fb; int fbp_bind[8], fbp_att[8], nfbp, fb_bad; } last[2];
    uint64_t reg = vk_tex_registry_gen(); VkDescriptorSet sets[2];
    for (int k = 0; k < 2; k++) {
        vshader *s = k ? fs : vs;
        if (last[k].s == s && last[k].gen == g_gen_bind[k] && last[k].reg == reg && last[k].sub == g_sub_gen && last[k].rt == g_gen_rt && s) { sets[k] = last[k].set; g_feedback |= last[k].fb; continue; }
        unsigned f0 = g_feedback; g_feedback = 0; g_nfbp = 0; g_fb_bad = 0;
        sets[k] = s ? stage_set(s, k, k, s->layout) : dset_alloc(V.empty_layout);
        last[k].s = s; last[k].gen = g_gen_bind[k]; last[k].reg = reg; last[k].sub = g_sub_gen; last[k].rt = g_gen_rt; last[k].set = sets[k]; last[k].fb = g_feedback;
        memcpy(last[k].fbp_bind, g_fbp_bind, sizeof g_fbp_bind); memcpy(last[k].fbp_att, g_fbp_att, sizeof g_fbp_att); last[k].nfbp = g_nfbp; last[k].fb_bad = g_fb_bad;
        g_feedback |= f0;
    }
    /* the draw samples attachments this pass wrote: a decal reads them at its own pixel through local read (vk_fbread.c, a
       BY_REGION barrier after the writes), as nvn_mtl.m att_fb_take; when its shader cannot, the pass splits (a run of such
       draws shares the split). TOTK_VK_FB=local / split (below) */
    /* TOTK_VK_FB=local: decals read through local read; default split (the ICD returned zeros from the input attachments, r49f: black stairs on the Plateau) */
    static int lr = -1; if (lr < 0) { const char *e = getenv("TOTK_VK_FB"); lr = e && !strcmp(e, "local"); }
    unsigned fbw = g_feedback & g_pass_written, fbm = 0; vshader *dfs = NULL;
    if (fbw) g_fb_why[!fs ? 0 : !lr ? 1 : (fbw & 0x100) ? 2 : (last[0].fb & fbw) ? 3 : last[1].fb_bad ? 4 : !last[1].nfbp ? 5 : 6]++;
    if (fbw && fs && lr && !last[1].fb_bad && !(fbw & 0x100) && !(last[0].fb & fbw) && last[1].nfbp) {
        dfs = vk_shader_fbread(fs, last[1].fbp_bind, last[1].fbp_att, last[1].nfbp);
        if (!dfs) g_fb_why[7]++;
        if (dfs) { for (int i = 0; i < last[1].nfbp; i++) fbm |= 1u << last[1].fbp_att[i];
                   unsigned f = g_feedback; sets[1] = stage_set(dfs, 1, 1, dfs->layout); g_feedback = f; fs = dfs; g_fb_draws++; }
    }
    pair_t *p = pair_get(vs, fs);
    if (!p->sh[0] || (fs && !p->sh[1])) { g_skips[3]++; g_q_skipped |= g_q_counting; return; }
    static int each = -1; if (each < 0) { const char *e = getenv("TOTK_VK_FB_DRAW"); each = e && atoi(e); }
    static unsigned prev_fb;
    if (!g_pass_open && !pass_begin()) return;   /* ended for an image made mutable while the sets were built */
    if (g_pclr_hit) { if (g_pass_recorded) { end_pass(6); pclr_flush(); if (!pass_begin()) return; } else pclr_flush(); }   /* a pending clear of an image this draw samples runs first */
    if (!dfs && fbw && (each || !prev_fb)) { pass_record(); end_pass(7); if (!pass_begin()) return; }
    if (g_pass_recorded && g_pass_nodepth && (S.ds.dt || S.ds.dw || S.ds.st)) { end_pass(5); if (!pass_begin()) return; }   /* the pass left depth out */
    pass_record();
    if (dfs && (g_lr_dirty & fbm)) {
        VkMemoryBarrier2 mb = {.sType = VK_STRUCTURE_TYPE_MEMORY_BARRIER_2, .srcStageMask = VK_PIPELINE_STAGE_2_COLOR_ATTACHMENT_OUTPUT_BIT, .srcAccessMask = VK_ACCESS_2_COLOR_ATTACHMENT_WRITE_BIT,
            .dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, .dstAccessMask = VK_ACCESS_2_INPUT_ATTACHMENT_READ_BIT};
        VkDependencyInfo di = {.sType = VK_STRUCTURE_TYPE_DEPENDENCY_INFO, .dependencyFlags = VK_DEPENDENCY_BY_REGION_BIT, .memoryBarrierCount = 1, .pMemoryBarriers = &mb};
        vkCmdPipelineBarrier2(vk_cb(), &di); g_lr_dirty = 0; g_fb_barriers++;
    }
    prev_fb = g_feedback;
    VkCommandBuffer cb = vk_cb();
    VkShaderStageFlagBits stages[5] = {VK_SHADER_STAGE_VERTEX_BIT, VK_SHADER_STAGE_FRAGMENT_BIT, VK_SHADER_STAGE_TESSELLATION_CONTROL_BIT, VK_SHADER_STAGE_TESSELLATION_EVALUATION_BIT, VK_SHADER_STAGE_GEOMETRY_BIT};
    VkShaderEXT sh[5] = {p->sh[0], fs ? p->sh[1] : VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE, VK_NULL_HANDLE};
    int ns = 2 + ((has_tess() & 1) ? 2 : 0); if (has_tess() & 2) { stages[ns] = VK_SHADER_STAGE_GEOMETRY_BIT; sh[ns] = VK_NULL_HANDLE; ns++; }
    if (!(has_tess() & 1) && (has_tess() & 2)) { stages[2] = VK_SHADER_STAGE_GEOMETRY_BIT; }
    if (dyn_changed(D_SHADERS, sh, sizeof sh)) V.CmdBindShadersEXT(cb, (uint32_t)ns, stages, sh);
    { uint64_t k_[3] = {(uint64_t)p->pl, (uint64_t)sets[0], (uint64_t)sets[1]}; if (dyn_changed(D_SETS, k_, sizeof k_)) vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_GRAPHICS, p->pl, 0, 2, sets, 0, NULL); }
    static uint64_t dyn_gen, dyn_sub, vtx_gen, vtx_sub; static vshader *vtx_vs;
    if (dyn_gen != g_gen_state || dyn_sub != g_cb_gen) { set_dynamic(cb); dyn_gen = g_gen_state; dyn_sub = g_cb_gen; }
    if (vtx_gen != g_gen_vtx || vtx_sub != g_cb_gen || vtx_vs != vs) { vertex_input(cb, vs); vtx_gen = g_gen_vtx; vtx_sub = g_cb_gen; vtx_vs = vs; }
    int prim = d ? d->prim : di->prim, inst = d ? (d->instances ? d->instances : 1) : (di->instances ? di->instances : 1);
    if (prim == NVN_DRAW_PRIMITIVE_QUADS || prim == NVN_DRAW_PRIMITIVE_QUAD_STRIP) {
        VkBuffer qb; VkDeviceSize qo; int n;
        if (!quad_indices(prim, di, d ? d->first : 0, d ? d->count : di->count, &qb, &qo, &n)) return;
        { VkPrimitiveTopology t_ = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; if (dyn_changed(D_TOPO, &t_, sizeof t_)) vkCmdSetPrimitiveTopology(cb, t_); }
        { uint64_t k_[3] = {(uint64_t)qb, qo, VK_INDEX_TYPE_UINT32}; if (dyn_changed(D_IB, k_, sizeof k_)) vkCmdBindIndexBuffer(cb, qb, qo, VK_INDEX_TYPE_UINT32); }
        vkCmdDrawIndexed(cb, (uint32_t)n, (uint32_t)inst, 0, di ? di->base_vertex : 0, (uint32_t)(d ? d->base_instance : di->base_instance));
    } else {
        { VkPrimitiveTopology t_ = topology(prim); if (dyn_changed(D_TOPO, &t_, sizeof t_)) vkCmdSetPrimitiveTopology(cb, t_); }
        if (d) vkCmdDraw(cb, (uint32_t)d->count, (uint32_t)inst, (uint32_t)d->first, (uint32_t)d->base_instance);
        else {
            VkBuffer ib; VkDeviceSize io;
            if (!vk_pool_find(di->ibuf, 1, &ib, &io)) { g_skips[4]++; return; }
            if (pool_shadow()) { VkDeviceSize n_ = (VkDeviceSize)di->count * (di->itype == 0 ? 1 : di->itype == 1 ? 2 : 4), r_; if (n_ && n_ <= (16u << 20)) range_copy(di->ibuf, n_, 0, &ib, &io, &r_); }
            { VkIndexType t_ = di->itype == 0 ? VK_INDEX_TYPE_UINT8_EXT : di->itype == 1 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32; uint64_t k_[3] = {(uint64_t)ib, io, t_};
              if (dyn_changed(D_IB, k_, sizeof k_)) vkCmdBindIndexBuffer(cb, ib, io, t_); }
            vkCmdDrawIndexed(cb, (uint32_t)di->count, (uint32_t)inst, 0, di->base_vertex, (uint32_t)di->base_instance);
        }
    }
    if (g_dump_first >= 0 && (long)g_presents == g_dump_first) {   /* TOTK_VK_DUMP frame: every draw with its target, blend, mask, viewport and fragment textures */
        char tx[512]; int o = 0; tx[0] = 0;
        for (int i = 0; fs && i < fs->nb && o < 440; i++) { const vk_rbind *b = &fs->b[i]; if (b->kind != RB_TEX) continue;
            uint32_t h = packed_handle(1, b->handle); tex_t t; view_t vw; int hv;
            if ((h & 0xfffff) && vk_tex_registry_get((int)(h & 0xfffff), &t, &vw, &hv)) o += snprintf(tx + o, sizeof tx - o, " b%u:s%llu/%#x/%ux%u/swz%d%d%d%d%s", b->binding, (unsigned long long)t.serial, hv && vw.set_format ? vw.format : t.d.format, t.d.width, t.d.height, hv && vw.set_swizzle ? vw.swz[0] : t.d.swizzle_r, hv && vw.set_swizzle ? vw.swz[1] : t.d.swizzle_g, hv && vw.set_swizzle ? vw.swz[2] : t.d.swizzle_b, hv && vw.set_swizzle ? vw.swz[3] : t.d.swizzle_a, t.mem ? "" : "/nomem");
                { vtex *tv_ = vk_tex_get(&t); if (o < 440) o += snprintf(tx + o, sizeof tx - o, "/st%d", vk_tex_state(tv_)); }
                { samp_par sp_; int sid_ = (int)(h >> 20) & 0xfff; if (sid_ && vk_samp_registry_get(sid_, &sp_) && o < 440) o += snprintf(tx + o, sizeof tx - o, "/samp%d:f%d%d lod %.1f..%.1f bias %.2f an %.0f", sid_, sp_.minf, sp_.magf, sp_.lod_min, sp_.lod_max, sp_.lod_bias, sp_.aniso); } }
        char va[400]; int vo = 0; va[0] = 0;   /* vertex attributes the shader reads: location:NVN format/stream+offset, stream stride / divisor */
        for (int l = 0; l < 16 && vo < 360; l++) if (vs && (vs->in_mask >> l & 1)) { va_st a_ = {0}; if (l < S.nattr) memcpy(&a_, &S.attr[l], 4);
            vo += snprintf(va + vo, sizeof va - vo, " %d:%#x/s%d+%d(st%u d%u)", l, a_.fmt, a_.stream, a_.offset, a_.stream < 16 ? S.stream[a_.stream].stride : 0, a_.stream < 16 ? S.stream[a_.stream].divisor : 0); }
        const blend_st *bl = &S.blend[0]; uint32_t aw_, ah_; VkFormat af_ = 0; if (g_patt[0].v) vk_tex_dims(g_patt[0].v, &aw_, &ah_, &af_);
        VKLOG("draw #%llu: pass %ux%u att0 %p fmt %d, vs %s fs %s, blend en %d rgb %d/%d/%d a %d/%d/%d mask %#x, vp %.0f,%.0f %.0fx%.0f, sc %d %d,%d %dx%d, depth t%d w%d, stencil %d, cull %d, prim %d count %d inst %d, attr%s, tex%s",
              (unsigned long long)g_probe_n, g_pw, g_ph, (void *)g_patt[0].v, (int)af_, vs ? vs->key : "-", fs ? fs->key : "-", S.color.blend_en & 1, bl->src_rgb, bl->dst_rgb, bl->eq_rgb, bl->src_a, bl->dst_a, bl->eq_a, S.chmask.mask & 0xf,
              S.nvp ? S.vp[0][0] : 0, S.nvp ? S.vp[0][1] : 0, S.nvp ? S.vp[0][2] : 0, S.nvp ? S.vp[0][3] : 0, S.has_sc, S.sc.x, S.sc.y, S.sc.w, S.sc.h, S.ds.dt, S.ds.dw, S.ds.st, S.poly.cull, prim, d ? d->count : di->count, inst, va, tx);
    }
    /* TOTK_VK_PROBE=<x>,<y>,<w>x<h> (with TOTK_VK_DUMP, its frame): after every draw into a pass of that size the pass is ended
       and the texel (x, y) of each colour attachment is copied out; logged per draw ("probe: draw n ...") on completion */
    if (g_dump_first >= 0 && (long)g_presents == g_dump_first && probe_on() && !g_probe_pass) probe_draw(1);
    { /* first uses of the ICD's link key (driver object.rs graphics(): shader objects, topology, blend / masks, attachment formats, vertex input) */
      VkPrimitiveTopology t_ = topology(prim); uint64_t k_[7] = {(uint64_t)p->sh[0], (uint64_t)sh[1], (uint64_t)t_, g_vi_hash, g_blend_hash, g_rt_hash, (uint64_t)(S.ms.a2c && S.ms.enable)};
      uint64_t h_ = 1469598103934665603ull; const uint8_t *q_ = (const uint8_t *)k_; for (size_t i = 0; i < sizeof k_; i++) h_ = (h_ ^ q_[i]) * 1099511628211ull;
      static uint64_t *set_; static size_t cap_ = 0, n_ = 0; if (!set_) { cap_ = 1u << 16; set_ = calloc(cap_, 8); }
      if (!h_) h_ = 1; size_t i_ = h_ & (cap_ - 1); while (set_[i_] && set_[i_] != h_) i_ = (i_ + 1) & (cap_ - 1);
      if (!set_[i_]) { set_[i_] = h_; n_++; g_links++;
          if (n_ * 2 > cap_) { uint64_t *o_ = set_; size_t oc_ = cap_; cap_ *= 2; set_ = calloc(cap_, 8); for (size_t j = 0; j < oc_; j++) if (o_[j]) { size_t k = o_[j] & (cap_ - 1); while (set_[k]) k = (k + 1) & (cap_ - 1); set_[k] = o_[j]; } free(o_); } } }
    g_q_draws += g_q_cur >= 0;
    if (g_q_cur >= 0 && g_q_open) { static int log = -1, n; if (log < 0) log = getenv("TOTK_VK_QUERY_LOG") != NULL;
        if (log && n++ < 40) VKLOG("counted draw: pass %ux%u colour %d depth %d (nodepth %d), colour mask %#x, depth test %d write %d func %d, stencil %d, cull %d, raster discard %d, vp %.0fx%.0f, prim %d count %d", g_pw, g_ph, g_pcount, g_pdepth.v != NULL, g_pass_nodepth,
            S.chmask.mask, S.ds.dt, S.ds.dw, S.ds.df, S.ds.st, S.poly.cull, S.raster_discard, S.nvp ? S.vp[0][2] : 0.f, S.nvp ? S.vp[0][3] : 0.f, d ? d->prim : di->prim, d ? d->count : di->count); }
    g_draws++; g_frame_draws++; if (g_pt_on && g_pt_n < PT_MAX) { g_pt[g_pt_n].draws++; pp_note(vs, fs); }
    if (!S.raster_discard) {
        for (int i = 0; i < g_pcount; i++) if (g_patt[i].v && (S.chmask.mask >> (4 * i) & 0xf)) { g_pass_written |= 1u << i; g_lr_dirty |= 1u << i; }
        if (g_pdepth.v && ((S.ds.dt && S.ds.dw) || (S.ds.st && (S.st_wmask[0] || S.st_wmask[1])))) { g_pass_written |= 1u << 8; g_lr_dirty |= 1u << 8; }
    }
}
static void dispatch(const p_dispatch *d) {
    vshader *cs = stage_shader(5); if (!cs) { g_skips[5]++; return; }
    end_pass(4); pclr_flush();
    comp_t *c = comp_get(cs); if (!c->sh) return;
    g_grid[0] = (uint32_t)d->x; g_grid[1] = (uint32_t)d->y; g_grid[2] = (uint32_t)d->z;
    VkDescriptorSet set = stage_set(cs, 5, 0, cs->layout);
    VkCommandBuffer cb = vk_cb();
    VkShaderStageFlagBits st = VK_SHADER_STAGE_COMPUTE_BIT; V.CmdBindShadersEXT(cb, 1, &st, &c->sh);
    vkCmdBindDescriptorSets(cb, VK_PIPELINE_BIND_POINT_COMPUTE, c->pl, 0, 1, &set, 0, NULL);
    if (g_pt_on && g_pt_n < PT_MAX) { g_pt[g_pt_n] = (pt_info){(uint32_t)d->x, (uint32_t)d->y, -1, 0, 0, 0, 0, 0}; pt_mark(0); }
    vkCmdDispatch(cb, (uint32_t)d->x, (uint32_t)d->y, (uint32_t)d->z);
    pt_close(9);
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
    if (!c->mask || c->index < 0 || c->index >= 8 || S.render_disabled) return;   /* NVN clears follow the render enable (conditional clears, hle_nvn.c) */
#ifdef GAME_BOTW
    int f = rt_dup_of(&S.rt, c->index); int idx = f >= 0 ? f : c->index;
#else
    int idx = c->index;
#endif
    if (!pass_begin() || !g_patt[idx].v) return;
    if (clear_taken()) { g_pass_written |= 1u << idx; return; }   /* the pass's load operation */
    VkClearAttachment a = {.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT, .colorAttachment = (uint32_t)idx}; memcpy(a.clearValue.color.float32, c->rgba, 16);
    VkClearRect r = {clear_rect(), 0, 1}; if (!r.rect.extent.width || !r.rect.extent.height) return;
    pass_record(); vkCmdClearAttachments(vk_cb(), 1, &a, 1, &r); g_pass_written |= 1u << idx; g_lr_dirty |= 1u << idx;
}
static void clear_ds(const p_clear_ds *c) {
    if (S.render_disabled || !pass_begin() || !g_pdepth.v) return;
    if (clear_taken()) { g_pass_written |= 1u << 8; return; }
    VkImageAspectFlags asp = 0, have = vk_tex_aspect(g_pdepth.v);
    if (c->depth_mask && (have & VK_IMAGE_ASPECT_DEPTH_BIT)) asp |= VK_IMAGE_ASPECT_DEPTH_BIT;
    if (c->stencil_mask && (have & VK_IMAGE_ASPECT_STENCIL_BIT)) asp |= VK_IMAGE_ASPECT_STENCIL_BIT;
    if (!asp) return;
    VkClearAttachment a = {.aspectMask = asp, .clearValue.depthStencil = {c->depth, (uint32_t)c->stencil}};
    VkClearRect r = {clear_rect(), 0, 1}; if (!r.rect.extent.width || !r.rect.extent.height) return;
    if (g_pass_recorded && g_pass_nodepth) { end_pass(5); if (!pass_begin()) return; }
    g_force_depth = 1; pass_record(); g_force_depth = 0; vkCmdClearAttachments(vk_cb(), 1, &a, 1, &r); g_pass_written |= 1u << 8; g_lr_dirty |= 1u << 8;
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
    gw_note(dst, dst + n);
    full_barrier(); g_copies++;
}
static int region_level(const view_t *v, int has) { return has && v->set_levels ? v->base_level : 0; }
static int region_layer(const view_t *v, int has) { return has && v->set_layers ? v->min_layer : 0; }
static void copy_b2t(const p_copy_b2t *c, int to_buffer) {
    const tex_t *t = c->tex; vtex *v = vk_tex_get(t);
    VkBuffer b; VkDeviceSize o;
    if (!v || !vk_pool_find(c->src, 1, &b, &o)) { g_skips[6]++; return; }
    end_pass(3); pclr_flush();
    int bw = 1, bh = 1, bpb = nvn_fmt_bpb(t->d.format, &bw, &bh);
    uint32_t rowlen = S.copy_stride && bpb ? (uint32_t)(S.copy_stride / bpb) * bw : 0;
    int z0 = c->r.zoffset, depth = c->r.depth ? c->r.depth : 1, is3d = t->d.target == NVN_TEXTURE_TARGET_3D;
    VkBufferImageCopy r = {.bufferOffset = o, .bufferRowLength = rowlen, .imageSubresource = {(VkImageAspectFlags)vk_tex_aspect(v) & ~VK_IMAGE_ASPECT_STENCIL_BIT ? (VkImageAspectFlags)vk_tex_aspect(v) & ~VK_IMAGE_ASPECT_STENCIL_BIT : (VkImageAspectFlags)vk_tex_aspect(v),
        (uint32_t)region_level(&c->view, c->has_view), (uint32_t)(region_layer(&c->view, c->has_view) + (is3d ? 0 : z0)), is3d ? 1 : (uint32_t)depth},
        .imageOffset = {c->r.xoffset, c->r.yoffset, is3d ? z0 : 0}, .imageExtent = {(uint32_t)c->r.width, (uint32_t)(c->r.height ? c->r.height : 1), is3d ? (uint32_t)depth : 1}};
    if (r.bufferOffset & 3) { static int said; if (!said++) VKLOG("【红】buffer/texture copy at an unaligned buffer offset"); g_skips[6]++; return; }
    if (to_buffer) { vk_tex_mark_read(v); vkCmdCopyImageToBuffer(vk_cb(), vk_tex_image(v), VK_IMAGE_LAYOUT_GENERAL, b, 1, &r);
        uint64_t pitch = S.copy_stride ? S.copy_stride : (uint64_t)((r.imageExtent.width + bw - 1) / bw) * (bpb ? bpb : 16); gw_note(c->src, c->src + pitch * ((r.imageExtent.height + bh - 1) / bh) * r.imageExtent.depth); }
    else { vk_tex_upload_if_stale(v, t); vkCmdCopyBufferToImage(vk_cb(), b, vk_tex_image(v), VK_IMAGE_LAYOUT_GENERAL, 1, &r); vk_tex_gpu_wrote(v); }
    full_barrier(); g_copies++;
}
static void copy_t2t(const p_copy_t2t *c) {
    vtex *s = vk_tex_get(c->src), *d = vk_tex_get(c->dst);
    if (!s || !d) { g_skips[6]++; return; }
    end_pass(3); pclr_flush();
    vk_tex_upload_if_stale(s, c->src); vk_tex_upload_if_stale(d, c->dst);
    VkImageAspectFlags as = vk_tex_aspect(s), ad = vk_tex_aspect(d);
    int sl = region_layer(&c->sv, c->has_sv), dl = region_layer(&c->dv, c->has_dv);
    VkImageBlit b = {.srcSubresource = {as, (uint32_t)region_level(&c->sv, c->has_sv), (uint32_t)(sl + c->sr.zoffset), 1},
        .srcOffsets = {{c->sr.xoffset, c->sr.yoffset, 0}, {c->sr.xoffset + c->sr.width, c->sr.yoffset + (c->sr.height ? c->sr.height : 1), 1}},
        .dstSubresource = {ad, (uint32_t)region_level(&c->dv, c->has_dv), (uint32_t)(dl + c->dr.zoffset), 1},
        .dstOffsets = {{c->dr.xoffset, c->dr.yoffset, 0}, {c->dr.xoffset + c->dr.width, c->dr.yoffset + (c->dr.height ? c->dr.height : 1), 1}}};
    /* a 3D texture's slice is a z offset (nvn_mtl.m copy: depthPlane / origin.z), not an array layer */
    if (vk_tex_is3d(s)) { b.srcSubresource.baseArrayLayer = 0; b.srcOffsets[0].z = sl + c->sr.zoffset; b.srcOffsets[1].z = b.srcOffsets[0].z + 1; }
    if (vk_tex_is3d(d)) { b.dstSubresource.baseArrayLayer = 0; b.dstOffsets[0].z = dl + c->dr.zoffset; b.dstOffsets[1].z = b.dstOffsets[0].z + 1; }
    int same = c->sr.width == c->dr.width && c->sr.height == c->dr.height;
    uint32_t sw, sh, dw, dh; VkFormat sf, df; vk_tex_dims(s, &sw, &sh, &sf); vk_tex_dims(d, &dw, &dh, &df);
    if (same && (sf == df || (as & VK_IMAGE_ASPECT_DEPTH_BIT))) {
        VkImageCopy r = {b.srcSubresource, b.srcOffsets[0], b.dstSubresource, b.dstOffsets[0], {(uint32_t)c->sr.width, (uint32_t)(c->sr.height ? c->sr.height : 1), 1}};
        vkCmdCopyImage(vk_cb(), vk_tex_image(s), VK_IMAGE_LAYOUT_GENERAL, vk_tex_image(d), VK_IMAGE_LAYOUT_GENERAL, 1, &r);
    } else vkCmdBlitImage(vk_cb(), vk_tex_image(s), VK_IMAGE_LAYOUT_GENERAL, vk_tex_image(d), VK_IMAGE_LAYOUT_GENERAL, 1, &b, (c->flags & 1) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST);
    { char x_[96]; snprintf(x_, sizeof x_, "to fmt %#x %ux%u mem %p (%s)", c->dst->d.format, c->dst->d.width, c->dst->d.height, (void *)c->dst->mem, same && (sf == df || (as & VK_IMAGE_ASPECT_DEPTH_BIT)) ? "copy" : "blit"); fmt_log("copy", c->src, c->has_sv && c->sv.set_format ? c->sv.format : c->src->d.format, s, x_); }
    vk_tex_mark_read(s); vk_tex_gpu_wrote(d); full_barrier(); g_copies++;
}
/* a texture clear goes pending (pclr_add); one of an attachment of the open pass ends that pass first. Draws of the open
   pass with sets built before it rebuild them, so a draw sampling the image finds it pending (g_pclr_hit, stage_set) */
static void clear_tex(const p_clear_tex *c) {
    vtex *v = vk_tex_get(c->tex); if (!v) return;
    vk_tex_upload_if_stale(v, c->tex);
    int att = 0; for (int a = 0; a < 9; a++) att |= (a < 8 ? g_patt[a].v : g_pdepth.v) == v;
    if (g_pass_open && att) end_pass(6);
    VkClearValue cv; VkImageAspectFlags asp = (VkImageAspectFlags)vk_tex_aspect(v);
    if (asp & VK_IMAGE_ASPECT_COLOR_BIT) memcpy(&cv.color, &c->c, 16); else cv.depthStencil = (VkClearDepthStencilValue){c->c.f[0], c->c.u[1]};
    pclr_add(v, region_level(&c->view, c->has_view), region_layer(&c->view, c->has_view), asp, cv);
    for (int k = 0; k < 6; k++) g_gen_bind[k]++;
}

/* ---------- conditional rendering (nvn_mtl.m cond_eval): evaluated in GPU order at replay. A counter report still pending
   on the GPU holds the value its completion will write when that is known (0 for every type but samples passed); an unknown
   one renders. TOTK requests a clear by writing counter 1 and resets it with the TIMESTAMP_TOP report after the clear ---------- */
#define CPEND 4096
typedef struct { uint64_t addr, fence; int known; } cpend_t;
static cpend_t g_cpend[CPEND]; static unsigned g_cpend_n;
static void cpend_push(uint64_t addr, int known) { g_cpend[g_cpend_n++ % CPEND] = (cpend_t){addr, vk_next_fence(), known}; }
/* -1: no pending report (read memory), 0: pending with an unknown value, 1: pending, value 0 */
static int cpend_lookup(uint64_t addr) {
    uint64_t done = vk_done(); unsigned n = g_cpend_n < CPEND ? g_cpend_n : CPEND;
    for (unsigned k = 0; k < n; k++) { const cpend_t *e = &g_cpend[(g_cpend_n - 1 - k) % CPEND]; if (e->fence <= done) break; if (e->addr == addr) return e->known; }
    return -1;
}
static uint64_t g_cond_off, g_cond_unknown;
static int cond_eval(const p_render_cond *c) {
    int kx = cpend_lookup(c->addr), ky = cpend_lookup(c->addr + 16);
    VkBuffer b; VkDeviceSize o;
    if (!kx || !ky || !vk_pool_find(c->addr, 32, &b, &o)) { g_cond_unknown++; return 1; }
    uint64_t x = 0, y = 0;
    if (kx < 0) memcpy(&x, (const void *)(uintptr_t)c->addr, 8);
    if (ky < 0) memcpy(&y, (const void *)(uintptr_t)(c->addr + 16), 8);
    int on = c->mode == 0 ? x == y : x != y; g_cond_off += !on; return on;
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
        op_hdr h; memcpy(&h, p, sizeof h); g_op = p;
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) VKDIE("corrupt command record at %p: op %u size %u", (const void *)p, h.op, h.size);
        const void *a = p + 8;
        switch (h.op) {
            case OP_END: return;
            case OP_BEGIN: break;
            case OP_JUMP: memcpy(&p, a, 8); continue;
            case OP_CALL: { const uint8_t *t; memcpy(&t, a, 8); mtl_exec(t); break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, (const uint8_t *)a + 8, 4); memcpy(&v, (const uint8_t *)a + 12, 4); vk_sub_post(1, val, m, v, 0); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, (const uint8_t *)a + 8, 8); cpend_push(ad, t != NVN_COUNTER_TYPE_SAMPLES_PASSED);
                if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) q_report(ad, g_pass_open); else vk_sub_post(2, NULL, 0, 0, ad); break; }
            case OP_RESET_COUNTER: { uint64_t t; memcpy(&t, a, 8); if (t == NVN_COUNTER_TYPE_SAMPLES_PASSED) q_reset(g_pass_open); break; }
            case OP_COPY_B2B: { uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, (const uint8_t *)a + 8, 8); memcpy(&n, (const uint8_t *)a + 16, 8); copy_b2b(s, d, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, (const uint8_t *)a + 8, 8); upd_add(d, (const uint8_t *)a + 16, n);
                for (int k = 0; k < 6; k++) for (int i = 0; i < 16; i++) { const p_bind_buf *u = &S.ubo[k][i];   /* the stages reading the updated range rebuild their sets */
                    if (u->addr && d < u->addr + (u->size && u->size <= 65536 ? u->size : 65536) && u->addr < d + n) { g_gen_bind[k]++; break; } }
                break; }
            case OP_SET_RT: { const p_set_rt *r = a; if (S.has_rt && same_rt(r, &S.rt)) { S.rt = *r; break; } end_pass(1); S.rt = *r; S.has_rt = 1; g_gen_rt++; break; }
            case OP_CLEAR_COLOR: clear_color(a); break;
            case OP_CLEAR_DS: clear_ds(a); break;
            case OP_VIEWPORTS: g_gen_state++; { const p_viewports *v = a; for (int i = 0; i < v->n && v->first + i < 16; i++) memcpy(S.vp[v->first + i], v->v[i], 24); if (v->first + v->n > S.nvp) S.nvp = v->first + v->n > 16 ? 16 : v->first + v->n; break; }
            case OP_SCISSOR: g_gen_state++; S.sc = *(const p_scissor *)a; S.has_sc = 1; break;
            case OP_DEPTH_RANGE: g_gen_state++; { const p_depth_range *d = a; S.dr[0] = d->n; S.dr[1] = d->f; break; }
            case OP_BIND_PROGRAM: S.prog = ((const p_bind_program *)a)->prog; break;
            case OP_BIND_STATE: g_gen_state++; { const p_bind_state *b = a;
                switch (b->kind) { case 0: { blend_st bs; memcpy(&bs, b->bytes, sizeof bs); if (bs.target < 8) S.blend[bs.target] = bs; break; }
                                   case 1: memcpy(&S.color, b->bytes, sizeof S.color); break; case 2: memcpy(&S.chmask, b->bytes, sizeof S.chmask); break;
                                   case 3: memcpy(&S.ms, b->bytes, sizeof S.ms); break; case 4: memcpy(&S.poly, b->bytes, sizeof S.poly); break; case 5: memcpy(&S.ds, b->bytes, sizeof S.ds); break; }
                break; }
            case OP_BIND_VATTR: g_gen_vtx++; { const p_bind_vattr *b = a; for (int i = 0; i < b->n && b->first + i < 16; i++) S.attr[b->first + i] = b->st[i]; S.nattr = b->first + b->n > 16 ? 16 : b->first + b->n; break; }
            case OP_BIND_VSTREAM: g_gen_vtx++; { const p_bind_vstream *b = a; for (int i = 0; i < b->n && b->first + i < 16; i++) S.stream[b->first + i] = b->st[i]; S.nstream = b->first + b->n; break; }
            case OP_BIND_VBUF: g_gen_vtx++; { const p_bind_vbuf *b = a; for (int i = 0; i < b->n && b->first + i < 16; i++) S.vbuf[b->first + i] = b->r[i]; break; }
            case OP_BIND_UBO: { const p_bind_buf *b = a; if (b->stage < 6 && b->index >= 0 && b->index < 16) S.ubo[b->stage][b->index] = *b; g_gen_bind[b->stage]++; break; }
            case OP_BIND_SSBO: { const p_bind_buf *b = a; if (b->stage < 6 && b->index >= 0 && b->index < 16) S.ssbo[b->stage][b->index] = *b; g_gen_bind[b->stage]++; break; }
            case OP_BIND_TEX: { const p_bind_handles *b = a; if (b->stage < 6 && b->first >= 0 && b->first + b->n <= 128) memcpy(&S.tex[b->stage][b->first], b->h, 8 * b->n); g_gen_bind[b->stage]++; break; }
            case OP_BIND_SAMP: { const p_bind_handles *b = a; if (b->stage < 6 && b->first >= 0 && b->first + b->n <= 128) memcpy(&S.samp[b->stage][b->first], b->h, 8 * b->n); g_gen_bind[b->stage]++; break; }
            case OP_BIND_IMG: { const p_bind_handles *b = a; if (b->stage < 6 && b->first >= 0 && b->first + b->n <= 32) memcpy(&S.img[b->stage][b->first], b->h, 8 * b->n); g_gen_bind[b->stage]++; break; }
            case OP_STENCIL: g_gen_state++; { const p_stencil *b = a; for (int f = 0; f < 2; f++) if (b->face & (1 << f)) { if (b->which == 0) S.st_vmask[f] = b->value_mask; else if (b->which == 1) S.st_wmask[f] = b->mask; else S.st_ref[f] = b->ref; } break; }
            case OP_POLY_OFFSET: g_gen_state++; S.po = *(const p_poly_offset *)a; break;
            case OP_DRAW: draw(a, NULL); break;
            case OP_DRAW_IDX: draw(NULL, a); break;
            case OP_DISPATCH: dispatch(a); break;
            case OP_BARRIER: { int bits; memcpy(&bits, a, 4);
                (void)bits; if (!g_pass_open) full_barrier(); break; }   /* inside a pass nothing, as nvn_mtl.m; every pass ends with a full barrier */
            case OP_SET_TEX_POOL: case OP_SET_SAMP_POOL: break;
            case OP_COPY_B2T: copy_b2t(a, 0); break;
            case OP_COPY_STRIDE: S.copy_stride = ((const p_copy_stride *)a)->stride; break;
            case OP_BLEND_COLOR: g_gen_state++; memcpy(S.blend_color, ((const p_blend_color *)a)->rgba, 16); break;
            case OP_COPY_T2B: copy_b2t(a, 1); break;   /* src: the destination buffer address */
            case OP_COPY_T2T: copy_t2t(a); break;
            case OP_CLEAR_TEX: clear_tex(a); break;
            case OP_DEPTH_CLAMP: g_gen_state++; S.depth_clamp = ((const p_flag *)a)->value != 0; break;
            case OP_RENDER_ENABLE: S.render_disabled = !((const p_flag *)a)->value; break;
            case OP_RASTER_DISCARD: g_gen_state++; S.raster_discard = ((const p_flag *)a)->value != 0; break;
            case OP_RENDER_COND: S.render_disabled = !cond_eval(a); break;
            default: break;
        }
        p += h.size;
    }
}
void mtl_exec_rest(const uint64_t *h, int n) { (void)h; (void)n; }
static uint64_t g_sub_t0;
void mtl_sub_begin(void) { vk_sub_open(); g_sub_t0 = clock_gettime_nsec_np(CLOCK_MONOTONIC); g_cut_at = g_draws; g_ncut = 0; }
uint64_t mtl_sub_commit(void) { upd_flush(); g_shadow_epoch++; g_sub_gen++; g_cb_gen++; g_dyn_set = 0; g_dpool = VK_NULL_HANDLE; uint64_t f = vk_sub_submit();
    static int sync = -1; if (sync < 0) { const char *e = getenv("TOTK_VK_SYNC_SUBMIT"); sync = e && atoi(e) == 1; if (sync) VKLOG("sync submit: every submission completes on the GPU before the game continues"); }
    if (sync && f && !mtl_wait(f, 0)) { int a_ = runtime_core_away(); mtl_wait(f, UINT64_MAX); runtime_core_back(&a_); }   /* TOTK_VK_SYNC_SUBMIT=1 (diagnostic) */
    void vk_replay_ns(uint64_t); vk_replay_ns(clock_gettime_nsec_np(CLOCK_MONOTONIC) - g_sub_t0); return f; }
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) { copy_b2b(src, dst, n); }
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { vk_sub_post(1, val, mode, v, 0); }
void mtl_sub_counter(uint64_t addr, uint32_t type) { cpend_push(addr, type != NVN_COUNTER_TYPE_SAMPLES_PASSED); vk_sub_post(2, NULL, 0, 0, addr); }
void *mtl_sub_capture_ubos(const uint64_t *h, int n) { (void)h; (void)n; return NULL; }
void mtl_sub_snapshot_ubos_cap(const uint64_t *h, int n, void *cap) { (void)h; (void)n; free(cap); }
/* Stereo by replay (nvn_mtl.m g_eye_pass) needs the uniform captures above, which this backend does not take: one replay
   per submission, so the eye functions are never reached. */
int mtl_stereo_replay(void) { return 0; }
int mtl_stereo_project(void) { return 0; }
void mtl_eye_pass(int e) { (void)e; }
void *mtl_ucap_dup(const void *base) { (void)base; return NULL; }
int mtl_ucap_eye(void *cap, int eye) { (void)cap; (void)eye; return 0; }
void mtl_ucap_free(void *c) { free(c); }
void mtl_eye_capture(int eye) { (void)eye; }
/* nvn_mtl.m binds its upload batch to the replaying thread here; vk-submit owns the queue and the replay records into the
   calling thread's own command buffers, so there is nothing to bind. */
void mtl_take_submit_thread(void) {}
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }

/* ---------- present ---------- */
static uint64_t g_presents;
static void pt_frame(uint64_t present) {
    static long at = -2; if (at == -2) { const char *e = getenv("TOTK_VK_PASS_TIMES"); at = e ? atol(e) : -1; }
    if (at < 0) return;
    if ((long)present == at) {
        if (!g_pt_pool) { VkQueryPoolCreateInfo qi = {.sType = VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, .queryType = VK_QUERY_TYPE_TIMESTAMP, .queryCount = 2 * PT_MAX}; VKCHECK(vkCreateQueryPool(V.dev, &qi, NULL, &g_pt_pool)); }
        vkResetQueryPool(V.dev, g_pt_pool, 0, 2 * PT_MAX); g_pt_n = 0; g_pt_on = 1; return;
    }
    if (!g_pt_on) return;
    g_pt_on = 0; mtl_finish();
    static uint64_t ts[2 * PT_MAX]; int n = g_pt_n, idx[PT_MAX]; double ms[PT_MAX], total = 0;
    if (vkGetQueryPoolResults(V.dev, g_pt_pool, 0, 2 * n, sizeof ts, ts, 8, VK_QUERY_RESULT_64_BIT) != VK_SUCCESS) { VKLOG("pass times: results not available"); return; }
    for (int i = 0; i < n; i++) { idx[i] = i; ms[i] = ts[2 * i + 1] > ts[2 * i] ? (ts[2 * i + 1] - ts[2 * i]) * V.props.limits.timestampPeriod / 1e6 : 0; total += ms[i]; }
    for (int i = 1; i < n; i++) for (int j = i; j > 0 && ms[idx[j]] > ms[idx[j - 1]]; j--) { int t = idx[j]; idx[j] = idx[j - 1]; idx[j - 1] = t; }
    VKLOG("pass times of present %llu: %d passes / dispatches, %.2f ms in them", (unsigned long long)present, n, total);
    for (int k = 0; k < n && k < 60; k++) { const pt_info *p = &g_pt[idx[k]];
        if (p->ncol < 0) VKLOG("  #%d %.3f ms dispatch %ux%u", idx[k], ms[idx[k]], p->w, p->h);
        else VKLOG("  #%d %.3f ms %ux%u colour %d (fmt %d) depth %d, clear-load %#x, %d draws, ended by %d", idx[k], ms[idx[k]], p->w, p->h, p->ncol, p->f0, p->depth, p->load_clear, p->draws, p->why); }
    for (int i = 0; i < g_npp; i++) VKLOG("  pipeline pass #%d: %d draws vs %s fs %s", g_pp[i].pass, g_pp[i].draws, g_pp[i].vs ? g_pp[i].vs->key : "-", g_pp[i].fs ? g_pp[i].fs->key : "-");
    g_npp = 0;
}
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)period_s; (void)crop; g_presents++; vk_tex_frame();
    { /* a frame over 60 ms: where the time went (vk_wait_stats; mtl_wait counts every thread) */
      static uint64_t pt, pw[4]; uint64_t now = clock_gettime_nsec_np(CLOCK_MONOTONIC), w[4]; void vk_wait_stats(uint64_t[4]); vk_wait_stats(w);
      static uint64_t lim; if (!lim) { const char *e = getenv("TOTK_VK_LONG_MS"); lim = (e ? atol(e) : 60) * 1000000ull; }   /* TOTK_VK_LONG_MS: the threshold */
      static uint64_t pmk[2], pup[3], pgpu, pdisp, psh[4], plink; uint64_t up_[3], gpu_ = 0, sh_[4]; vk_shader_stats(sh_); void vk_tex_stats(uint64_t *, uint64_t *, uint64_t *); vk_tex_stats(&up_[0], &up_[1], &up_[2]); { uint64_t vk_gpu_ns(void); gpu_ = vk_gpu_ns(); }
      if (pt && now - pt > lim && g_presents > 8000) { static int n; if (n++ < 200) VKLOG("long frame at present %llu: %.1f ms; job queue full %.1f, mtl_wait %.1f, one-shot %.1f, replay %.1f ms; shader objects %llu / %.1f ms, new link keys %llu, uploads %llu / %.1f MB, GPU %.1f ms since the last present; this frame %llu passes / %llu draws / %llu dispatches; variants loaded %llu / %.1f ms, translated %llu / %.1f ms", (unsigned long long)g_presents,
          (now - pt) / 1e6, (w[0] - pw[0]) / 1e6, (w[1] - pw[1]) / 1e6, (w[2] - pw[2]) / 1e6, (w[3] - pw[3]) / 1e6, (unsigned long long)(g_mk[0] - pmk[0]), (g_mk[1] - pmk[1]) / 1e6, (unsigned long long)(g_links - plink), (unsigned long long)(up_[0] - pup[0]), (up_[1] - pup[1]) / 1e6, (gpu_ - pgpu) / 1e6, (unsigned long long)g_frame_passes, (unsigned long long)g_frame_draws, (unsigned long long)(g_dispatches - pdisp), (unsigned long long)(sh_[0] - psh[0]), (sh_[1] - psh[1]) / 1e6, (unsigned long long)(sh_[2] - psh[2]), (sh_[3] - psh[3]) / 1e6);
          if (n < 4) VKLOG("  uniform copies %llu, %.1f MB", (unsigned long long)g_ucopy[0], g_ucopy[1] / 1e6); }
      memset(g_ucopy, 0, sizeof g_ucopy); memcpy(pmk, g_mk, sizeof pmk); memcpy(pup, up_, sizeof pup); pgpu = gpu_; pdisp = g_dispatches; memcpy(psh, sh_, sizeof psh); plink = g_links;
      pt = now; memcpy(pw, w, sizeof w); }
    /* frames in flight: present N waits until the GPU finished the frame of present N - TOTK_VK_FRAMES_AHEAD (2). Without a
       swapchain nothing else holds the game thread back, and a sync wait of the game (every 10 frames in the Shrine) then
       waited for the whole backlog */
    { static int ahead = -1; static uint64_t fence[16]; if (ahead < 0) { const char *e = getenv("TOTK_VK_FRAMES_AHEAD"); ahead = e ? atoi(e) : 2; if (ahead > 15) ahead = 15; }
      if (ahead > 0) { uint64_t f = fence[(g_presents + 16 - (uint64_t)ahead) % 16]; if (f && !mtl_wait(f, 0)) { int a_ = runtime_core_away(); mtl_wait(f, UINT64_MAX); runtime_core_back(&a_); } fence[g_presents % 16] = mtl_last_fence(); } }
    if (t) {
        vtex *v = vk_tex_get(t);
        if (v) { vk_tex_mark_read(v); uint32_t w, h; VkFormat f; vk_tex_dims(v, &w, &h, &f); vk_present_image(vk_tex_image(v), w, h, f); }
        front_frame_presented();
    }
    if (g_presents % 600 == 0) {
        uint64_t up, ub, ug; void vk_tex_stats(uint64_t *, uint64_t *, uint64_t *); vk_tex_stats(&up, &ub, &ug);
        VKLOG("present %llu: %llu passes / %llu draws (last frame %llu / %llu), %llu dispatches, %llu copies, uploads %llu (%.1f MB, %llu deswizzled on the GPU); skipped: no target %llu, disabled %llu, no shader %llu, compile %llu, index %llu, compute %llu, copy %llu",
              (unsigned long long)g_presents, (unsigned long long)g_passes, (unsigned long long)g_draws, (unsigned long long)g_frame_passes, (unsigned long long)g_frame_draws,
              (unsigned long long)g_dispatches, (unsigned long long)g_copies, (unsigned long long)up, ub / 1e6, (unsigned long long)ug, (unsigned long long)g_skips[0], (unsigned long long)g_skips[1],
              (unsigned long long)g_skips[2], (unsigned long long)g_skips[3], (unsigned long long)g_skips[4], (unsigned long long)g_skips[5], (unsigned long long)g_skips[6]);
        { uint64_t vk_gpu_ns(void), vk_submit_ns(void), vk_end_ns(void); void vk_cb_stats(uint64_t *, uint64_t *); static uint64_t pg, ps, pe, pb, pc; uint64_t g = vk_gpu_ns(), q = vk_submit_ns(), e = vk_end_ns(), bn, cn; vk_cb_stats(&bn, &cn);
          VKLOG("GPU busy %.2f ms per present over the last 600; submit thread per present: vkEndCommandBuffer %.2f ms, vkQueueSubmit2 %.2f ms; %.1f command buffer pairs, vkBeginCommandBuffer %.2f ms (game thread); uniform copies %.0f / %.2f MB per present; occlusion reports %llu zero, %llu not; state commands emitted %llu, skipped %llu", (g - pg) / 1e6 / 600, (e - pe) / 1e6 / 600, (q - ps) / 1e6 / 600, (cn - pc) / 600.0, (bn - pb) / 1e6 / 600, g_ucopy_all[0] / 600.0, g_ucopy_all[1] / 600e6, ({ uint64_t r_[2]; vk_query_stats(r_); (unsigned long long)r_[0]; }), ({ uint64_t r_[2]; vk_query_stats(r_); (unsigned long long)r_[1]; }), (unsigned long long)g_dyn_emitted, (unsigned long long)g_dyn_skipped); pg = g; ps = q; pe = e; pb = bn; pc = cn; memset(g_ucopy_all, 0, sizeof g_ucopy_all); }
        if (pool_shadow()) VKLOG("pool shadow: %llu ranges copied, %llu over 16 MiB left in place", (unsigned long long)g_pshadow[0], (unsigned long long)g_pshadow[1]);
        if (ubo_snapshot() || pool_shadow()) VKLOG("snapshot: %llu ranges copied, %llu read from memory (GPU-written, %d writes pending), %llu copies dropped by GPU writes", (unsigned long long)g_usnap[0], (unsigned long long)g_usnap[1], g_ngw, (unsigned long long)g_usnap[2]);
        { uint64_t s_[4]; vk_shader_stats(s_); VKLOG("shader objects: %llu created (%.1f ms); first-use link keys %llu; variants loaded %llu (%.1f ms), translated %llu (%.1f ms)", (unsigned long long)g_mk[0], g_mk[1] / 1e6, (unsigned long long)g_links, (unsigned long long)s_[0], s_[1] / 1e6, (unsigned long long)s_[2], s_[3] / 1e6); vk_shader_report(); }
        VKLOG("pass ends: submit %llu, targets %llu, buffer copy %llu, texture copy %llu, dispatch %llu, depth needed %llu, texture clear %llu, feedback %llu, update flush %llu",
              (unsigned long long)g_end_why[0], (unsigned long long)g_end_why[1], (unsigned long long)g_end_why[2], (unsigned long long)g_end_why[3], (unsigned long long)g_end_why[4],
              (unsigned long long)g_end_why[5], (unsigned long long)g_end_why[6], (unsigned long long)g_end_why[7], (unsigned long long)g_end_why[8]);
        VKLOG("render conditions: %llu off, %llu unknown (rendered); clears as load operations %llu, pending clears as load operations %llu, run %llu; passes that only cleared %llu; decal draws through local read %llu, their barriers %llu; attachments not stored %llu (pruned images read later %llu), passes without their depth %llu; self-sampling draws by kind %llu %llu %llu %llu %llu %llu %llu %llu", (unsigned long long)g_cond_off, (unsigned long long)g_cond_unknown, (unsigned long long)g_load_clears, (unsigned long long)g_pclr_loads, (unsigned long long)g_pclr_run, (unsigned long long)g_pass_dropped, (unsigned long long)g_fb_draws, (unsigned long long)g_fb_barriers, ({ uint64_t a_, b_; vk_tex_prune_stats(&a_, &b_); (unsigned long long)a_; }), ({ uint64_t a_, b_; vk_tex_prune_stats(&a_, &b_); (unsigned long long)b_; }), (unsigned long long)g_nodepth_passes, (unsigned long long)g_fb_why[0], (unsigned long long)g_fb_why[1], (unsigned long long)g_fb_why[2], (unsigned long long)g_fb_why[3], (unsigned long long)g_fb_why[4], (unsigned long long)g_fb_why[5], (unsigned long long)g_fb_why[6], (unsigned long long)g_fb_why[7]);
    }
    g_frame_passes = g_frame_draws = 0; pt_frame(g_presents);
    return g_presents;
}
void mtl_init(void) { vk_init(); void runtime_audio_hold(int); runtime_audio_hold(0); }   /* audio_mixer.c holds the sound until nvn_mtl.m warm_start ends its warm-up; this backend has none */
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = g_passes; *draws = g_draws; }
void mtl_wb_fence_done(void) {}
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
