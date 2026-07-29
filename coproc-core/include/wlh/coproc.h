#ifndef WLH_COPROC_H
#define WLH_COPROC_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include "wlh/osal.h"
#include "wlh/protocol.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WLH_COPROC_CHANNEL_COUNT 256u
#define WLH_COPROC_MAX_FRAME_SIZE 4096u
#define WLH_COPROC_MAX_QUEUE_DEPTH 32u
#define WLH_COPROC_MAX_HCI_PACKET 1024u
#define WLH_COPROC_BLUETOOTH_INITIAL_CREDIT 16u
/* Window for best-effort LE advertising reports on
 * WLH_CHANNEL_BLUETOOTH_HCI_ADV. Deliberately small: it bounds how many
 * reports can sit ahead of a reliable HCI event anywhere in the shared
 * transport pipeline, so control-plane acks are never head-of-line blocked
 * behind a report flood. Reports beyond the window are shed at the source. */
#define WLH_COPROC_BLUETOOTH_ADV_INITIAL_CREDIT 4u

/* Reason codes carried by BluetoothStateChangedEvent when the core itself
 * enters the ERROR state. Adapters may pass their own codes to
 * wlh_coproc_bluetooth_fatal_error(). */
#define WLH_COPROC_BLUETOOTH_REASON_MALFORMED_HCI 1u

/* OTA_STREAM chunking. stream_chunk_size is the largest firmware payload one
 * OTA_STREAM record carries; it fits the max frame after the frame header, the
 * 8-byte raw record header and the 16-byte OTA stream sub-header. alignment is
 * the offset/size granularity the host must honour so the backend can write
 * flash without a read-modify-write. The initial credit bounds how many chunks
 * ride ahead of the flash writer: credit is returned only in write_complete,
 * so the window equals the backend's receive queue depth. */
#define WLH_COPROC_OTA_STREAM_HEADER_SIZE 16u
#define WLH_COPROC_OTA_CHUNK_SIZE 4048u
#define WLH_COPROC_OTA_ALIGNMENT 16u
#define WLH_COPROC_OTA_INITIAL_CREDIT 4u
/* OTA_STREAM record_type for a firmware data chunk. */
#define WLH_COPROC_OTA_RECORD_TYPE 1u

typedef enum wlh_coproc_result {
    WLH_COPROC_OK = 0,
    WLH_COPROC_INVALID_ARGUMENT = -1,
    WLH_COPROC_INVALID_STATE = -2,
    WLH_COPROC_TRANSPORT_ERROR = -3,
    WLH_COPROC_PROTOCOL_ERROR = -4,
    WLH_COPROC_NO_CREDIT = -5,
    WLH_COPROC_NOT_SUPPORTED = -6,
    WLH_COPROC_BACKEND_ERROR = -7
} wlh_coproc_result_t;

typedef enum wlh_coproc_state {
    WLH_COPROC_STATE_STOPPED = 0,
    WLH_COPROC_STATE_WAITING_FOR_HELLO,
    WLH_COPROC_STATE_READY,
    WLH_COPROC_STATE_CONGESTED,
    WLH_COPROC_STATE_FAILED
} wlh_coproc_state_t;

typedef void (*wlh_coproc_tx_complete_fn)(
    void *completion_context, uint8_t *frame, size_t size, int status
);
typedef int (*wlh_coproc_submit_tx_fn)(
    void *context,
    uint8_t *frame,
    size_t size,
    wlh_coproc_tx_complete_fn completion,
    void *completion_context
);
typedef void (*wlh_coproc_ethernet_rx_fn)(
    void *context, const uint8_t *frame, size_t size
);

typedef struct wlh_coproc_port {
    void *context;
    wlh_coproc_submit_tx_fn submit_tx;
    /* Must copy/enqueue the frame and return without running the net stack. */
    wlh_coproc_ethernet_rx_fn ethernet_rx;
    wlh_coproc_ethernet_rx_fn ethernet_ap_rx;
} wlh_coproc_port_t;

