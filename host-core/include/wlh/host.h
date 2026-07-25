#ifndef WLH_HOST_H
#define WLH_HOST_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wlh/osal.h"
#include "wlh/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WLH_HOST_MAX_PENDING 16u
#define WLH_HOST_CHANNEL_COUNT 256u
#define WLH_HOST_MAX_SSID_SIZE 32u
#define WLH_HOST_MAX_CREDENTIAL_SIZE 64u
#define WLH_HOST_MAX_USER_PAYLOAD_SIZE 512u
#define WLH_HOST_MAX_QUEUE_DEPTH 32u

typedef enum wlh_host_result {
    WLH_HOST_OK = 0,
    WLH_HOST_INVALID_ARGUMENT = -1,
    WLH_HOST_INVALID_STATE = -2,
    WLH_HOST_NO_MEMORY = -3,
    WLH_HOST_TRANSPORT_ERROR = -4,
    WLH_HOST_PROTOCOL_ERROR = -5,
    WLH_HOST_NO_CREDIT = -6,
    WLH_HOST_PENDING_FULL = -7,
    WLH_HOST_TIMEOUT = -8,
    WLH_HOST_SESSION_CHANGED = -9,
    WLH_HOST_NOT_FOUND = -10
} wlh_host_result_t;

typedef enum wlh_host_state {
    WLH_HOST_STATE_UNINITIALIZED = 0,
    WLH_HOST_STATE_TRANSPORT_STARTING,
    WLH_HOST_STATE_WAITING_FOR_PEER,
    WLH_HOST_STATE_NEGOTIATING,
    WLH_HOST_STATE_READY,
    WLH_HOST_STATE_CONGESTED,
    WLH_HOST_STATE_RECOVERING,
    WLH_HOST_STATE_FAILED,
    WLH_HOST_STATE_STOPPING
} wlh_host_state_t;

typedef enum wlh_host_event_kind {
    WLH_HOST_EVENT_STATE_CHANGED = 1,
    WLH_HOST_EVENT_WIFI_SCAN_RESULT,
    WLH_HOST_EVENT_WIFI_SCAN_COMPLETED,
    WLH_HOST_EVENT_WIFI_CONNECTED,
    WLH_HOST_EVENT_WIFI_DISCONNECTED,
    WLH_HOST_EVENT_ETHERNET_STA_RX,
    WLH_HOST_EVENT_PROTOCOL_FAULT,
    WLH_HOST_EVENT_USER_MESSAGE_RESULT,
    WLH_HOST_EVENT_WIFI_AP_CLIENT_JOINED,
    WLH_HOST_EVENT_WIFI_AP_CLIENT_LEFT,
    WLH_HOST_EVENT_ETHERNET_AP_RX
} wlh_host_event_kind_t;

typedef struct wlh_host_event {
    wlh_host_event_kind_t kind;
    wlh_host_state_t state;
    uint16_t service_id;
    uint16_t method_id;
    const uint8_t *payload;
    size_t payload_size;
} wlh_host_event_t;

typedef void (*wlh_transport_lifecycle_complete_fn)(
    void *completion_context, int status
);
/*
 * Lifecycle calls submit work and must not wait for hardware.  A zero return
 * means accepted; readiness/stoppage is reported exactly once via completion.
 * Completion runs from Adapter task context, never inline or from an ISR.
 */
typedef int (*wlh_transport_start_fn)(
    void *context,
    wlh_transport_lifecycle_complete_fn completion,
    void *completion_context
);
typedef int (*wlh_transport_stop_fn)(
    void *context,
    wlh_transport_lifecycle_complete_fn completion,
    void *completion_context
);
typedef void (*wlh_transport_tx_complete_fn)(
    void *completion_context, uint8_t *frame, size_t size, int status
);
/* Success transfers frame ownership to the Adapter until completion fires. */
typedef int (*wlh_transport_submit_tx_fn)(
    void *context,
    uint8_t *frame,
    size_t size,
    wlh_transport_tx_complete_fn completion,
    void *completion_context
);

