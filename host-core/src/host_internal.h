#ifndef WLH_HOST_INTERNAL_H
#define WLH_HOST_INTERNAL_H

#include "wlh/host.h"

#include <pb.h>

#define WLH_HOST_PROTOBUF_LIMIT 2048u
#define WLH_HOST_RPC_LIMIT (WLH_RPC_ENVELOPE_SIZE + WLH_HOST_PROTOBUF_LIMIT)

typedef enum wlh_host_job_kind {
    WLH_HOST_JOB_STOP = 1,
    WLH_HOST_JOB_RX_FRAME,
    WLH_HOST_JOB_RPC_REQUEST,
    WLH_HOST_JOB_ETHERNET_TX,
    WLH_HOST_JOB_BLUETOOTH_TX,
    WLH_HOST_JOB_OTA_TX,
    WLH_HOST_JOB_TRANSPORT_LOST,
    WLH_HOST_JOB_TRANSPORT_STARTED,
    WLH_HOST_JOB_TRANSPORT_START_FAILED,
    WLH_HOST_JOB_TRANSPORT_STOPPED,
    WLH_HOST_JOB_TRANSPORT_STOP_FAILED
} wlh_host_job_kind_t;

typedef struct wlh_host_job {
    uint32_t kind;
    void *payload;
} wlh_host_job_t;

typedef struct wlh_host_data_job {
    uint8_t channel;
    size_t size;
    /* Ethernet jobs start with compact storage and grow to the negotiated
     * wire payload bound only when the worker finds an adjacent peer. */
    size_t capacity;
    uint8_t data[];
} wlh_host_data_job_t;

typedef struct wlh_host_rpc_job {
    uint16_t service_id;
    uint16_t method_id;
    uint32_t request_id;
    uint32_t timeout_ms;
    wlh_rpc_completion_fn completion;
    void *completion_context;
    size_t payload_size;
    uint8_t payload[];
} wlh_host_rpc_job_t;

#if defined(__GNUC__) || defined(__clang__)
#define WLH_NOINLINE __attribute__((noinline))
#else
#define WLH_NOINLINE
#endif

wlh_host_result_t wlh_host_internal_encode_pb(
    uint8_t *output,
    size_t capacity,
    size_t *size,
    const pb_msgdesc_t *fields,
    const void *message
);
wlh_host_result_t wlh_host_internal_send_payload_frame(
    wlh_host_t *host,
    uint8_t channel,
    const uint8_t *payload,
    size_t payload_size,
    bool reserved
);
wlh_host_result_t wlh_host_internal_send_payload_frame_units(
    wlh_host_t *host,
    uint8_t channel,
    const uint8_t *payload,
    size_t payload_size,
    bool reserved,
    uint32_t credit_units
);
wlh_host_result_t wlh_host_internal_rpc_message_request(
    wlh_host_t *host,
    uint16_t service_id,
    uint16_t method_id,
    const pb_msgdesc_t *fields,
    const void *message,
    wlh_rpc_completion_fn completion,
    void *context
);
int wlh_host_internal_enqueue_job(
    wlh_host_t *host,
    wlh_host_job_kind_t kind,
    void *payload,
    uint32_t timeout_ms
);
uint64_t wlh_host_internal_now_ms(const wlh_host_t *host);
bool wlh_host_internal_dispatch_event(
    wlh_host_t *host,
    wlh_host_event_kind_t kind,
    uint16_t service_id,
    uint16_t method_id,
    const uint8_t *payload,
    size_t payload_size
);
void wlh_host_internal_dispatch_completion(
    wlh_host_t *host,
    wlh_rpc_completion_fn completion,
    void *context,
    wlh_host_result_t result,
    uint16_t status_domain,
    int16_t status_code,
    const uint8_t *payload,
    size_t payload_size
);
void wlh_host_internal_set_state(wlh_host_t *host, wlh_host_state_t state);
void wlh_host_internal_cancel_pending(
    wlh_host_t *host, wlh_host_result_t result
);
wlh_host_result_t wlh_host_internal_send_rpc(
    wlh_host_t *host,
    const wlh_rpc_envelope_t *envelope,
    const uint8_t *payload,
    size_t payload_size,
    bool reserved
);
wlh_host_result_t wlh_host_internal_send_rpc_message(
    wlh_host_t *host,
    const wlh_rpc_envelope_t *envelope,
    const pb_msgdesc_t *fields,
    const void *message,
    bool reserved
);
/* units must equal the number of credit units the peer spent to deliver the
 * frame being acknowledged. Ethernet channels charge one unit per raw record,
 * so an aggregated frame carrying N records must return N units. */