typedef uint8_t *(*wlh_coproc_buffer_alloc_fn)(void *context, size_t size);
typedef void (*wlh_coproc_buffer_free_fn)(void *context, uint8_t *buffer);
typedef struct wlh_coproc_buffer_ops {
    void *context;
    wlh_coproc_buffer_alloc_fn alloc;
    wlh_coproc_buffer_free_fn free;
} wlh_coproc_buffer_ops_t;

typedef struct wlh_coproc_wifi_connect {
    uint8_t ssid[32];
    size_t ssid_size;
    uint8_t credential[64];
    size_t credential_size;
    uint32_t security;
} wlh_coproc_wifi_connect_t;

typedef struct wlh_coproc_wifi_ap {
    uint8_t ssid[32];
    size_t ssid_size;
    uint8_t credential[64];
    size_t credential_size;
    uint32_t security;
    uint32_t channel;
    uint32_t max_clients;
} wlh_coproc_wifi_ap_t;

typedef int (*wlh_wifi_initialize_fn)(
    void *context, uint32_t operation_id, uint32_t interface_flags
);
typedef int (*wlh_wifi_scan_fn)(void *context, uint32_t scan_id);
typedef int (*wlh_wifi_connect_fn)(
    void *context, const wlh_coproc_wifi_connect_t *request
);
typedef int (*wlh_wifi_disconnect_fn)(void *context);
typedef int (*wlh_wifi_start_ap_fn)(
    void *context, const wlh_coproc_wifi_ap_t *request
);
typedef int (*wlh_wifi_stop_ap_fn)(void *context);

typedef struct wlh_coproc_wifi_ops {
    void *context;
    /* All operations are nonblocking submissions; results arrive via ingress
     * APIs. */
    wlh_wifi_initialize_fn initialize;
    wlh_wifi_scan_fn scan;
    wlh_wifi_connect_fn connect;
    wlh_wifi_disconnect_fn disconnect;
    wlh_wifi_start_ap_fn start_ap;
    wlh_wifi_stop_ap_fn stop_ap;
} wlh_coproc_wifi_ops_t;

/* Device Information service (optional). Sizes match the nanopb bounds of
 * DeviceInfoResponse in the protocol schema. */
typedef struct wlh_coproc_device_info {
    char vendor[33];
    char mcu_model[33];
    uint8_t uid[32];
    /* 0..32; 0 means the security policy hides the uid. */
    size_t uid_size;
    char board_profile[65];
} wlh_coproc_device_info_t;

/* Runs on the core task; must be nonblocking. Fill `info` and return 0;
 * nonzero rejects the request with a DEVICE_INFO domain error. */
typedef int (*wlh_coproc_get_device_info_fn)(
    void *context, wlh_coproc_device_info_t *info
);

typedef struct wlh_coproc_device_info_ops {
    void *context;
    wlh_coproc_get_device_info_fn get_info;
} wlh_coproc_device_info_ops_t;

/* Shared result codes for the synchronous optional-service backends (IO, ADC,
 * KV). Core maps them onto wire status codes inside the service's status
 * domain; any unrecognised negative value becomes INTERNAL.
 *
 * INVALID_STATE has no published wlh_status_code_t counterpart, so it maps to
 * the closest one, NOT_READY. That is the code a host sees for e.g. writing a
 * pin that is configured as INPUT. */
typedef enum wlh_coproc_service_status {
    WLH_COPROC_SERVICE_OK = 0,
    WLH_COPROC_SERVICE_INVALID_ARGUMENT = -1,
    WLH_COPROC_SERVICE_NOT_FOUND = -2,
    WLH_COPROC_SERVICE_NOT_SUPPORTED = -3,
    WLH_COPROC_SERVICE_INVALID_STATE = -4,
    WLH_COPROC_SERVICE_NO_SPACE = -5,
    WLH_COPROC_SERVICE_INTERNAL = -6
} wlh_coproc_service_status_t;

/* IO service (optional). Values match the IoMode/IoPull wire enums, but the
 * adapter contract stays free of protobuf types. */
typedef enum wlh_coproc_io_mode {
    WLH_COPROC_IO_MODE_INPUT = 1,
    WLH_COPROC_IO_MODE_OUTPUT = 2,
    WLH_COPROC_IO_MODE_OPEN_DRAIN = 3
} wlh_coproc_io_mode_t;

