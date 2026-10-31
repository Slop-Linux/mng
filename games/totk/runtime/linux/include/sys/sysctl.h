/* Linux: no sysctlbyname (callers fall back). */
#pragma once
#include <stddef.h>
static inline int sysctlbyname(const char *n, void *o, size_t *on, void *i, size_t in) { (void)n; (void)o; (void)on; (void)i; (void)in; return -1; }
