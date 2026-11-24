// The fatal line and crash spot, kept for the crash message (plat_fatal_notice); the details stay in the log.
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

#include "platform.h"
#include "rt.h"

static char g_line[512];   // the first fatal line (FATAL message / "signal 11 accessing 0x10")
int rt_romfs_files = -1;   // RomFS files indexed (services/fs.c; -1: not read yet)
uint64_t rt_romfs_bytes;
int rt_romfs_missing;

void rt_fatal_msg(const char *fmt, ...) {
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof b, fmt, ap);
    va_end(ap);
    fprintf(stderr, "FATAL: %s", b);
    if (!g_line[0]) snprintf(g_line, sizeof g_line, "%s", b);
}

void rt_crash_fault(const char *what, uint64_t fa, uint64_t pc) {
    (void)pc;
    if (!g_line[0]) snprintf(g_line, sizeof g_line, "%s accessing %#llx", what, (unsigned long long)fa);
}

void rt_crash_explain(char *out, size_t n, const char *log_path) {
    snprintf(out, n, "YKW Recomp stopped: %s\n\nDetails are in %s.", g_line[0] ? g_line : "unknown error",
             log_path ? log_path : "the log");
}