typedef enum wlh_coproc_io_pull {
    WLH_COPROC_IO_PULL_NONE = 1,
    WLH_COPROC_IO_PULL_UP = 2,
    WLH_COPROC_IO_PULL_DOWN = 3
} wlh_coproc_io_pull_t;

typedef struct wlh_coproc_io_config {
    /* Logical profile pin, not a vendor GPIO number. */
    uint32_t pin_id;
    wlh_coproc_io_mode_t mode;
    wlh_coproc_io_pull_t pull;
    /* Latch this before switching an OUTPUT/OPEN_DRAIN pin's direction so the
     * transition does not glitch. Ignored for INPUT. */
    bool initial_level;
} wlh_coproc_io_config_t;

typedef struct wlh_coproc_io_state {
    bool level;
    /* Configuration actually in effect, not the cached request. */
    wlh_coproc_io_mode_t mode;
    wlh_coproc_io_pull_t pull;
} wlh_coproc_io_state_t;

/* All three run on the core task and must be nonblocking. Return 0 or a
 * wlh_coproc_service_status_t. */
typedef int (*wlh_coproc_io_configure_fn)(
    void *context, const wlh_coproc_io_config_t *config
);
typedef int (*wlh_coproc_io_read_fn)(
    void *context, uint32_t pin_id, wlh_coproc_io_state_t *state
);
typedef int (*wlh_coproc_io_write_fn)(
    void *context, uint32_t pin_id, bool level
);

typedef struct wlh_coproc_io_ops {
    void *context;
    wlh_coproc_io_configure_fn configure;
    wlh_coproc_io_read_fn read;
    wlh_coproc_io_write_fn write;
} wlh_coproc_io_ops_t;

/* ADC service (optional). Reports the calibrated pin voltage; resolution,
 * attenuation and raw codes belong to the adapter. */
typedef int (*wlh_coproc_adc_read_fn)(
    void *context, uint32_t pin_id, uint32_t *millivolts
);

typedef struct wlh_coproc_adc_ops {
    void *context;
    wlh_coproc_adc_read_fn read;
} wlh_coproc_adc_ops_t;

/* KV service (optional). Bounds match the nanopb bounds of the Kv messages,
 * excluding the NUL terminator the adapter contract adds. */
#define WLH_COPROC_KV_MAX_KEY_SIZE 64u
#define WLH_COPROC_KV_MAX_VALUE_SIZE 512u

/* key and value are NUL-terminated UTF-8; the sizes exclude the terminator.
 * read must NUL-terminate `value` and set `value_size` to the byte length. */
typedef int (*wlh_coproc_kv_read_fn)(
    void *context,
    const char *key,
    char *value,
    size_t value_capacity,
    size_t *value_size
);
typedef int (*wlh_coproc_kv_write_fn)(
    void *context, const char *key, const char *value, size_t value_size
);
typedef int (*wlh_coproc_kv_erase_fn)(void *context, const char *key);

typedef struct wlh_coproc_kv_ops {
    void *context;
    wlh_coproc_kv_read_fn read;
    wlh_coproc_kv_write_fn write;
    wlh_coproc_kv_erase_fn erase;
} wlh_coproc_kv_ops_t;

/* User Passthrough service (optional, RPC form only). */
typedef struct wlh_coproc_user_message {
    uint32_t endpoint_id;
    uint32_t message_type;
    uint32_t flags;
    /* Valid only for the duration of the on_message call. */
    const uint8_t *payload;
    size_t payload_size;
    /* Envelope request_id of the SEND; correlation key for a later RESULT
     * event via wlh_coproc_user_message_result(). */
    uint32_t request_id;
} wlh_coproc_user_message_t;

/* Runs on the core task; must be nonblocking. Return 0 to accept the message
 * (SEND is acked with OK); nonzero rejects it with a USER domain error. */
typedef int (*wlh_coproc_user_message_fn)(
    void *context, const wlh_coproc_user_message_t *message
);

