/* Assembly symbols on both hosts: Mach-O prefixes C symbols with '_' and addresses pages with @PAGE / @PAGEOFF; ELF uses
   the plain name and :lo12:. TP_EL0 is the register that holds the host thread pointer of host_tp.h. */
#pragma once
#ifdef __APPLE__
#define SYM(x) _##x
#define PAGE(x) _##x@PAGE
#define PAGEOFF(x) _##x@PAGEOFF
#define TP_EL0 tpidrro_el0
#else
#define SYM(x) x
#define PAGE(x) x
#define PAGEOFF(x) :lo12:x
#define TP_EL0 tpidr_el0
#endif
#ifdef __APPLE__
#define CSTRING_SECTION .cstring
#else
#define CSTRING_SECTION .section .rodata.str1.1,"aMS",%progbits,1
#endif
