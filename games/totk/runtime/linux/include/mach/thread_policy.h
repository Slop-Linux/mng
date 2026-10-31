/* Linux: Mach real-time thread policy; not available (callers log that it did not take). */
#pragma once
#include "mach/mach.h"
typedef struct { uint32_t period, computation, constraint; boolean_t preemptible; } thread_time_constraint_policy_data_t;
#define THREAD_TIME_CONSTRAINT_POLICY 2
#define THREAD_TIME_CONSTRAINT_POLICY_COUNT ((mach_msg_type_number_t)(sizeof(thread_time_constraint_policy_data_t) / 4))
static inline kern_return_t thread_policy_set(thread_act_t t, int f, thread_policy_t p, mach_msg_type_number_t n) { (void)t; (void)f; (void)p; (void)n; return KERN_FAILURE; }
static inline kern_return_t thread_policy_get(thread_act_t t, int f, thread_policy_t p, mach_msg_type_number_t *n, boolean_t *def) { (void)t; (void)f; (void)p; (void)n; if (def) *def = 1; return KERN_FAILURE; }
