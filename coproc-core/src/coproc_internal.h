#ifndef WLH_COPROC_INTERNAL_H
#define WLH_COPROC_INTERNAL_H

#include "wlh/coproc.h"

#include "bluetooth.pb.h"
#include "wifi.pb.h"
#include <pb.h>

#define RPC_BUFFER_SIZE 1536u
#define RAW_HEADER_SIZE 8u
#define WLH_COPROC_MAX_SSID_SIZE                                               \
    sizeof(((wlh_protocol_v1_WifiLinkInfo *)0)->ssid.bytes)

#define BT_STATE_UNSPECIFIED                                                             \
    (                                                                                    \
        (                                                                                \
            uint32_t                                                                     \
        )wlh_protocol_v1_BluetoothControllerState_BLUETOOTH_CONTROLLER_STATE_UNSPECIFIED \
    )
#define BT_STATE_DISABLED                                                      \
    ((                                                                         \
        uint32_t                                                               \
    )wlh_protocol_v1_BluetoothControllerState_BLUETOOTH_CONTROLLER_STATE_DISABLED)
#define BT_STATE_ENABLED                                                       \
    ((                                                                         \
        uint32_t                                                               \
    )wlh_protocol_v1_BluetoothControllerState_BLUETOOTH_CONTROLLER_STATE_ENABLED)
#define BT_STATE_ERROR                                                         \
    ((                                                                         \
        uint32_t                                                               \
    )wlh_protocol_v1_BluetoothControllerState_BLUETOOTH_CONTROLLER_STATE_ERROR)

#if defined(__GNUC__) || defined(__clang__)
#define WLH_NOINLINE __attribute__((noinline))
#else
#define WLH_NOINLINE
#endif

typedef enum coproc_job_kind {
    COPROC_JOB_STOP = 1,
    COPROC_JOB_RX_FRAME,
    COPROC_JOB_WIFI_INITIALIZED,
    COPROC_JOB_RPC_EVENT,
    COPROC_JOB_ETHERNET_TX,
    COPROC_JOB_TRANSPORT_FAILED,
    COPROC_JOB_BLUETOOTH_COMPLETE,
    COPROC_JOB_BLUETOOTH_INFO,
    COPROC_JOB_BLUETOOTH_TX,
    COPROC_JOB_BLUETOOTH_FATAL
} coproc_job_kind_t;

typedef struct coproc_job {
    uint32_t kind;
    void *payload;
} coproc_job_t;

typedef struct coproc_data_job {
    uint16_t method_id;
    uint16_t service_id;
    uint8_t channel;
    size_t size;
    uint8_t data[];
} coproc_data_job_t;

typedef struct coproc_wifi_initialized_job {
    uint32_t operation_id;
    int backend_status;
} coproc_wifi_initialized_job_t;

typedef struct coproc_bluetooth_complete_job {
    uint32_t operation_id;
    int backend_status;
} coproc_bluetooth_complete_job_t;

typedef struct coproc_bluetooth_info_job {
    uint32_t operation_id;
    int backend_status;
    wlh_coproc_bluetooth_info_t info;
} coproc_bluetooth_info_job_t;

typedef struct coproc_bluetooth_fatal_job {
    uint32_t reason;
} coproc_bluetooth_fatal_job_t;

