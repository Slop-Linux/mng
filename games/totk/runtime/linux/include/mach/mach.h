/* Linux: the Mach calls the runtime uses. Memory reservation and safe reads are real; thread inspection (suspend, register
   state, debug registers) and port accounting fail, so the diagnostics that use them skip. */
#pragma once
#include "darwin_compat.h"
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <sys/mman.h>
#include <sys/uio.h>
typedef int kern_return_t;
typedef int boolean_t;
typedef unsigned natural_t;
typedef unsigned mach_port_t, vm_map_t, thread_act_t, task_t, mach_port_name_t, mem_entry_name_port_t;
typedef unsigned mach_msg_type_number_t;
typedef uint64_t mach_vm_address_t, mach_vm_size_t, mach_vm_offset_t, vm_address_t, vm_size_t, vm_offset_t, memory_object_offset_t;
typedef int vm_prot_t, vm_inherit_t, vm_region_flavor_t, thread_flavor_t, thread_policy_flavor_t, task_flavor_t;
typedef int *vm_region_info_t, *vm_region_recurse_info_t, *thread_state_t, *thread_info_t, *thread_policy_t, *task_info_t;
typedef thread_act_t *thread_act_array_t;
#define KERN_SUCCESS 0
#define KERN_FAILURE 5
#define KERN_NO_SPACE 3
#define KERN_INVALID_ADDRESS 1
#define MACH_PORT_NULL 0u
#define MACH_PORT_RIGHT_SEND 0
#define MEMORY_OBJECT_NULL 0u
#define VM_PROT_NONE 0
#define VM_PROT_READ 1
#define VM_PROT_WRITE 2
#define VM_PROT_EXECUTE 4
#define VM_PROT_ALL 7
#define VM_FLAGS_FIXED 0
#define VM_FLAGS_ANYWHERE 1
#define VM_FLAGS_OVERWRITE 0x4000
#define VM_INHERIT_DEFAULT 1
#define VM_INHERIT_NONE 2
#define FALSE 0
#define TRUE 1
#define vm_page_size ((uint64_t)sysconf(_SC_PAGESIZE))
static inline mach_port_t mach_task_self(void) { return 1; }
static inline mach_port_t mach_thread_self(void) { return (mach_port_t)syscall(SYS_gettid); }
static inline mach_port_t mach_host_self(void) { return 1; }
static inline mach_port_t pthread_mach_thread_np(pthread_t t) { (void)t; return 0; }
static inline pthread_t pthread_from_mach_thread_np(mach_port_t p) { (void)p; return 0; }
static inline kern_return_t mach_port_deallocate(mach_port_t t, mach_port_t p) { (void)t; (void)p; return KERN_SUCCESS; }
static inline kern_return_t mach_port_mod_refs(mach_port_t t, mach_port_t p, int r, int d) { (void)t; (void)p; (void)r; (void)d; return KERN_FAILURE; }
/* A read that fails instead of faulting: process_vm_readv on this process. */
static inline kern_return_t mach_vm_read_overwrite(mach_port_t t, mach_vm_address_t a, mach_vm_size_t n, mach_vm_address_t to, mach_vm_size_t *got) {
    (void)t; struct iovec l = {(void *)(uintptr_t)to, n}, r = {(void *)(uintptr_t)a, n};
    ssize_t k = process_vm_readv(getpid(), &l, 1, &r, 1, 0);
    if (got) *got = k > 0 ? (mach_vm_size_t)k : 0;
    return k == (ssize_t)n ? KERN_SUCCESS : KERN_INVALID_ADDRESS;
}
static inline kern_return_t vm_read_overwrite(mach_port_t t, vm_address_t a, vm_size_t n, vm_address_t to, vm_size_t *got) { return mach_vm_read_overwrite(t, a, n, to, got); }
static inline kern_return_t mach_vm_write(mach_port_t t, mach_vm_address_t a, vm_offset_t from, mach_msg_type_number_t n) {
    (void)t; struct iovec l = {(void *)(uintptr_t)from, n}, r = {(void *)(uintptr_t)a, n};
    return process_vm_writev(getpid(), &l, 1, &r, 1, 0) == (ssize_t)n ? KERN_SUCCESS : KERN_INVALID_ADDRESS;
}
static inline int mach_prot(vm_prot_t p) { return (p & 1 ? PROT_READ : 0) | (p & 2 ? PROT_WRITE : 0) | (p & 4 ? PROT_EXEC : 0); }
static inline kern_return_t mach_vm_allocate(mach_port_t t, mach_vm_address_t *a, mach_vm_size_t n, int flags) {
    (void)t; int fixed = !(flags & VM_FLAGS_ANYWHERE);
    void *p = mmap(fixed ? (void *)(uintptr_t)*a : NULL, n, PROT_READ | PROT_WRITE, MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
                   (fixed ? (flags & VM_FLAGS_OVERWRITE ? MAP_FIXED : MAP_FIXED_NOREPLACE) : 0), -1, 0);
    if (p == MAP_FAILED) return KERN_NO_SPACE;
    if (fixed && p != (void *)(uintptr_t)*a) { munmap(p, n); return KERN_NO_SPACE; }
    *a = (uintptr_t)p; return KERN_SUCCESS;
}
static inline kern_return_t mach_vm_deallocate(mach_port_t t, mach_vm_address_t a, mach_vm_size_t n) { (void)t; return munmap((void *)(uintptr_t)a, n) ? KERN_INVALID_ADDRESS : KERN_SUCCESS; }
static inline kern_return_t vm_deallocate(mach_port_t t, vm_address_t a, vm_size_t n) { return mach_vm_deallocate(t, a, n); }
/* Anonymous memory only (MEMORY_OBJECT_NULL): a reservation with the given current protection. */
static inline kern_return_t mach_vm_map(mach_port_t t, mach_vm_address_t *a, mach_vm_size_t n, mach_vm_offset_t mask, int flags,
                                        mem_entry_name_port_t obj, memory_object_offset_t off, boolean_t copy, vm_prot_t cur, vm_prot_t max, vm_inherit_t inh) {
    (void)t; (void)mask; (void)obj; (void)off; (void)copy; (void)max; (void)inh;
    if (obj != MEMORY_OBJECT_NULL) return KERN_FAILURE;
    int fixed = !(flags & VM_FLAGS_ANYWHERE);
    void *p = mmap(fixed ? (void *)(uintptr_t)*a : NULL, n, mach_prot(cur), MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE |
                   (fixed ? (flags & VM_FLAGS_OVERWRITE ? MAP_FIXED : MAP_FIXED_NOREPLACE) : 0), -1, 0);
    if (p == MAP_FAILED) return KERN_NO_SPACE;
    if (fixed && p != (void *)(uintptr_t)*a) { munmap(p, n); return KERN_NO_SPACE; }
    *a = (uintptr_t)p; return KERN_SUCCESS;
}
/* Regions from /proc/self/maps: the first mapping that ends above *a (Mach semantics), merged with adjacent ones of the same protection. */
typedef struct { vm_prot_t protection, max_protection; unsigned inheritance, shared, reserved, behavior; uint64_t offset; unsigned short user_wired_count; } vm_region_basic_info_data_64_t;
typedef struct { vm_prot_t protection, max_protection; unsigned user_tag, pages_resident, pages_dirtied, share_mode, is_submap; uint64_t offset; } vm_region_submap_info_data_64_t;
#define VM_REGION_BASIC_INFO_64 9
#define VM_REGION_BASIC_INFO_COUNT_64 ((mach_msg_type_number_t)(sizeof(vm_region_basic_info_data_64_t) / 4))
#define VM_REGION_SUBMAP_INFO_COUNT_64 ((mach_msg_type_number_t)(sizeof(vm_region_submap_info_data_64_t) / 4))
static inline kern_return_t mach_vm_region(mach_port_t t, mach_vm_address_t *a, mach_vm_size_t *n, vm_region_flavor_t f, vm_region_info_t info,
                                           mach_msg_type_number_t *cnt, mach_port_t *obj) {
    (void)t; (void)f; (void)cnt; if (obj) *obj = MACH_PORT_NULL;
    FILE *m = fopen("/proc/self/maps", "r"); if (!m) return KERN_FAILURE;
    char line[512]; unsigned long long lo, hi, flo = 0, fhi = 0; char perm[8]; int found = 0, prot = 0;
    while (fgets(line, sizeof line, m)) {
        if (sscanf(line, "%llx-%llx %7s", &lo, &hi, perm) != 3) continue;
        int p = (perm[0] == 'r' ? 1 : 0) | (perm[1] == 'w' ? 2 : 0) | (perm[2] == 'x' ? 4 : 0);
        if (!found) { if (hi > *a) { flo = lo; fhi = hi; prot = p; found = 1; } }
        else if (lo == fhi && p == prot) fhi = hi; else break;
    }
    fclose(m);
    if (!found) return KERN_INVALID_ADDRESS;
    *a = flo; *n = fhi - flo;
    vm_region_basic_info_data_64_t *b = (vm_region_basic_info_data_64_t *)info; memset(b, 0, sizeof *b); b->protection = prot; b->max_protection = 7;
    return KERN_SUCCESS;
}
static inline kern_return_t mach_vm_region_recurse(mach_port_t t, mach_vm_address_t *a, mach_vm_size_t *n, natural_t *depth, vm_region_recurse_info_t info, mach_msg_type_number_t *cnt) {
    (void)depth; vm_region_basic_info_data_64_t b; kern_return_t kr = mach_vm_region(t, a, n, 0, (vm_region_info_t)&b, cnt, NULL);
    if (kr == KERN_SUCCESS) { vm_region_submap_info_data_64_t *s = (vm_region_submap_info_data_64_t *)info; memset(s, 0, sizeof *s); s->protection = b.protection; s->max_protection = 7; }
    return kr;
}
/* Thread inspection: unavailable (diagnostics only). */
typedef struct { uint64_t __x[29], __fp, __lr, __sp, __pc; uint32_t __cpsr, __pad; } arm_thread_state64_t;
typedef struct { uint64_t __bvr[16], __bcr[16], __wvr[16], __wcr[16], __mdscr_el1; } arm_debug_state64_t;
#define arm_thread_state64_get_pc(s) ((s).__pc)
#define arm_thread_state64_get_lr(s) ((s).__lr)
#define arm_thread_state64_get_fp(s) ((s).__fp)
#define ARM_THREAD_STATE64 6
#define ARM_THREAD_STATE64_COUNT ((mach_msg_type_number_t)(sizeof(arm_thread_state64_t) / 4))
#define ARM_DEBUG_STATE64 15
#define ARM_DEBUG_STATE64_COUNT ((mach_msg_type_number_t)(sizeof(arm_debug_state64_t) / 4))
typedef struct { struct { int seconds, microseconds; } user_time, system_time; int cpu_usage, policy, run_state, flags, suspend_count, sleep_time; } thread_basic_info_data_t;
typedef struct { uint64_t thread_id, thread_handle, dispatch_qaddr; } thread_identifier_info_data_t;
typedef struct { uint64_t pth_user_time, pth_system_time; int pth_cpu_usage, pth_policy, pth_run_state, pth_flags, pth_sleep_time, pth_curpri, pth_priority, pth_maxpriority; char pth_name[64]; } thread_extended_info_data_t;
#define THREAD_BASIC_INFO 3
#define THREAD_BASIC_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_basic_info_data_t) / 4))
#define THREAD_IDENTIFIER_INFO 4
#define THREAD_IDENTIFIER_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_identifier_info_data_t) / 4))
#define THREAD_EXTENDED_INFO 5
#define THREAD_EXTENDED_INFO_COUNT ((mach_msg_type_number_t)(sizeof(thread_extended_info_data_t) / 4))
static inline kern_return_t thread_suspend(thread_act_t t) { (void)t; return KERN_FAILURE; }
static inline kern_return_t thread_resume(thread_act_t t) { (void)t; return KERN_FAILURE; }
static inline kern_return_t thread_get_state(thread_act_t t, int f, thread_state_t s, mach_msg_type_number_t *n) { (void)t; (void)f; (void)s; (void)n; return KERN_FAILURE; }
static inline kern_return_t thread_set_state(thread_act_t t, int f, thread_state_t s, mach_msg_type_number_t n) { (void)t; (void)f; (void)s; (void)n; return KERN_FAILURE; }
static inline kern_return_t thread_info(thread_act_t t, int f, thread_info_t i, mach_msg_type_number_t *n) { (void)t; (void)f; (void)i; (void)n; return KERN_FAILURE; }
static inline kern_return_t task_threads(mach_port_t t, thread_act_array_t *l, mach_msg_type_number_t *n) { (void)t; *l = NULL; *n = 0; return KERN_FAILURE; }
typedef struct { uint64_t virtual_size, phys_footprint, resident_size; } task_vm_info_data_t;
#define TASK_VM_INFO 22
#define TASK_VM_INFO_COUNT ((mach_msg_type_number_t)(sizeof(task_vm_info_data_t) / 4))
static inline kern_return_t task_info(mach_port_t t, int f, task_info_t i, mach_msg_type_number_t *n) {   /* phys_footprint ~ RSS */
    (void)t; (void)n; if (f != TASK_VM_INFO) return KERN_FAILURE;
    FILE *s = fopen("/proc/self/statm", "r"); unsigned long long size = 0, rss = 0;
    if (!s) return KERN_FAILURE; int ok = fscanf(s, "%llu %llu", &size, &rss) == 2; fclose(s); if (!ok) return KERN_FAILURE;
    task_vm_info_data_t *v = (task_vm_info_data_t *)i; v->virtual_size = size * vm_page_size; v->resident_size = v->phys_footprint = rss * vm_page_size;
    return KERN_SUCCESS;
}
#include "mach/thread_policy.h"
