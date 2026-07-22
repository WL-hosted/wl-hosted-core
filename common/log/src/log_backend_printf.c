#include "wlh/log.h"

#if defined(WLH_LOG_BACKEND_PRINTF)

#include <stdarg.h>
#include <stdio.h>

static const char *level_char(wlh_log_level_t level) {
    switch (level) {
    case WLH_LOG_LEVEL_ASSERT:
        return "A";
    case WLH_LOG_LEVEL_ERROR:
        return "E";
    case WLH_LOG_LEVEL_WARN:
        return "W";
    case WLH_LOG_LEVEL_INFO:
        return "I";
    case WLH_LOG_LEVEL_DEBUG:
        return "D";
    case WLH_LOG_LEVEL_VERBOSE:
        return "V";
    default:
        return "?";
    }
}

void wlh_log_printf(
    wlh_log_level_t level, const char *tag, const char *fmt, ...
) {
    va_list args;
    va_start(args, fmt);
    fprintf(stderr, "[%s][%s] ", level_char(level), tag);
    vfprintf(stderr, fmt, args);
    fputc('\n', stderr);
    va_end(args);
}

void wlh_log_init(void) {
    /* No runtime initialization required for the printf backend. */
}

#endif