uint64_t wlh_coproc_internal_now_ms(const wlh_coproc_t *coproc);
void wlh_coproc_internal_set_state(
    wlh_coproc_t *coproc, wlh_coproc_state_t state
);
int wlh_coproc_internal_enqueue_job(
    wlh_coproc_t *coproc,
    coproc_job_kind_t kind,
    void *payload,
    uint32_t timeout_ms
);
wlh_coproc_result_t wlh_coproc_internal_send_payload(
    wlh_coproc_t *coproc,
    uint8_t channel,
    const uint8_t *payload,
    size_t payload_size
);
wlh_coproc_result_t wlh_coproc_internal_send_rpc(
    wlh_coproc_t *coproc,
    uint16_t service,
    uint16_t method,
    uint32_t request_id,
    uint8_t kind,
    uint16_t status_domain,
    int16_t status_code,
    const uint8_t *message,
    size_t message_size
);
bool wlh_coproc_internal_encode_message(
    uint8_t *output,
    size_t capacity,
    size_t *output_size,
    const pb_msgdesc_t *fields,
    const void *message
);
wlh_coproc_result_t wlh_coproc_internal_send_rpc_message(
    wlh_coproc_t *coproc,
    uint16_t service,
    uint16_t method,
    uint32_t request_id,
    uint8_t kind,
    uint16_t status_domain,
    int16_t status_code,
    const pb_msgdesc_t *fields,
    const void *message
);
wlh_coproc_result_t wlh_coproc_internal_send_credit_update(
    wlh_coproc_t *coproc, uint8_t channel
);
wlh_coproc_result_t wlh_coproc_internal_send_status(
    wlh_coproc_t *coproc, const wlh_rpc_envelope_t *request, int status
);
wlh_coproc_result_t wlh_coproc_internal_send_service_error(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    uint16_t status_domain,
    int backend_status
);
wlh_coproc_result_t wlh_coproc_internal_handle_rpc(
    wlh_coproc_t *coproc,
    const wlh_frame_header_t *frame_header,
    const uint8_t *payload,
    size_t payload_size
);
wlh_coproc_result_t wlh_coproc_internal_process_frame(
    wlh_coproc_t *coproc, const uint8_t *frame, size_t size
);
wlh_coproc_result_t wlh_coproc_internal_emit_due_heartbeat(
    wlh_coproc_t *coproc
);
uint32_t wlh_coproc_internal_next_wait_ms(const wlh_coproc_t *coproc);
bool wlh_coproc_internal_bluetooth_backend_present(const wlh_coproc_t *coproc);
wlh_coproc_result_t wlh_coproc_internal_handle_hello_request(
    wlh_coproc_t *coproc,
    const wlh_frame_header_t *frame_header,
    const wlh_rpc_envelope_t *request,
    const uint8_t *message,
    size_t message_size
);
wlh_coproc_result_t wlh_coproc_internal_handle_wifi(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    const uint8_t *message,
    size_t message_size
);
wlh_coproc_result_t wlh_coproc_internal_handle_bluetooth(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    const uint8_t *message,
    size_t message_size
);
wlh_coproc_result_t wlh_coproc_internal_handle_device_info_request(
    wlh_coproc_t *coproc, const wlh_rpc_envelope_t *request
);
wlh_coproc_result_t wlh_coproc_internal_handle_diagnostics_request(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    const uint8_t *message,
    size_t message_size
);
wlh_coproc_result_t wlh_coproc_internal_handle_user_message_request(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    const uint8_t *message,
    size_t message_size
);
wlh_coproc_result_t wlh_coproc_internal_handle_io_request(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    const uint8_t *message,
    size_t message_size
);
wlh_coproc_result_t wlh_coproc_internal_handle_adc_request(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    const uint8_t *message,
    size_t message_size
);
wlh_coproc_result_t wlh_coproc_internal_handle_kv_request(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    const uint8_t *message,
    size_t message_size
);
wlh_coproc_result_t wlh_coproc_internal_process_hci_frame(
    wlh_coproc_t *coproc, const uint8_t *payload, size_t payload_size
);
void wlh_coproc_internal_bluetooth_operation_completed(
    wlh_coproc_t *coproc, const coproc_bluetooth_complete_job_t *completed
);
void wlh_coproc_internal_bluetooth_info_completed(
    wlh_coproc_t *coproc, const coproc_bluetooth_info_job_t *completed
);
void wlh_coproc_internal_bluetooth_enter_error(
    wlh_coproc_t *coproc, uint32_t reason
);
wlh_coproc_result_t wlh_coproc_internal_send_event_message(
    wlh_coproc_t *coproc,
    uint16_t service,
    uint16_t method,
    const pb_msgdesc_t *fields,
    const void *message
);

#define now_ms wlh_coproc_internal_now_ms
#define set_state wlh_coproc_internal_set_state
#define enqueue_job wlh_coproc_internal_enqueue_job
#define send_payload wlh_coproc_internal_send_payload
#define send_rpc wlh_coproc_internal_send_rpc
#define encode_message wlh_coproc_internal_encode_message
#define send_rpc_message wlh_coproc_internal_send_rpc_message
#define send_credit_update wlh_coproc_internal_send_credit_update
#define send_status wlh_coproc_internal_send_status
#define send_service_error wlh_coproc_internal_send_service_error
#define handle_rpc wlh_coproc_internal_handle_rpc
#define process_frame wlh_coproc_internal_process_frame
#define coproc_emit_due_heartbeat wlh_coproc_internal_emit_due_heartbeat
#define coproc_next_wait_ms wlh_coproc_internal_next_wait_ms
#define bluetooth_backend_present wlh_coproc_internal_bluetooth_backend_present
#define handle_hello_request wlh_coproc_internal_handle_hello_request
#define handle_wifi wlh_coproc_internal_handle_wifi
#define handle_bluetooth wlh_coproc_internal_handle_bluetooth
#define handle_device_info_request                                             \
    wlh_coproc_internal_handle_device_info_request
#define handle_diagnostics_request                                             \
    wlh_coproc_internal_handle_diagnostics_request
#define handle_user_message_request                                            \
    wlh_coproc_internal_handle_user_message_request
#define handle_io_request wlh_coproc_internal_handle_io_request
#define handle_adc_request wlh_coproc_internal_handle_adc_request
#define handle_kv_request wlh_coproc_internal_handle_kv_request
#define process_hci_frame wlh_coproc_internal_process_hci_frame
#define bluetooth_operation_completed                                          \
    wlh_coproc_internal_bluetooth_operation_completed
#define bluetooth_info_completed wlh_coproc_internal_bluetooth_info_completed
#define bluetooth_enter_error wlh_coproc_internal_bluetooth_enter_error
#define send_event_message wlh_coproc_internal_send_event_message

#endif
