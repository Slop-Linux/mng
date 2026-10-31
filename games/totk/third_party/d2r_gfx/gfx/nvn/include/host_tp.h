/* Per-thread host slots that the rewritten code reaches through the thread pointer (patch_nso.py KEY_INDEX: 0 K18, 1 KTLS,
   2 KTLR, 3 KGATE; 4 the host-stack top of hle_stack.S). The key page holds each slot's offset from the thread pointer.
   Darwin: pthread TSD slots at TPIDRRO_EL0 + key * 8. Linux, where TPIDRRO_EL0 reads 0: a local-exec TLS array at a fixed
   offset from TPIDR_EL0, and the loader turns the rewriter's mrs tpidrro_el0 into mrs tpidr_el0. */
#pragma once
#include <stdint.h>
#include <pthread.h>
#define RUNTIME_NSLOT 8
#ifdef __linux__
extern __thread uint64_t runtime_tp_slot[RUNTIME_NSLOT] __attribute__((tls_model("local-exec")));
static inline uintptr_t runtime_tp(void) { uintptr_t tp; __asm__ volatile("mrs %0, tpidr_el0" : "=r"(tp)); return tp; }
static inline uint64_t runtime_slot_offset(unsigned i) { return (uintptr_t)&runtime_tp_slot[i] - runtime_tp(); }
static inline void *runtime_slot_get(unsigned i) { return (void *)(uintptr_t)runtime_tp_slot[i]; }
static inline int runtime_slot_set(unsigned i, const void *v) { runtime_tp_slot[i] = (uintptr_t)v; return 0; }
#else
extern pthread_key_t runtime_keys[8];
static inline uintptr_t runtime_tp(void) { uintptr_t tp; __asm__ volatile("mrs %0, tpidrro_el0" : "=r"(tp)); return tp; }
static inline uint64_t runtime_slot_offset(unsigned i) { return (uint64_t)runtime_keys[i] * 8; }
static inline void *runtime_slot_get(unsigned i) { return pthread_getspecific(runtime_keys[i]); }
static inline int runtime_slot_set(unsigned i, const void *v) { return pthread_setspecific(runtime_keys[i], v); }
#endif
