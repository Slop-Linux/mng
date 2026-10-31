/* Linux null graphics backend: the nvn_mtl.h interface without a GPU. A submission's CPU-visible effects (event writes,
   counter records, buffer copies, inline uniform updates) are applied in command order when it is committed, so the game's
   waits on syncs, events and queries complete; draws, dispatches and texture work are recorded and dropped. Counted with
   TOTK_NULL_STATS=1 (per op, logged every 600 presents). */
#include "../../third_party/d2r_gfx/gfx/nvn/nvn_obj.h"
#include "../../third_party/d2r_gfx/gfx/nvn/nvn_mtl.h"
#include "../../third_party/d2r_gfx/host/dm.h"
#include <mach/mach_time.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef struct { const void *mem; uint64_t size; } pool_ent;
static pool_ent g_pools[4096]; static int g_npool; static pthread_mutex_t g_pool_mu = PTHREAD_MUTEX_INITIALIZER;
typedef struct { int kind; volatile uint32_t *val; uint32_t mode, v; uint64_t addr, src, n; const uint8_t *data; } post_op;
static post_op *g_post; static size_t g_npost, g_postcap;
static _Atomic uint64_t g_fence, g_frame;
static uint64_t g_ops[OP_COUNT_], g_presents;
static int stats_on(void) { static int on = -1; if (on < 0) on = getenv("TOTK_NULL_STATS") != NULL; return on; }
static uint64_t gpu_ticks(void) { return (uint64_t)((__uint128_t)mach_absolute_time() * 12 / 625); }   /* 19.2 MHz */

void mtl_init(void) { static int once; if (!once) { once = 1; LOG("null graphics backend: no GPU work, CPU-visible effects at commit"); } }
void mtl_pool_add(const void *mem, uint64_t size) {
    pthread_mutex_lock(&g_pool_mu);
    if (g_npool < (int)(sizeof g_pools / sizeof *g_pools)) g_pools[g_npool++] = (pool_ent){mem, size};
    pthread_mutex_unlock(&g_pool_mu);
}
void mtl_pool_del(const void *mem) {
    pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) if (g_pools[i].mem == mem) { g_pools[i] = g_pools[--g_npool]; break; }
    pthread_mutex_unlock(&g_pool_mu);
}
int mtl_addr(uint64_t addr, uint64_t len, void **buf, uint64_t *off) {
    int r = 0; pthread_mutex_lock(&g_pool_mu);
    for (int i = 0; i < g_npool; i++) { uint64_t b = (uintptr_t)g_pools[i].mem; if (addr >= b && addr + len <= b + g_pools[i].size) { *buf = NULL; *off = addr - b; r = 1; break; } }
    pthread_mutex_unlock(&g_pool_mu); return r;
}
void mtl_pool_invalidate(const pool_t *p, uint64_t off, uint64_t n) { (void)p; (void)off; (void)n; }

