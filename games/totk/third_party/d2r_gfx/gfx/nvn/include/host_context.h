/* Signal context fields on both hosts: u->UC_PC, u->UC_X[i], u->UC_LR, u->UC_FP, u->UC_SP, u->UC_FAR, UC_ESR(u), UC_V(u)[i]. */
#pragma once
#include <stdint.h>
#include <sys/ucontext.h>
#ifdef __linux__
#include <asm/sigcontext.h>
#define UC_PC uc_mcontext.pc
#define UC_X uc_mcontext.regs
#define UC_LR uc_mcontext.regs[30]
#define UC_FP uc_mcontext.regs[29]
#define UC_SP uc_mcontext.sp
#define UC_FAR uc_mcontext.fault_address
static inline const struct _aarch64_ctx *uc_record(const ucontext_t *u, uint32_t magic) {
    const uint8_t *p = (const uint8_t *)u->uc_mcontext.__reserved, *end = p + sizeof u->uc_mcontext.__reserved;
    while (p + sizeof(struct _aarch64_ctx) <= end) {
        const struct _aarch64_ctx *h = (const struct _aarch64_ctx *)p;
        if (!h->magic || !h->size) break;
        if (h->magic == magic) return h;
        p += h->size;
    }
    return 0;
}
static inline uint64_t UC_ESR(const ucontext_t *u) { const struct esr_context *e = (const struct esr_context *)uc_record(u, ESR_MAGIC); return e ? e->esr : 0; }
static inline __uint128_t *UC_V(ucontext_t *u) { static __thread __uint128_t none[32]; struct fpsimd_context *f = (struct fpsimd_context *)uc_record(u, FPSIMD_MAGIC); return f ? f->vregs : none; }
#else
#define UC_PC uc_mcontext->__ss.__pc
#define UC_X uc_mcontext->__ss.__x
#define UC_LR uc_mcontext->__ss.__lr
#define UC_FP uc_mcontext->__ss.__fp
#define UC_SP uc_mcontext->__ss.__sp
#define UC_FAR uc_mcontext->__es.__far
#define UC_ESR(u) ((u)->uc_mcontext->__es.__esr)
#define UC_V(u) ((u)->uc_mcontext->__ns.__v)
#endif
