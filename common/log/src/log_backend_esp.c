#include "wlh/log.h"

#if defined(WLH_LOG_BACKEND_ESP)

#include "esp_log.h"

void wlh_log_init(void) {
    /* ESP-IDF logging is initialized by the framework before app_main(). */
}

#endif
