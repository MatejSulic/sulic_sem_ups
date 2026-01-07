#include "log.h"
#include <stdarg.h>
#include <stdio.h>
#include <time.h>

static void log_common(const char *level, const char *fmt, va_list ap) {
    time_t now = time(NULL);
    struct tm *tm = localtime(&now);

    char ts[32];
    strftime(ts, sizeof(ts), "%H:%M:%S", tm);

    fprintf(stderr, "[%s] %s ", level, ts);
    vfprintf(stderr, fmt, ap);
    fprintf(stderr, "\n");
}

void log_info(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_common("INFO", fmt, ap);
    va_end(ap);
}

void log_warn(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_common("WARN", fmt, ap);
    va_end(ap);
}

void log_error(const char *fmt, ...) {
    va_list ap;
    va_start(ap, fmt);
    log_common("ERROR", fmt, ap);
    va_end(ap);
}