typedef struct wlh_transport_ops {
    void *context;
    wlh_transport_start_fn start;
    wlh_transport_stop_fn stop;
    wlh_transport_submit_tx_fn submit_tx;
} wlh_transport_ops_t;

typedef uint8_t *(*wlh_buffer_alloc_fn)(void *context, size_t size);
typedef void (*wlh_buffer_free_fn)(void *context, uint8_t *buffer);

typedef struct wlh_buffer_ops {
    void *context;
    wlh_buffer_alloc_fn alloc;
    wlh_buffer_free_fn free;
} wlh_buffer_ops_t;

typedef void (*wlh_task_fn)(void *context);
typedef int (*wlh_executor_post_fn)(
    void *context, wlh_task_fn task, void *task_context
);

typedef struct wlh_executor_ops {
    void *context;
    /* Must enqueue without running task inline or waiting for the consumer. */
    wlh_executor_post_fn post;
} wlh_executor_ops_t;

typedef void (*wlh_host_event_fn)(void *context, const wlh_host_event_t *event);
typedef void (*wlh_rpc_completion_fn)(
    void *context,
    wlh_host_result_t result,
    uint16_t status_domain,
    int16_t status_code,
    const uint8_t *payload,
    size_t payload_size
);

typedef struct wlh_host_config {
    wlh_transport_ops_t transport;
    wlh_buffer_ops_t buffers;
    wlh_osal_ops_t osal;
    wlh_executor_ops_t executor;

    wlh_host_event_fn on_event;
    void *event_context;

    uint32_t max_frame_size;
    uint32_t rpc_timeout_ms;
    uint32_t heartbeat_timeout_ms;
    uint8_t max_pending_rpc;
    uint8_t core_queue_depth;
    uint32_t stop_timeout_ms;
    wlh_osal_task_attributes_t core_task;
} wlh_host_config_t;

typedef struct wlh_host_diagnostics {
    wlh_host_state_t state;
    uint32_t session_id;
    uint32_t pending_rpc;
    uint32_t tx_frames;
    uint32_t rx_frames;
    uint32_t rpc_timeouts;
    uint32_t checksum_errors;
    uint32_t sequence_gaps;
    uint32_t peer_resets;
    uint32_t transport_resets;
    uint32_t buffer_allocation_failures;
    uint64_t last_peer_activity_ms;
} wlh_host_diagnostics_t;

typedef struct wlh_pending_rpc {
    bool active;
    uint32_t session_id;
    uint32_t request_id;
    uint16_t service_id;
    uint16_t method_id;
    uint64_t deadline_ms;
    wlh_rpc_completion_fn completion;
    void *completion_context;
} wlh_pending_rpc_t;

typedef struct wlh_host {
    wlh_host_config_t config;
    wlh_host_state_t state;

    uint32_t session_id;
    uint32_t next_request_id;

    uint32_t tx_sequence[WLH_HOST_CHANNEL_COUNT];
    uint32_t expected_rx_sequence[WLH_HOST_CHANNEL_COUNT];
    uint32_t tx_credit[WLH_HOST_CHANNEL_COUNT];
    bool rx_sequence_valid[WLH_HOST_CHANNEL_COUNT];

    wlh_pending_rpc_t pending[WLH_HOST_MAX_PENDING];
    wlh_host_diagnostics_t diagnostics;

    uint64_t started_ms;

    wlh_osal_task_t core_task;
    wlh_osal_queue_t core_queue;
    wlh_osal_mutex_t state_mutex;
    uintptr_t core_queue_storage[WLH_HOST_MAX_QUEUE_DEPTH * 2u];
    bool worker_started;
    bool worker_stopping;
} wlh_host_t;

typedef struct wlh_wifi_scan_params {
    uint32_t scan_id;
    const uint8_t *ssid;
    size_t ssid_size;
    bool include_hidden;
    uint32_t max_results;
} wlh_wifi_scan_params_t;

typedef struct wlh_wifi_connect_params {
    const uint8_t *ssid;
    size_t ssid_size;
    const uint8_t *credential;
    size_t credential_size;
    uint32_t security;
    uint32_t timeout_ms;
} wlh_wifi_connect_params_t;

