/* Linux frontend: nn::vi (display, layer, native window, vsync event) and the boot progress lines. The window is the
   graphics backend's (SDL3 + Vulkan); without one (null backend, TOTK_HEADLESS) the native window is a token and the
   vsync event ticks at 60 Hz from a host thread. */
#include "runtime.h"
#include "../../third_party/d2r_gfx/host/front.h"
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

void hle_signal_system_event(void *ev);
static uint64_t boot_ns;
static int vi_ready;
static char display_identity, layer_identity, native_window;
static pthread_mutex_t vsync_lock = PTHREAD_MUTEX_INITIALIZER;
static void *vsync_events[16]; static unsigned vsync_count;
static _Atomic uint64_t presented;

void *front_metal_layer(void) { return &native_window; }
void front_frame_presented(void) { if (atomic_fetch_add(&presented, 1) == 0) fprintf(stderr, "[totk-boot] %.3f first presented frame\n", (runtime_now_ns() - boot_ns) / 1e9); }
uint64_t front_game_res(int docked) { (void)docked; return (720ULL << 32) | 1280; }
int runtime_front_focus(void) { return 1; }
void runtime_front_request_close(void) { fprintf(stderr, "[totk] close requested\n"); exit(0); }
void runtime_front_text_test(const char *op, const char *text) { (void)op; (void)text; }
void runtime_settings_toggle(void) {}
void runtime_boot_progress(const char *stage, uint64_t done, uint64_t total) {
    static const char *last; (void)done; (void)total;
    if (stage != last) { fprintf(stderr, "[totk-boot] %.3f %s\n", boot_ns ? (runtime_now_ns() - boot_ns) / 1e9 : 0, stage ? stage : "startup"); last = stage; }
}
void runtime_boot_count(const char *stage, uint64_t done, uint64_t total) { runtime_boot_progress(stage, done, total); }
void runtime_boot_status(const char *stage, const char *detail) {
    if (boot_ns) fprintf(stderr, "[totk-boot] %.3f %s — %s\n", (runtime_now_ns() - boot_ns) / 1e9, stage ? stage : "startup", detail ? detail : "");
}
void runtime_warm_progress(uint64_t done, uint64_t total, uint64_t shaders, uint64_t shader_total, uint64_t pipelines, uint64_t pipeline_total) {
    (void)done; (void)total; (void)shaders; (void)shader_total; (void)pipelines; (void)pipeline_total;
}
void runtime_warm_done(void) {}

static void *vsync_thread(void *arg) {
    (void)arg; struct timespec t; clock_gettime(CLOCK_MONOTONIC, &t);
    for (;;) {
        t.tv_nsec += 16666667; if (t.tv_nsec >= 1000000000) { t.tv_sec++; t.tv_nsec -= 1000000000; }
        clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL);
        pthread_mutex_lock(&vsync_lock); for (unsigned i = 0; i < vsync_count; i++) hle_signal_system_event(vsync_events[i]); pthread_mutex_unlock(&vsync_lock);
    }
    return NULL;
}
static void vi_initialize(void) { vi_ready = 1; fprintf(stderr, "[totk] VI display initialized (Linux)\n"); }
static uint32_t vi_open(void **out) { if (!vi_ready || !out) return 0xf601; *out = &display_identity; return 0; }
static uint32_t vi_layer(void **out, void *display) { if (!out || display != &display_identity) return 0xf601; *out = &layer_identity; return 0; }
static uint32_t vi_window(void **out, void *layer) { if (!out || layer != &layer_identity) return 0xf601; *out = front_metal_layer(); return 0; }
static uint32_t vi_vsync(void *ev, void *display) {
    if (!ev || display != &display_identity) return 0xf601;
    hle_bind_system_event(ev, 1);
    pthread_mutex_lock(&vsync_lock);
    if (vsync_count >= 16) runtime_fail("VI display event limit");
    vsync_events[vsync_count++] = ev;
    static int started; if (!started) { pthread_t th; started = !pthread_create(&th, NULL, vsync_thread, NULL); if (started) pthread_detach(th); }
    pthread_mutex_unlock(&vsync_lock);
    return 0;
}
#ifdef GAME_BOTW
static uint32_t vi_scaling_mode(void *layer, int mode) { (void)layer; (void)mode; return 0; }
#endif
void *runtime_front_override(const char *name) {
    static const struct { const char *name; void *fn; } entries[] = {
        {"_ZN2nn2vi10InitializeEv", vi_initialize}, {"_ZN2nn2vi18OpenDefaultDisplayEPPNS0_7DisplayE", vi_open},
        {"_ZN2nn2vi11CreateLayerEPPNS0_5LayerEPNS0_7DisplayE", vi_layer}, {"_ZN2nn2vi15GetNativeWindowEPPvPNS0_5LayerE", vi_window},
        {"_ZN2nn2vi20GetDisplayVsyncEventEPNS_2os15SystemEventTypeEPNS0_7DisplayE", vi_vsync},
#ifdef GAME_BOTW
        {"_ZN2nn2vi19SetLayerScalingModeEPNS0_5LayerENS0_11ScalingModeE", vi_scaling_mode},
#endif
    };
    for (unsigned i = 0; i < sizeof entries / sizeof *entries; i++) if (!strcmp(name, entries[i].name)) return entries[i].fn;
    return NULL;
}
void runtime_front_run(void (*callback)(void)) {
    boot_ns = runtime_now_ns();
    const char *commit = getenv("TOTK_SOURCE_COMMIT");
    fprintf(stderr, "[totk] built from commit %s\n", commit && *commit ? commit : "(not recorded)");
    runtime_gamepad_start();
    callback();
}
void runtime_gpu_path_probe(const char *which) { fprintf(stderr, "[totk] %s: GPU path probes are Metal-only\n", which); exit(2); }
void runtime_vpool_probe(void) { fprintf(stderr, "[totk] probe-vpool: Metal-only\n"); exit(2); }
