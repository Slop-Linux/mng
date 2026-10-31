#pragma once
#include <stddef.h>
static inline void sys_icache_invalidate(void *p, size_t n) { __builtin___clear_cache((char *)p, (char *)p + n); }
static inline void sys_dcache_flush(void *p, size_t n) {
    for (char *a = (char *)((uintptr_t)p & ~(uintptr_t)63), *e = (char *)p + n; a < e; a += 64) __asm__ volatile("dc civac, %0" :: "r"(a) : "memory");
    __asm__ volatile("dsb ish" ::: "memory");
}
