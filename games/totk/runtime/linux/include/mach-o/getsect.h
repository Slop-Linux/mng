/* Linux: getsectiondata over ELF sections named by DM_SEC (the linker's __start_ / __stop_ symbols). */
#pragma once
#include <string.h>
extern const char __start_swnvn[] __attribute__((weak)), __stop_swnvn[] __attribute__((weak));
extern const char __start_totknvnx[] __attribute__((weak)), __stop_totknvnx[] __attribute__((weak));
extern const char __start_totkgfx[] __attribute__((weak)), __stop_totkgfx[] __attribute__((weak));
static inline const void *getsectiondata(const void *mh, const char *seg, const char *sect, unsigned long *size) {
    (void)mh; (void)seg; const char *a = NULL, *b = NULL;
    if (!strcmp(sect, "swnvn")) { a = __start_swnvn; b = __stop_swnvn; }
    else if (!strcmp(sect, "totknvnx")) { a = __start_totknvnx; b = __stop_totknvnx; }
    else if (!strcmp(sect, "totkgfx")) { a = __start_totkgfx; b = __stop_totkgfx; }
    *size = a && b ? (unsigned long)(b - a) : 0;
    return a;
}