static void post(post_op o) {
    if (g_npost == g_postcap) { g_postcap = g_postcap ? g_postcap * 2 : 64; g_post = realloc(g_post, g_postcap * sizeof *g_post); if (!g_post) abort(); }
    g_post[g_npost++] = o;
}
void mtl_sub_begin(void) {}
void mtl_sub_copy(uint64_t src, uint64_t dst, uint64_t n) { post((post_op){.kind = 3, .addr = dst, .src = src, .n = n}); }
void mtl_sub_event(volatile uint32_t *val, uint32_t mode, uint32_t v) { post((post_op){.kind = 1, .val = val, .mode = mode, .v = v}); }
void mtl_sub_counter(uint64_t addr, uint32_t type) { post((post_op){.kind = 2, .addr = addr, .mode = type}); }
void *mtl_sub_capture_ubos(const uint64_t *h, int n) { (void)h; (void)n; return NULL; }
void mtl_sub_snapshot_ubos_cap(const uint64_t *h, int n, void *cap) { (void)h; (void)n; free(cap); }
void mtl_sub_index(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec_rest(const uint64_t *h, int n) { (void)h; (void)n; }
void mtl_exec(const uint8_t *p) {
    for (;;) {
        op_hdr h; memcpy(&h, p, sizeof h);
        if (h.size < 8 || (h.size & 7) || h.op >= OP_COUNT_) { LOG("【红】null: corrupt command record at %p: op %u size %u", (const void *)p, h.op, h.size); abort(); }
        const uint8_t *a = p + 8; g_ops[h.op]++;
        switch (h.op) {
            case OP_END: return;
            case OP_JUMP: memcpy(&p, a, 8); continue;
            case OP_CALL: { const uint8_t *t; memcpy(&t, a, 8); mtl_exec(t); break; }
            case OP_SIGNAL_EVENT: { volatile uint32_t *val; uint32_t m, v; memcpy(&val, a, 8); memcpy(&m, a + 8, 4); memcpy(&v, a + 12, 4); mtl_sub_event(val, m, v); break; }
            case OP_REPORT_COUNTER: { uint64_t t, ad; memcpy(&t, a, 8); memcpy(&ad, a + 8, 8); mtl_sub_counter(ad, (uint32_t)t); break; }
            case OP_COPY_B2B: { uint64_t s, d, n; memcpy(&s, a, 8); memcpy(&d, a + 8, 8); memcpy(&n, a + 16, 8); mtl_sub_copy(s, d, n); break; }
            case OP_UPDATE_UB: { uint64_t d, n; memcpy(&d, a, 8); memcpy(&n, a + 8, 8); uint8_t *c = malloc(n ? n : 1); memcpy(c, a + 16, n); post((post_op){.kind = 4, .addr = d, .n = n, .data = c}); break; }
            default: break;
        }
        p += h.size;
    }
}
uint64_t mtl_sub_commit(void) {
    for (size_t i = 0; i < g_npost; i++) {
        post_op *o = &g_post[i];
        if (o->kind == 1) { if (o->mode == NVN_EVENT_SIGNAL_MODE_ADD) *o->val += o->v; else *o->val = o->v; }
        /* NVNcounterData {counter, timestamp}: samples passed reports visible (no draws ran), the others 0 */
        else if (o->kind == 2) { uint64_t rec[2] = {o->mode == NVN_COUNTER_TYPE_SAMPLES_PASSED ? 1u << 20 : 0, gpu_ticks()}; memcpy((void *)(uintptr_t)o->addr, rec, sizeof rec); }
        else if (o->kind == 3) memmove((void *)(uintptr_t)o->addr, (const void *)(uintptr_t)o->src, o->n);
        else if (o->kind == 4) { memcpy((void *)(uintptr_t)o->addr, o->data, o->n); free((void *)o->data); }
    }
    g_npost = 0;
    return atomic_fetch_add(&g_fence, 1) + 1;
}
uint64_t mtl_last_fence(void) { return atomic_load(&g_fence); }
int mtl_wait(uint64_t fence, uint64_t timeout_ns) { (void)fence; (void)timeout_ns; return 1; }
void mtl_finish(void) {}
void mtl_drain(void) {}
void mtl_wb_fence_done(void) {}

/* Objects: nothing on the device */
void mtl_reg_tex(int id, const tex_t *t, const view_t *v) { (void)id; (void)t; (void)v; }
void mtl_reg_samp(int id, const samp_par *p) { (void)id; (void)p; }
void mtl_set_tex_pool_n(int n) { (void)n; }
void mtl_tex_release(tex_t *t) { (void)t; }
void mtl_prog_release(prog_t *p) { (void)p; }
void mtl_prog_prefetch(const prog_t *p) { (void)p; }
void mtl_dead_forget(const uint64_t *serials, size_t n) { (void)serials; (void)n; }
void mtl_pin_refs(void (*mark)(uint64_t)) { (void)mark; }
void mtl_xv_window_tex(uint64_t serial) { (void)serial; }
void mtl_read_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, uint8_t *dst, size_t row, size_t img) { (void)t; (void)level; (void)layer0; (void)r; (void)dst; (void)row; (void)img; }
void mtl_write_texels(const tex_t *t, unsigned level, unsigned layer0, const NVNcopyRegion *r, const uint8_t *src, size_t row, size_t img) { (void)t; (void)level; (void)layer0; (void)r; (void)src; (void)row; (void)img; }
void mtl_probe_pin_rt(const tex_t *t) { (void)t; }

