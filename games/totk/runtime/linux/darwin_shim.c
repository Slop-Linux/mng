/* Linux: the out-of-line parts of the Darwin shims (runtime/linux/include). */
#include <mach/mach_time.h>
#include <pthread/qos.h>
uint64_t mach_absolute_time(void) { return clock_gettime_nsec_np(CLOCK_MONOTONIC); }
__thread qos_class_t darwin_qos_class;
