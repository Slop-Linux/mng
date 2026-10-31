/* Linux: os_unfair_lock as a futex lock (0 free, 1 held, 2 held with waiters). */
#pragma once
#include <stdint.h>
#include <linux/futex.h>
#include <sys/syscall.h>
#include <unistd.h>
typedef struct { uint32_t v; } os_unfair_lock, *os_unfair_lock_t;
#define OS_UNFAIR_LOCK_INIT ((os_unfair_lock){0})
static inline void os_unfair_lock_lock(os_unfair_lock_t l) {
    uint32_t c = 0;
    if (__atomic_compare_exchange_n(&l->v, &c, 1, 0, __ATOMIC_ACQUIRE, __ATOMIC_RELAXED)) return;
    if (c != 2) c = __atomic_exchange_n(&l->v, 2, __ATOMIC_ACQUIRE);
    while (c) { syscall(SYS_futex, &l->v, FUTEX_WAIT_PRIVATE, 2, NULL, NULL, 0); c = __atomic_exchange_n(&l->v, 2, __ATOMIC_ACQUIRE); }
}
static inline void os_unfair_lock_unlock(os_unfair_lock_t l) {
    if (__atomic_exchange_n(&l->v, 0, __ATOMIC_RELEASE) == 2) syscall(SYS_futex, &l->v, FUTEX_WAKE_PRIVATE, 1, NULL, NULL, 0);
}