/* Presentation: no window; one present a frame, paced by the game */
static const char *const op_names[OP_COUNT_] = {"END", "JUMP", "BEGIN", "CALL", "SIGNAL_EVENT", "REPORT_COUNTER", "COPY_B2B", "UPDATE_UB",
    "SET_RT", "CLEAR_COLOR", "CLEAR_DS", "VIEWPORTS", "SCISSOR", "DEPTH_RANGE", "BIND_PROGRAM", "BIND_STATE", "BIND_VATTR", "BIND_VSTREAM", "BIND_VBUF",
    "BIND_UBO", "BIND_SSBO", "BIND_TEX", "BIND_SAMP", "STENCIL", "POLY_OFFSET", "DRAW", "DRAW_IDX", "DISPATCH", "BARRIER", "SET_TEX_POOL", "SET_SAMP_POOL",
    "COPY_B2T", "COPY_STRIDE", "BLEND_COLOR", "COPY_T2B", "BIND_IMG", "COPY_T2T", "RESET_COUNTER", "CLEAR_TEX", "DEPTH_CLAMP", "RENDER_ENABLE",
    "RASTER_DISCARD", "RENDER_COND"};
uint64_t mtl_present(const tex_t *t, double period_s, const int crop[4]) {
    (void)t; (void)period_s; (void)crop; g_presents++;
    if (stats_on() && g_presents % 600 == 0) {
        char b[2048]; size_t o = 0;
        for (int i = 0; i < OP_COUNT_; i++) if (g_ops[i] && o < sizeof b - 64) o += (size_t)snprintf(b + o, sizeof b - o, " %s=%llu", op_names[i] ? op_names[i] : "?", (unsigned long long)g_ops[i]);
        LOG("null: %llu presents, ops since start:%s", (unsigned long long)g_presents, b);
    }
    return g_presents;
}
void mtl_present_wait(uint64_t ticket) { (void)ticket; }
double mtl_presented_time(uint64_t n) { (void)n; return 0; }
uint32_t mtl_present_qos(void) { return 0; }
void mtl_shot_at_present(const tex_t *t) { (void)t; }
int mtl_has_window(void) { return 0; }
int mtl_display_hz(void) { return 0; }
int mtl_link_active(void) { return 0; }
int mtl_link_wait_tick(uint64_t timeout_ns, unsigned frame_ticks) { (void)timeout_ns; (void)frame_ticks; return 0; }
int mtl_warm_wait(int timeout_ms) { (void)timeout_ms; return 0; }
void mtl_set_frame(uint64_t f) { atomic_store(&g_frame, f); }
void mtl_ub_present(double iv_ms) { (void)iv_ms; }
void mtl_note_world(void) {}
int mtl_pipeline_archive_tool(int argc, char **argv) { (void)argc; (void)argv; return 1; }

/* MetalFX keys and diagnostics of the Metal backend: off */
int mfx_enabled(void) { return 0; }
int mfx_latch(void) { return 0; }
void mfx_set(int mode) { (void)mode; }
int mfxup_enabled(void) { return 0; }
__attribute__((weak)) int dm_detclock_enabled(void) { return 0; }
__attribute__((weak)) void dm_guest_frame_tick(void) {}
uint64_t g_draw_skip[16];
void dm_gfx_anomdump(void) {}
void dm_gfx_ashot(const char *path) { (void)path; }
void dm_gfx_passdump_next(long n) { (void)n; }
void dm_gfx_querylog_next(long n) { (void)n; }
void dm_gfx_set_flag(const char *name, int value) { (void)name; (void)value; }
void dm_gfx_shot_at(long frames, const char *path) { (void)frames; (void)path; }
void dm_gfx_texdump_next(const char *path) { (void)path; }
void dm_gfx_window_shot(const char *path) { (void)path; }
void dm_nvn_probe_prediction(void) {}
void mtl_res_counts(uint32_t tick, const char *why) { (void)tick; (void)why; }