typedef struct wlh_coproc_user_passthrough_ops {
    void *context;
    wlh_coproc_user_message_fn on_message;
} wlh_coproc_user_passthrough_ops_t;

/* Bluetooth Controller service (optional). All lifecycle operations are
 * nonblocking submissions; results arrive via
 * wlh_coproc_bluetooth_operation_complete() /
 * wlh_coproc_bluetooth_info_result() with the same operation_id. */
typedef int (*wlh_bluetooth_initialize_fn)(
    void *context, uint32_t operation_id, uint32_t feature_flags
);
typedef int (*wlh_bluetooth_enable_fn)(
    void *context, uint32_t operation_id, uint32_t mode_flags
);
typedef int (*wlh_bluetooth_disable_fn)(void *context, uint32_t operation_id);
typedef int (*wlh_bluetooth_deinitialize_fn)(
    void *context, uint32_t operation_id, bool release_memory
);
typedef int (*wlh_bluetooth_get_info_fn)(void *context, uint32_t operation_id);
/* Host->Controller HCI packet. `payload` excludes the H4 type byte and is
 * valid only for the duration of the call; copy or reject without blocking.
 * A nonzero return keeps the packet undelivered (core withholds the credit
 * and stops session HCI), so reject only on fatal conditions. */
typedef int (*wlh_bluetooth_hci_send_fn)(
    void *context, uint8_t h4_type, const uint8_t *payload, size_t payload_size
);
/* Optional. Fired when the usable Controller->Host credit goes from zero to
 * positive. Runs on the core task with internal locks held: only signal the
 * backend task here; never call wlh_coproc_bluetooth_hci_send() inline. */
typedef void (*wlh_bluetooth_hci_tx_ready_fn)(void *context);

typedef struct wlh_coproc_bluetooth_ops {
    void *context;
    wlh_bluetooth_initialize_fn initialize;
    wlh_bluetooth_enable_fn enable;
    wlh_bluetooth_disable_fn disable;
    wlh_bluetooth_deinitialize_fn deinitialize;
    wlh_bluetooth_get_info_fn get_info;
    wlh_bluetooth_hci_send_fn hci_send;
    wlh_bluetooth_hci_tx_ready_fn hci_tx_ready;
} wlh_coproc_bluetooth_ops_t;

/* Controller information reported by the backend after GET_INFO. The core
 * supplies the lifecycle state itself; the backend fills the rest. */
typedef struct wlh_coproc_bluetooth_info {
    uint8_t public_address[6];
    bool has_public_address;
    uint8_t hci_version;
    uint16_t manufacturer_id;
    uint64_t feature_bits;
    uint32_t max_hci_packet;
} wlh_coproc_bluetooth_info_t;

/* OTA firmware update service (optional). BEGIN/FINALIZE/ABORT/ACTIVATE are
 * nonblocking submissions correlated by operation_id; their outcome arrives via
 * the matching wlh_coproc_ota_*_complete() ingress call. WRITE is driven by the
 * OTA_STREAM channel and does not carry an operation_id: the core returns one
 * unit of flow-control credit only once wlh_coproc_ota_write_complete() reports
 * the chunk durably written, which paces the host to the flash write rate.
 * QUERY is answered by the core from session state without a backend call. */
typedef struct wlh_coproc_ota_begin_params {
    uint32_t image_type;
    uint64_t image_size;
    uint8_t sha256[32];
    char target_version[33];
    char slot[17];
    /* Valid only for the duration of the begin call; copy if retained. */
    const uint8_t *signature;
    size_t signature_size;
} wlh_coproc_ota_begin_params_t;

/* transfer_id is assigned by the core and is stable for the whole transfer.
 * `data` in write points into the received frame and is valid only for the
 * duration of the call; the backend must copy or enqueue it and return without
 * blocking. All ops return 0 to accept the submission, nonzero to reject it. */