typedef struct wlh_wifi_start_ap_params {
    const uint8_t *ssid;
    size_t ssid_size;
    const uint8_t *credential;
    size_t credential_size;
    uint32_t security;
    uint32_t channel;
    uint32_t max_clients;
} wlh_wifi_start_ap_params_t;

wlh_host_result_t wlh_host_init(
    wlh_host_t *host, const wlh_host_config_t *config
);
wlh_host_result_t wlh_host_start(wlh_host_t *host);
wlh_host_result_t wlh_host_stop(wlh_host_t *host);
/* Nonblocking ingress: the frame is copied and consumed by the Core task. */
wlh_host_result_t wlh_host_on_frame(
    wlh_host_t *host, const uint8_t *frame, size_t size
);
void wlh_host_transport_lost(wlh_host_t *host);

wlh_host_result_t wlh_host_rpc_request(
    wlh_host_t *host,
    uint16_t service_id,
    uint16_t method_id,
    const uint8_t *payload,
    size_t payload_size,
    uint32_t timeout_ms,
    wlh_rpc_completion_fn completion,
    void *completion_context,
    uint32_t *request_id
);

wlh_host_result_t wlh_host_wifi_initialize(
    wlh_host_t *host, wlh_rpc_completion_fn completion, void *context
);
wlh_host_result_t wlh_host_wifi_scan(
    wlh_host_t *host,
    const wlh_wifi_scan_params_t *params,
    wlh_rpc_completion_fn completion,
    void *context
);
wlh_host_result_t wlh_host_wifi_connect(
    wlh_host_t *host,
    const wlh_wifi_connect_params_t *params,
    wlh_rpc_completion_fn completion,
    void *context
);
wlh_host_result_t wlh_host_wifi_disconnect(
    wlh_host_t *host, wlh_rpc_completion_fn completion, void *context
);
wlh_host_result_t wlh_host_wifi_start_ap(
    wlh_host_t *host,
    const wlh_wifi_start_ap_params_t *params,
    wlh_rpc_completion_fn completion,
    void *context
);
wlh_host_result_t wlh_host_wifi_stop_ap(
    wlh_host_t *host, wlh_rpc_completion_fn completion, void *context
);

/* Device Information service client. Sizes match the protocol schema. */
typedef struct wlh_host_device_info {
    char vendor[33];
    char mcu_model[33];
    uint8_t uid[32];
    size_t uid_size;
    char board_profile[65];
} wlh_host_device_info_t;

/* info is NULL unless result == WLH_HOST_OK, and is valid only for the
 * duration of the call. */
typedef void (*wlh_host_device_info_fn)(
    void *context,
    wlh_host_result_t result,
    uint16_t status_domain,
    int16_t status_code,
    const wlh_host_device_info_t *info
);

wlh_host_result_t wlh_host_get_device_info(
    wlh_host_t *host, wlh_host_device_info_fn completion, void *context
);

/* User Passthrough service client (RPC form). The completion reports the
 * SEND acknowledgement; an optional RESULT event arrives via
 * WLH_HOST_EVENT_USER_MESSAGE_RESULT. */
wlh_host_result_t wlh_host_user_message_send(
    wlh_host_t *host,
    uint32_t endpoint_id,
    uint32_t message_type,
    uint32_t flags,
    const uint8_t *payload,
    size_t payload_size,
    wlh_rpc_completion_fn completion,
    void *context
);

wlh_host_result_t wlh_host_ethernet_sta_send(
    wlh_host_t *host, const uint8_t *ethernet_frame, size_t size
);
wlh_host_result_t wlh_host_ethernet_ap_send(
    wlh_host_t *host, const uint8_t *ethernet_frame, size_t size
);

void wlh_host_get_diagnostics(
    const wlh_host_t *host, wlh_host_diagnostics_t *diagnostics
);

#ifdef WLH_ENABLE_TEST_HOOKS
void wlh_host_test_set_credit(
    wlh_host_t *host, uint8_t channel, uint32_t credit
);
void wlh_host_test_force_session_change(wlh_host_t *host, uint32_t session_id);
void wlh_host_test_expire_all(wlh_host_t *host);
#endif

#ifdef __cplusplus
}
#endif

#endif