wlh_host_result_t wlh_host_internal_send_credit_update(
    wlh_host_t *host, uint8_t channel, uint32_t units
);
bool wlh_host_internal_flush_ethernet_rx_credits(wlh_host_t *host);
void wlh_host_internal_reset_ethernet_rx_credits(wlh_host_t *host);
wlh_host_result_t wlh_host_internal_send_hello(wlh_host_t *host);
wlh_pending_rpc_t *wlh_host_internal_find_pending(
    wlh_host_t *host, const wlh_rpc_envelope_t *envelope
);
wlh_host_result_t wlh_host_internal_handle_hello_response(
    wlh_host_t *host, const uint8_t *payload, size_t payload_size
);
void wlh_host_internal_handle_link_event(
    wlh_host_t *host,
    const wlh_rpc_envelope_t *envelope,
    const uint8_t *payload,
    size_t payload_size
);
void wlh_host_internal_request_transport_start(wlh_host_t *host);
void wlh_host_internal_finish_shutdown(wlh_host_t *host);
void wlh_host_internal_process_transport_lost(wlh_host_t *host);
void wlh_host_internal_transport_stop_complete(void *context, int status);
wlh_host_result_t wlh_host_internal_process_deadlines(wlh_host_t *host);
uint32_t wlh_host_internal_next_wait_ms(const wlh_host_t *host);
wlh_host_result_t wlh_host_internal_process_frame(
    wlh_host_t *host, const uint8_t *frame, size_t size
);
wlh_host_result_t wlh_host_internal_process_rpc_request(
    wlh_host_t *host, const wlh_host_rpc_job_t *job
);
wlh_host_result_t wlh_host_internal_process_hci_frame(
    wlh_host_t *host,
    uint8_t channel,
    const uint8_t *payload,
    size_t payload_size
);
wlh_host_result_t wlh_host_internal_process_ota_frame(
    wlh_host_t *host, const uint8_t *payload, size_t payload_size
);

#define now_ms wlh_host_internal_now_ms
#define dispatch_event wlh_host_internal_dispatch_event
#define dispatch_completion wlh_host_internal_dispatch_completion
#define set_state wlh_host_internal_set_state
#define cancel_pending wlh_host_internal_cancel_pending
#define encode_pb wlh_host_internal_encode_pb
#define send_payload_frame wlh_host_internal_send_payload_frame
#define send_payload_frame_units wlh_host_internal_send_payload_frame_units
#define send_rpc wlh_host_internal_send_rpc
#define send_rpc_message wlh_host_internal_send_rpc_message
#define send_credit_update wlh_host_internal_send_credit_update
#define flush_ethernet_rx_credits wlh_host_internal_flush_ethernet_rx_credits
#define reset_ethernet_rx_credits wlh_host_internal_reset_ethernet_rx_credits
#define send_hello wlh_host_internal_send_hello
#define find_pending wlh_host_internal_find_pending
#define handle_hello_response wlh_host_internal_handle_hello_response
#define handle_link_event wlh_host_internal_handle_link_event
#define request_transport_start wlh_host_internal_request_transport_start
#define finish_shutdown wlh_host_internal_finish_shutdown
#define process_transport_lost wlh_host_internal_process_transport_lost
#define transport_stop_complete wlh_host_internal_transport_stop_complete
#define host_process_deadlines wlh_host_internal_process_deadlines
#define host_next_wait_ms wlh_host_internal_next_wait_ms
#define process_frame wlh_host_internal_process_frame
#define process_rpc_request wlh_host_internal_process_rpc_request
#define process_hci_frame wlh_host_internal_process_hci_frame
#define process_ota_frame wlh_host_internal_process_ota_frame
#define rpc_message_request wlh_host_internal_rpc_message_request
#define enqueue_job wlh_host_internal_enqueue_job

#endif
