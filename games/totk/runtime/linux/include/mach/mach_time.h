/* Linux: mach_absolute_time in nanoseconds (timebase 1/1), CLOCK_MONOTONIC (does not count suspend, as on Darwin);
   mach_continuous_time counts it (CLOCK_BOOTTIME). */
#pragma once
#include "darwin_compat.h"
typedef struct { uint32_t numer, denom; } mach_timebase_info_data_t;
static inline int mach_timebase_info(mach_timebase_info_data_t *t) { t->numer = 1; t->denom = 1; return 0; }
uint64_t mach_absolute_time(void);   /* runtime/linux/darwin_shim.c: an out-of-line symbol (nvn_entry.S calls it) */
static inline uint64_t mach_continuous_time(void) { return clock_gettime_nsec_np(CLOCK_BOOTTIME); }
static inline int mach_wait_until(uint64_t deadline) {
    struct timespec t = {(time_t)(deadline / 1000000000u), (long)(deadline % 1000000000u)};
    while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &t, NULL) == 4 /* EINTR */) {}
    return 0;
}
