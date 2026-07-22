#ifndef WLH_LOG_H
#define WLH_LOG_H

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum wlh_log_level {
    WLH_LOG_LEVEL_ASSERT = 0,
    WLH_LOG_LEVEL_ERROR,
    WLH_LOG_LEVEL_WARN,
    WLH_LOG_LEVEL_INFO,
    WLH_LOG_LEVEL_DEBUG,
    WLH_LOG_LEVEL_VERBOSE
} wlh_log_level_t;

/* Backend selection is performed by the build system via one of the
 * WLH_LOG_BACKEND_* compile definitions. */
#if defined(WLH_LOG_BACKEND_EASYLOGGER)

#include "elog.h"

#define WLH_LOGA(tag, ...) elog_a(tag, __VA_ARGS__)
#define WLH_LOGE(tag, ...) elog_e(tag, __VA_ARGS__)
#define WLH_LOGW(tag, ...) elog_w(tag, __VA_ARGS__)
#define WLH_LOGI(tag, ...) elog_i(tag, __VA_ARGS__)
#define WLH_LOGD(tag, ...) elog_d(tag, __VA_ARGS__)
#define WLH_LOGV(tag, ...) elog_v(tag, __VA_ARGS__)

#elif defined(WLH_LOG_BACKEND_ESP)

#include "esp_log.h"

#define WLH_LOGA(tag, ...) ESP_LOGV(tag, __VA_ARGS__)
#define WLH_LOGE(tag, ...) ESP_LOGE(tag, __VA_ARGS__)
#define WLH_LOGW(tag, ...) ESP_LOGW(tag, __VA_ARGS__)
#define WLH_LOGI(tag, ...) ESP_LOGI(tag, __VA_ARGS__)
#define WLH_LOGD(tag, ...) ESP_LOGD(tag, __VA_ARGS__)
#define WLH_LOGV(tag, ...) ESP_LOGV(tag, __VA_ARGS__)

#elif defined(WLH_LOG_BACKEND_PRINTF)

void wlh_log_printf(
    wlh_log_level_t level, const char *tag, const char *fmt, ...
);

#define WLH_LOGA(tag, ...)                                                     \
    wlh_log_printf(WLH_LOG_LEVEL_ASSERT, tag, __VA_ARGS__)
#define WLH_LOGE(tag, ...) wlh_log_printf(WLH_LOG_LEVEL_ERROR, tag, __VA_ARGS__)
#define WLH_LOGW(tag, ...) wlh_log_printf(WLH_LOG_LEVEL_WARN, tag, __VA_ARGS__)
#define WLH_LOGI(tag, ...) wlh_log_printf(WLH_LOG_LEVEL_INFO, tag, __VA_ARGS__)
#define WLH_LOGD(tag, ...) wlh_log_printf(WLH_LOG_LEVEL_DEBUG, tag, __VA_ARGS__)
#define WLH_LOGV(tag, ...)                                                     \
    wlh_log_printf(WLH_LOG_LEVEL_VERBOSE, tag, __VA_ARGS__)

#else

/* No backend selected: logging compiles away. This keeps core portable when a
 * platform has not yet provided an adapter. */
#define WLH_LOGA(tag, ...) ((void)0)
#define WLH_LOGE(tag, ...) ((void)0)
#define WLH_LOGW(tag, ...) ((void)0)
#define WLH_LOGI(tag, ...) ((void)0)
#define WLH_LOGD(tag, ...) ((void)0)
#define WLH_LOGV(tag, ...) ((void)0)

#endif

/* Optional runtime initialization. Expands to nothing when no backend is
 * selected; otherwise it is provided by the active backend source. */
#if defined(WLH_LOG_BACKEND_EASYLOGGER) || defined(WLH_LOG_BACKEND_ESP) ||     \
    defined(WLH_LOG_BACKEND_PRINTF)
void wlh_log_init(void);
#define WLH_LOG_INIT() wlh_log_init()
#else
#define WLH_LOG_INIT() ((void)0)
#endif

#ifdef __cplusplus
}
#endif

#endif