typedef int (*wlh_coproc_ota_begin_fn)(
    void *context,
    uint32_t operation_id,
    uint32_t transfer_id,
    const wlh_coproc_ota_begin_params_t *params
);
typedef int (*wlh_coproc_ota_write_fn)(
    void *context,
    uint32_t transfer_id,
    uint64_t offset,
    const uint8_t *data,
    size_t size
);
typedef int (*wlh_coproc_ota_finalize_fn)(
    void *context,
    uint32_t operation_id,
    uint32_t transfer_id,
    uint64_t bytes_sent
);
typedef int (*wlh_coproc_ota_abort_fn)(
    void *context, uint32_t operation_id, uint32_t transfer_id
);
typedef int (*wlh_coproc_ota_activate_fn)(
    void *context, uint32_t operation_id, uint32_t transfer_id, bool reboot
);

typedef struct wlh_coproc_ota_ops {
    void *context;
    wlh_coproc_ota_begin_fn begin;
    wlh_coproc_ota_write_fn write;
    wlh_coproc_ota_finalize_fn finalize;
    wlh_coproc_ota_abort_fn abort;
    wlh_coproc_ota_activate_fn activate;
} wlh_coproc_ota_ops_t;

typedef struct wlh_coproc_diagnostics {
    wlh_coproc_state_t state;
    uint32_t session_id;
    uint32_t tx_frames;
    uint32_t rx_frames;
    uint32_t checksum_errors;
    uint32_t sequence_gaps;
    uint32_t rpc_requests;
    uint32_t peer_resets;
    uint32_t hci_malformed;
    uint32_t hci_drops;
    /* Best-effort LE advertising reports shed for lack of ADV-channel
     * credit. Growth under scan flood is expected, not a fault. */
    uint32_t hci_adv_drops;
    uint32_t bluetooth_mismatches;
    uint64_t last_peer_activity_ms;
} wlh_coproc_diagnostics_t;

typedef struct wlh_coproc_config {
    wlh_coproc_port_t port;
    wlh_coproc_wifi_ops_t wifi;
    wlh_coproc_device_info_ops_t device_info;
    wlh_coproc_io_ops_t io;
    wlh_coproc_adc_ops_t adc;
    wlh_coproc_kv_ops_t kv;
    wlh_coproc_user_passthrough_ops_t user_passthrough;
    wlh_coproc_bluetooth_ops_t bluetooth;
    wlh_coproc_ota_ops_t ota;
    wlh_coproc_buffer_ops_t buffers;
    wlh_osal_ops_t osal;
    uint32_t max_frame_size;
    uint32_t heartbeat_interval_ms;
    uint32_t initial_credit;
    uint32_t initial_session_id;
    uint8_t core_queue_depth;
    uint32_t stop_timeout_ms;
    wlh_osal_task_attributes_t core_task;
    /* Reported in HelloResponse.implementation_version; empty falls back to a
     * built-in default. Used by the host to confirm the running firmware after
     * an OTA activation. */
    char implementation_version[33];
} wlh_coproc_config_t;

