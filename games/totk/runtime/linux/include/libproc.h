/* Linux: libproc's process listing and paths from /proc. */
#pragma once
#include <dirent.h>
#include <stdlib.h>
#include <stdio.h>
#include <unistd.h>
#define PROC_PIDPATHINFO_MAXSIZE 4096
static inline int proc_pidpath(int pid, void *buf, uint32_t n) {
    char p[64]; snprintf(p, sizeof p, "/proc/%d/exe", pid);
    ssize_t k = readlink(p, (char *)buf, n ? n - 1 : 0); if (k <= 0) return 0; ((char *)buf)[k] = 0; return (int)k;
}
static inline int proc_listallpids(void *buf, int bytes) {   /* NULL: the count */
    DIR *d = opendir("/proc"); if (!d) return -1; struct dirent *e; int n = 0, cap = buf ? bytes / (int)sizeof(int) : 0;
    while ((e = readdir(d))) { int pid = atoi(e->d_name); if (pid <= 0) continue; if (buf && n < cap) ((int *)buf)[n] = pid; n++; }
    closedir(d); return buf && n > cap ? cap : n;
}
