/* Linux: the Darwin extensions the runtime uses, on glibc. Force-included (-include) by Makefile.linux. */
#pragma once
#ifndef _GNU_SOURCE
#define _GNU_SOURCE 1
#endif
#include <stdint.h>
#include <stddef.h>
#include <pthread.h>
#include <time.h>
#include <unistd.h>
#include <fcntl.h>
#include <sys/syscall.h>
#ifdef __cplusplus
extern "C" {
#endif
/* ELF sections named like the Mach-O "__DATA,<name>" ones: section(DM_SEC("name")) and getsectiondata (mach-o/getsect.h) */
#define DM_SEC(name) name
/* the calling thread's id (callers pass NULL or themselves, or log another thread's: that one is not known here) */
#define pthread_threadid_np(t, id) (*(id) = (uint64_t)syscall(SYS_gettid), 0)
static inline int pthread_cond_timedwait_relative_np(pthread_cond_t *c, pthread_mutex_t *m, const struct timespec *rel) {
    struct timespec now; clock_gettime(CLOCK_REALTIME, &now);
    now.tv_sec += rel->tv_sec; now.tv_nsec += rel->tv_nsec; if (now.tv_nsec >= 1000000000) { now.tv_sec++; now.tv_nsec -= 1000000000; }
    return pthread_cond_timedwait(c, m, &now);
}
static inline void *pthread_get_stackaddr_np(pthread_t t) {   /* Darwin: the top of the stack */
    pthread_attr_t a; void *lo = NULL; size_t n = 0;
    if (pthread_getattr_np(t, &a)) return NULL;
    pthread_attr_getstack(&a, &lo, &n); pthread_attr_destroy(&a); return (char *)lo + n;
}
static inline size_t pthread_get_stacksize_np(pthread_t t) {
    pthread_attr_t a; void *lo = NULL; size_t n = 0;
    if (pthread_getattr_np(t, &a)) return 0;
    pthread_attr_getstack(&a, &lo, &n); pthread_attr_destroy(&a); return n;
}
/* CLOCK_UPTIME_RAW: monotonic, not counting sleep (CLOCK_MONOTONIC_RAW); clock_gettime_nsec_np */
#define CLOCK_UPTIME_RAW CLOCK_MONOTONIC_RAW
static inline uint64_t clock_gettime_nsec_np(clockid_t c) { struct timespec t; clock_gettime(c, &t); return (uint64_t)t.tv_sec * 1000000000ull + (uint64_t)t.tv_nsec; }
/* struct stat times (no birth time: the change time) */
#define st_mtimespec st_mtim
#define st_ctimespec st_ctim
#define st_atimespec st_atim
#define st_birthtimespec st_ctim
/* mincore: Linux reports residency in bit 0 only */
#define MINCORE_INCORE 1
#define MINCORE_PAGED_OUT 0
#define F_FULLFSYNC (-1)   /* fcntl(fd, F_FULLFSYNC) callers use fsync (fs_host.c) */
#ifdef __cplusplus
}
#endif
/* Darwin names the calling thread only */
#define pthread_setname_np(name) pthread_setname_np(pthread_self(), name)