typedef struct wlh_coproc {
    wlh_coproc_config_t config;
    wlh_coproc_state_t state;
    uint32_t session_id;
    uint32_t next_session_id;
    uint32_t tx_sequence[WLH_COPROC_CHANNEL_COUNT];
    uint32_t rx_sequence[WLH_COPROC_CHANNEL_COUNT];
    uint32_t tx_credit[WLH_COPROC_CHANNEL_COUNT];
    bool rx_sequence_valid[WLH_COPROC_CHANNEL_COUNT];
    uint64_t started_ms;
    uint64_t last_heartbeat_ms;
    wlh_coproc_diagnostics_t diagnostics;
    wlh_osal_task_t core_task;
    wlh_osal_queue_t core_queue;
    wlh_osal_mutex_t state_mutex;
    uintptr_t core_queue_storage[WLH_COPROC_MAX_QUEUE_DEPTH * 2u];
    /* Ethernet RX is lossy by design under congestion. Keep its queued work
     * below the core queue capacity so control RPCs and completion events can
     * always make progress. Protected by state_mutex. */
    uint8_t ethernet_tx_jobs_pending;
    bool worker_started;
    bool worker_stopping;
    uint32_t next_backend_operation_id;
    struct {
        bool active;
        uint32_t operation_id;
        uint32_t session_id;
        uint32_t request_id;
    } wifi_initialize_pending;
    struct {
        bool active;
        uint32_t operation_id;
        uint32_t session_id;
        uint32_t request_id;
        uint16_t method_id;
    } bluetooth_pending;
    /* Wire BluetoothControllerState value tracked by the core state machine. */
    uint32_t bluetooth_state;
    /* Controller->Host HCI jobs queued but not yet sent; reserves credit. */
    uint32_t bluetooth_tx_inflight;
    /* Same reservation for WLH_CHANNEL_BLUETOOTH_HCI_ADV. */
    uint32_t bluetooth_adv_tx_inflight;
    /* Host declared WLH_CHANNEL_BLUETOOTH_HCI_ADV in its Hello; reports fall
     * back to the reliable channel when the peer predates the split. */
    bool bluetooth_adv_channel;
    bool bluetooth_hci_stopped;
    /* OTA transfer state machine (wlh_protocol_v1_OtaState). A single transfer
     * is in flight at a time; ota_pending correlates the outstanding
     * BEGIN/FINALIZE/ABORT/ACTIVATE backend submission with its RPC request. */
    struct {
        bool active;
        uint32_t operation_id;
        uint32_t session_id;
        uint32_t request_id;
        uint16_t method_id;
    } ota_pending;
    uint32_t ota_state;
    uint32_t ota_transfer_id;
    uint32_t next_ota_transfer_id;
    uint64_t ota_image_size;
    /* Bytes handed to the backend; advances as chunks are accepted so the next
     * chunk's offset can be validated while earlier writes are still in flight
     * (up to the OTA_STREAM credit window). */
    uint64_t ota_bytes_accepted;
    /* Bytes the backend reported durably written; drives progress and QUERY. */
    uint64_t ota_bytes_received;
    uint64_t ota_progress_reported_bytes;
    char ota_target_version[33];
} wlh_coproc_t;

typedef struct wlh_coproc_bss {
    const uint8_t *ssid;
    size_t ssid_size;
    uint8_t bssid[6];
    uint8_t interface_mac[6];
    uint32_t security;
    uint32_t channel;
    int32_t rssi_dbm;
} wlh_coproc_bss_t;

wlh_coproc_result_t wlh_coproc_init(
    wlh_coproc_t *coproc, const wlh_coproc_config_t *config
);
wlh_coproc_result_t wlh_coproc_start(wlh_coproc_t *coproc);
wlh_coproc_result_t wlh_coproc_stop(wlh_coproc_t *coproc);
/* Nonblocking ingress. Core copies input and processes it on its OSAL task. */
wlh_coproc_result_t wlh_coproc_on_frame(
    wlh_coproc_t *coproc, const uint8_t *frame, size_t size
);

wlh_coproc_result_t wlh_coproc_wifi_scan_result(
    wlh_coproc_t *coproc, uint32_t scan_id, const wlh_coproc_bss_t *bss
);
wlh_coproc_result_t wlh_coproc_wifi_initialized(
    wlh_coproc_t *coproc, uint32_t operation_id, int backend_status
);
wlh_coproc_result_t wlh_coproc_wifi_scan_completed(
    wlh_coproc_t *coproc,
    uint32_t scan_id,
    uint32_t result_count,
    bool cancelled
);
wlh_coproc_result_t wlh_coproc_wifi_connected(
    wlh_coproc_t *coproc, const wlh_coproc_bss_t *bss
);
wlh_coproc_result_t wlh_coproc_wifi_disconnected(
    wlh_coproc_t *coproc, uint32_t reason, bool locally_initiated
);
wlh_coproc_result_t wlh_coproc_wifi_ap_started(
    wlh_coproc_t *coproc, const wlh_coproc_bss_t *ap
);
wlh_coproc_result_t wlh_coproc_wifi_ap_stopped(
    wlh_coproc_t *coproc, uint32_t reason, bool locally_initiated
);
wlh_coproc_result_t wlh_coproc_wifi_ap_client_joined(
    wlh_coproc_t *coproc,
    const uint8_t mac[6],
    int32_t rssi_dbm,
    uint32_t association_id
);
wlh_coproc_result_t wlh_coproc_wifi_ap_client_left(
    wlh_coproc_t *coproc,
    const uint8_t mac[6],
    uint32_t association_id,
    uint32_t ieee80211_reason
);
wlh_coproc_result_t wlh_coproc_ethernet_sta_send(
    wlh_coproc_t *coproc, const uint8_t *frame, size_t size
);
wlh_coproc_result_t wlh_coproc_ethernet_ap_send(
    wlh_coproc_t *coproc, const uint8_t *frame, size_t size
);
/* Emit a USER_PASSTHROUGH RESULT event. Nonblocking ingress, callable from
 * any task context. correlation_id must be the request_id of the original
 * SEND and must not be reused across sessions. */