/* Statistics of the Metal backend: zero */
void mtl_alloc_stats(uint64_t *a, uint64_t *b, int *c) { *a = *b = 0; *c = 0; }
void mtl_args_stats(uint64_t *enc, uint64_t *reuse) { *enc = *reuse = 0; }
void mtl_blit_stats(uint64_t v[6]) { memset(v, 0, 6 * sizeof *v); }
void mtl_bw(uint64_t *load, uint64_t *store, uint64_t *full) { *load = *store = *full = 0; }
uint64_t mtl_chunk_commits(void) { return 0; }
void mtl_create_ns(uint64_t *lib_ns, uint64_t *pso_ns) { *lib_ns = *pso_ns = 0; }
void mtl_create_stats(uint64_t *lib, uint64_t *pso) { *lib = *pso = 0; }
uint64_t mtl_drawable_ns_take(void) { return 0; }
uint64_t mtl_ds_omit(void) { return 0; }
double mtl_gpu_ms(uint64_t f) { (void)f; return 0; }
int mtl_gpu_span(uint64_t f, uint64_t *t0, uint64_t *t1) { (void)f; *t0 = *t1 = 0; return 0; }
void mtl_keepalive_stats(uint64_t v[3]) { memset(v, 0, 3 * sizeof *v); }
void mtl_layer_stats(uint64_t v[4]) { memset(v, 0, 4 * sizeof *v); }
void mtl_mem_stat(unsigned long long a, unsigned long long b) { (void)a; (void)b; }
uint64_t mtl_pkg_miss(void) { return 0; }
void mtl_pkg_stats(uint64_t *uploads, uint64_t *reuploads) { *uploads = *reuploads = 0; }
void mtl_pool_res_stats(uint64_t v[4]) { memset(v, 0, 4 * sizeof *v); }
void mtl_ren_stats(uint64_t v[6]) { memset(v, 0, 6 * sizeof *v); }
void mtl_scan_stats(uint64_t *shader_ns, uint64_t *pso_ns) { *shader_ns = *pso_ns = 0; }
void mtl_stats(uint64_t *passes, uint64_t *draws) { *passes = 0; *draws = g_ops[OP_DRAW] + g_ops[OP_DRAW_IDX]; }
uint64_t mtl_udc_saved_bytes(void) { return 0; }
uint64_t mtl_upload_bytes(void) { return 0; }
uint64_t mtl_upload_gpu_ns_take(void) { return 0; }
uint64_t mtl_upload_ns_take(void) { return 0; }
void mtl_upload_wait_stats(uint64_t *a, uint64_t *b) { *a = *b = 0; }
void mtl_use_stats(uint64_t *call, uint64_t *skip) { *call = *skip = 0; }
void mtl_variant_stats(uint64_t *n, uint64_t *ns, uint64_t *pre, uint64_t *bg, uint64_t *plain) { *n = *ns = *pre = *bg = *plain = 0; }
uint64_t mtl_view_stats(void) { return 0; }
void mtl_wait_stats(uint64_t *plib_ns, uint64_t *pso_ns) { *plib_ns = *pso_ns = 0; }
void totk_rt_submit_stats(uint64_t *wait_n, uint64_t *wait_ns, uint64_t *sync_n, uint64_t *sync_ns) { *wait_n = *wait_ns = *sync_n = *sync_ns = 0; }
int thread_selfcounts(int type, void *buf, size_t nbytes) { (void)type; memset(buf, 0, nbytes); return -1; }
