#include "wlh/log.h"

#if defined(WLH_LOG_BACKEND_RTT_ULOG)

#include "ulog.h"

void wlh_log_init(void) {
    ulog_init();
}

#endif