wlh_coproc_result_t wlh_coproc_user_message_result(
    wlh_coproc_t *coproc,
    uint32_t endpoint_id,
    uint32_t message_type,
    uint32_t correlation_id,
    int32_t result,
    const uint8_t *payload,
    size_t payload_size
);
/* Completion for INITIALIZE/ENABLE/DISABLE/DEINITIALIZE. Nonblocking ingress,
 * callable from any task context. */
wlh_coproc_result_t wlh_coproc_bluetooth_operation_complete(
    wlh_coproc_t *coproc, uint32_t operation_id, int backend_status
);
/* Completion for GET_INFO. `info` may be NULL only when backend_status is
 * nonzero. */
wlh_coproc_result_t wlh_coproc_bluetooth_info_result(
    wlh_coproc_t *coproc,
    uint32_t operation_id,
    int backend_status,
    const wlh_coproc_bluetooth_info_t *info
);
/* Controller->Host HCI packet without the H4 type byte. Returns NO_CREDIT
 * when the host has not returned enough credit; the backend must keep the
 * packet and retry after the hci_tx_ready callback fires. */
wlh_coproc_result_t wlh_coproc_bluetooth_hci_send(
    wlh_coproc_t *coproc,
    uint8_t h4_type,
    const uint8_t *packet,
    size_t packet_size
);
/* Moves the controller into the ERROR state and emits STATE_CHANGED. */
wlh_coproc_result_t wlh_coproc_bluetooth_fatal_error(
    wlh_coproc_t *coproc, uint32_t reason
);

/* OTA completions. All are nonblocking ingress callable from any task context;
 * transfer_id must match the transfer the core assigned in the begin op.
 * begin/finalize/abort/activate correlate by operation_id and answer the
 * pending RPC. write reports one delivered chunk: on success the core returns a
 * unit of OTA_STREAM credit and advances progress; on failure it fails the
 * transfer. bytes_received is the cumulative durable byte count. */
wlh_coproc_result_t wlh_coproc_ota_begin_complete(
    wlh_coproc_t *coproc, uint32_t operation_id, int backend_status
);
wlh_coproc_result_t wlh_coproc_ota_write_complete(
    wlh_coproc_t *coproc,
    uint32_t transfer_id,
    uint64_t bytes_received,
    int backend_status
);
wlh_coproc_result_t wlh_coproc_ota_finalize_complete(
    wlh_coproc_t *coproc, uint32_t operation_id, int backend_status
);
wlh_coproc_result_t wlh_coproc_ota_abort_complete(
    wlh_coproc_t *coproc, uint32_t operation_id, int backend_status
);
wlh_coproc_result_t wlh_coproc_ota_activate_complete(
    wlh_coproc_t *coproc, uint32_t operation_id, int backend_status
);

void wlh_coproc_get_diagnostics(
    const wlh_coproc_t *coproc, wlh_coproc_diagnostics_t *diagnostics
);

#ifdef WLH_ENABLE_TEST_HOOKS
void wlh_coproc_test_set_credit(
    wlh_coproc_t *coproc, uint8_t channel, uint32_t credit
);
void wlh_coproc_test_reset_channel(wlh_coproc_t *coproc, uint8_t channel);
void wlh_coproc_test_reset_session(wlh_coproc_t *coproc, uint32_t reason);
#endif

#ifdef __cplusplus
}
#endif
#endif
