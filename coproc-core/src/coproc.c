#include "wlh/coproc.h"
#include "wlh/log.h"

#include <limits.h>
#include <string.h>

#include "adc.pb.h"
#include "bluetooth.pb.h"
#include "device_info.pb.h"
#include "diagnostics.pb.h"
#include "io.pb.h"
#include "kv.pb.h"
#include "link.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "user_passthrough.pb.h"
#include "wifi.pb.h"

#define RPC_BUFFER_SIZE 1536u
#define RAW_HEADER_SIZE 8u

/* Short aliases for the wire BluetoothControllerState values the core state
   machine stores in coproc->bluetooth_state. */
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

/* Bound for caller-supplied SSIDs, taken from the nanopb field so it tracks
   the schema. WifiNetwork and WifiLinkInfo must agree for MAX_SSID_SIZE to be
   a valid bound for both event shapes. */
#define WLH_COPROC_MAX_SSID_SIZE                                               \
    sizeof(((wlh_protocol_v1_WifiLinkInfo *)0)->ssid.bytes)

_Static_assert(
    sizeof(((wlh_protocol_v1_WifiNetwork *)0)->ssid.bytes) ==
        WLH_COPROC_MAX_SSID_SIZE,
    "WifiNetwork and WifiLinkInfo ssid bounds diverged"
);

/* The adapter contract is NUL-terminated strings, so every KV bound is one
   byte smaller than the nanopb field that carries it. */
_Static_assert(
    sizeof(((wlh_protocol_v1_KvWriteRequest *)0)->key) ==
        WLH_COPROC_KV_MAX_KEY_SIZE + 1u,
    "KV key bound diverged from the schema"
);
_Static_assert(
    sizeof(((wlh_protocol_v1_KvReadResponse *)0)->value) ==
        WLH_COPROC_KV_MAX_VALUE_SIZE + 1u,
    "KV value bound diverged from the schema"
);

/* A BSS is only usable if its SSID fits the schema bound and is non-NULL when
   non-empty; every event path copies it into a fixed-size nanopb field. */
static bool bss_ssid_valid(const wlh_coproc_bss_t *bss) {
    return bss->ssid_size <= WLH_COPROC_MAX_SSID_SIZE &&
           (bss->ssid_size == 0u || bss->ssid != NULL);
}

static bool bluetooth_backend_present(const wlh_coproc_t *coproc) {
    return coproc->config.bluetooth.hci_send != NULL;
}

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

_Static_assert(
    sizeof(coproc_job_t) <= sizeof(uintptr_t) * 2u,
    "coprocessor queue slot too small"
);

static const char *coproc_state_name(wlh_coproc_state_t state) {
    switch (state) {
    case WLH_COPROC_STATE_STOPPED:
        return "STOPPED";
    case WLH_COPROC_STATE_WAITING_FOR_HELLO:
        return "WAITING_FOR_HELLO";
    case WLH_COPROC_STATE_READY:
        return "READY";
    case WLH_COPROC_STATE_CONGESTED:
        return "CONGESTED";
    case WLH_COPROC_STATE_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}

static void log_state_transition(
    wlh_coproc_state_t previous, wlh_coproc_state_t state
) {
    const char *previous_name = coproc_state_name(previous);
    const char *state_name = coproc_state_name(state);
    (void)previous_name;
    (void)state_name;
    WLH_LOGI("wlh_coproc", "state %s -> %s", previous_name, state_name);
}

static void set_state(wlh_coproc_t *coproc, wlh_coproc_state_t state) {
    wlh_coproc_state_t previous;
    if (coproc->state == state) {
        return;
    }
    previous = coproc->state;
    coproc->state = state;
    coproc->diagnostics.state = state;
    log_state_transition(previous, state);
}

static uint64_t now_ms(const wlh_coproc_t *coproc) {
    return coproc->config.osal.monotonic_time_ms(coproc->config.osal.context);
}

static int enqueue_job(
    wlh_coproc_t *coproc,
    coproc_job_kind_t kind,
    void *payload,
    uint32_t timeout_ms
) {
    coproc_job_t job = {(uint32_t)kind, payload};
    if (coproc == NULL || !coproc->worker_started)
        return -1;
    return coproc->config.osal.queue_send(
        coproc->config.osal.context, &coproc->core_queue, &job, timeout_ms
    );
}

static void tx_complete(
    void *completion_context, uint8_t *frame, size_t size, int status
) {
    wlh_coproc_t *coproc = completion_context;
    (void)size;
    coproc->config.buffers.free(coproc->config.buffers.context, frame);
    if (status != 0)
        (void)enqueue_job(
            coproc, COPROC_JOB_TRANSPORT_FAILED, NULL, WLH_OSAL_NO_WAIT
        );
}

static wlh_coproc_result_t send_payload(
    wlh_coproc_t *coproc,
    uint8_t channel,
    const uint8_t *payload,
    size_t payload_size
) {
    uint8_t *frame;
    wlh_frame_header_t header;
    size_t frame_size = 0;

    if (payload_size + WLH_FRAME_HEADER_SIZE > coproc->config.max_frame_size) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }
    if (channel != WLH_CHANNEL_LINK_CONTROL &&
        coproc->tx_credit[channel] == 0u) {
        set_state(coproc, WLH_COPROC_STATE_CONGESTED);
        WLH_LOGW("wlh_coproc", "no credit on channel %u", (unsigned)channel);
        return WLH_COPROC_NO_CREDIT;
    }

    frame = coproc->config.buffers.alloc(
        coproc->config.buffers.context, WLH_FRAME_HEADER_SIZE + payload_size
    );
    if (frame == NULL)
        return WLH_COPROC_BACKEND_ERROR;

    wlh_frame_header_init(&header, channel);
    header.session_id = coproc->session_id;
    header.sequence = coproc->tx_sequence[channel]++;
    if (wlh_frame_encode(
            frame,
            WLH_FRAME_HEADER_SIZE + payload_size,
            &frame_size,
            &header,
            payload,
            payload_size
        ) != WLH_WIRE_OK) {
        coproc->config.buffers.free(coproc->config.buffers.context, frame);
        return WLH_COPROC_PROTOCOL_ERROR;
    }
    if (coproc->config.port.submit_tx(
            coproc->config.port.context, frame, frame_size, tx_complete, coproc
        ) != 0) {
        coproc->config.buffers.free(coproc->config.buffers.context, frame);
        return WLH_COPROC_TRANSPORT_ERROR;
    }

    if (channel != WLH_CHANNEL_LINK_CONTROL) {
        --coproc->tx_credit[channel];
    }
    ++coproc->diagnostics.tx_frames;
    return WLH_COPROC_OK;
}

// clang-format off
static WLH_NOINLINE wlh_coproc_result_t send_rpc(
    wlh_coproc_t *coproc,
    uint16_t service_id, uint16_t method_id, uint32_t request_id, uint8_t kind,
    uint16_t status_domain, int16_t status_code,
    const uint8_t *payload, size_t payload_size) {
    // clang-format on
    uint8_t *rpc;
    wlh_rpc_envelope_t envelope;
    wlh_coproc_result_t result;
    size_t rpc_capacity;
    size_t rpc_size = 0;

    if (payload_size > RPC_BUFFER_SIZE - WLH_RPC_ENVELOPE_SIZE)
        return WLH_COPROC_INVALID_ARGUMENT;
    rpc_capacity = WLH_RPC_ENVELOPE_SIZE + payload_size;
    rpc = coproc->config.buffers.alloc(
        coproc->config.buffers.context, rpc_capacity
    );
    if (rpc == NULL)
        return WLH_COPROC_BACKEND_ERROR;

    memset(&envelope, 0, sizeof(envelope));
    envelope.service_id = service_id;
    envelope.method_id = method_id;
    envelope.request_id = request_id;
    envelope.kind = kind;
    envelope.status_domain = status_domain;
    envelope.status_code = status_code;

    if (wlh_rpc_encode(
            rpc, rpc_capacity, &rpc_size, &envelope, payload, payload_size
        ) != WLH_WIRE_OK) {
        coproc->config.buffers.free(coproc->config.buffers.context, rpc);
        return WLH_COPROC_PROTOCOL_ERROR;
    }
    result = send_payload(
        coproc,
        service_id == WLH_SERVICE_LINK ? WLH_CHANNEL_LINK_CONTROL
                                       : WLH_CHANNEL_CONTROL_RPC,
        rpc,
        rpc_size
    );
    coproc->config.buffers.free(coproc->config.buffers.context, rpc);
    return result;
}

static bool encode_message(
    uint8_t *output,
    size_t capacity,
    size_t *size,
    const pb_msgdesc_t *fields,
    const void *message
) {
    pb_ostream_t stream = pb_ostream_from_buffer(output, capacity);
    if (!pb_encode(&stream, fields, message)) {
        return false;
    }
    *size = stream.bytes_written;
    return true;
}

static WLH_NOINLINE wlh_coproc_result_t send_rpc_message(
    wlh_coproc_t *coproc,
    uint16_t service_id,
    uint16_t method_id,
    uint32_t request_id,
    uint8_t kind,
    uint16_t status_domain,
    int16_t status_code,
    const pb_msgdesc_t *fields,
    const void *message
) {
    uint8_t *payload;
    size_t payload_size = 0u;
    size_t encoded_size = 0u;
    wlh_coproc_result_t result;

    if (!pb_get_encoded_size(&payload_size, fields, message) ||
        payload_size > RPC_BUFFER_SIZE - WLH_RPC_ENVELOPE_SIZE) {
        return WLH_COPROC_PROTOCOL_ERROR;
    }
    if (payload_size == 0u) {
        return send_rpc(
            coproc,
            service_id,
            method_id,
            request_id,
            kind,
            status_domain,
            status_code,
            NULL,
            0u
        );
    }
    payload = coproc->config.buffers.alloc(
        coproc->config.buffers.context, payload_size
    );
    if (payload == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    if (!encode_message(
            payload, payload_size, &encoded_size, fields, message
        ) ||
        encoded_size != payload_size) {
        coproc->config.buffers.free(coproc->config.buffers.context, payload);
        return WLH_COPROC_PROTOCOL_ERROR;
    }
    result = send_rpc(
        coproc,
        service_id,
        method_id,
        request_id,
        kind,
        status_domain,
        status_code,
        payload,
        payload_size
    );
    coproc->config.buffers.free(coproc->config.buffers.context, payload);
    return result;
}

static wlh_coproc_result_t send_credit_update(
    wlh_coproc_t *coproc, uint8_t channel
) {
    wlh_protocol_v1_CreditUpdate update =
        wlh_protocol_v1_CreditUpdate_init_zero;

    update.channel_id = channel;
    update.units = 1u;
    return send_rpc_message(
        coproc,
        WLH_SERVICE_LINK,
        WLH_LINK_METHOD_CREDIT_UPDATE,
        0u,
        WLH_RPC_KIND_EVENT,
        0u,
        0,
        wlh_protocol_v1_CreditUpdate_fields,
        &update
    );
}

static WLH_NOINLINE wlh_coproc_result_t
send_hello_response(wlh_coproc_t *coproc, uint32_t request_id) {
    wlh_protocol_v1_HelloResponse *response;
    size_t i;
    uint32_t selected_session;
    wlh_coproc_result_t result;

    response = (wlh_protocol_v1_HelloResponse *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*response)
    );
    if (response == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    memset(response, 0, sizeof(*response));

    selected_session = coproc->next_session_id++;
    if (selected_session == 0u) {
        selected_session = coproc->next_session_id++;
    }

    response->has_selected_protocol = true;
    response->selected_protocol.major = 1u;
    response->session_id = selected_session;
    response->boot_id = selected_session;
    memcpy(
        response->implementation, "wlh-coproc-core", sizeof("wlh-coproc-core")
    );
    memcpy(response->implementation_version, "0.1.0", sizeof("0.1.0"));
    response->max_frame_size = coproc->config.max_frame_size;
    response->alignment = 1u;
    response->checksum_mode = wlh_protocol_v1_ChecksumMode_CHECKSUM_MODE_SUM32;
    response->initial_credits_count = 4u;

    for (i = 0; i < 4u; ++i) {
        response->initial_credits[i].channel_id = (uint32_t)i;
        response->initial_credits[i].units = coproc->config.initial_credit;
        response->initial_credits[i].unit_bytes = 1u;
        coproc->tx_credit[i] = coproc->config.initial_credit;
    }

    coproc->tx_credit[WLH_CHANNEL_BLUETOOTH_HCI] = 0u;
    coproc->tx_credit[WLH_CHANNEL_BLUETOOTH_HCI_ADV] = 0u;
    if (bluetooth_backend_present(coproc)) {
        response->services_count = 1u;
        response->services[0].service_id = WLH_SERVICE_BLUETOOTH;
        response->services[0].major = 1u;
        response->channels_count = 1u;
        response->channels[0].channel_id = WLH_CHANNEL_BLUETOOTH_HCI;
        response->channels[0].max_frame_payload = WLH_COPROC_MAX_HCI_PACKET;
        response->channels[0].alignment = 1u;
        response->initial_credits[4].channel_id = WLH_CHANNEL_BLUETOOTH_HCI;
        response->initial_credits[4].units =
            WLH_COPROC_BLUETOOTH_INITIAL_CREDIT;
        response->initial_credits[4].unit_bytes = 1u;
        response->initial_credits_count = 5u;
        coproc->tx_credit[WLH_CHANNEL_BLUETOOTH_HCI] =
            WLH_COPROC_BLUETOOTH_INITIAL_CREDIT;
        if (coproc->bluetooth_adv_channel) {
            response->channels_count = 2u;
            response->channels[1].channel_id = WLH_CHANNEL_BLUETOOTH_HCI_ADV;
            response->channels[1].max_frame_payload = WLH_COPROC_MAX_HCI_PACKET;
            response->channels[1].alignment = 1u;
            response->initial_credits[5].channel_id =
                WLH_CHANNEL_BLUETOOTH_HCI_ADV;
            response->initial_credits[5].units =
                WLH_COPROC_BLUETOOTH_ADV_INITIAL_CREDIT;
            response->initial_credits[5].unit_bytes = 1u;
            response->initial_credits_count = 6u;
            coproc->tx_credit[WLH_CHANNEL_BLUETOOTH_HCI_ADV] =
                WLH_COPROC_BLUETOOTH_ADV_INITIAL_CREDIT;
        }
    }

    /* Negotiation frames use session 0. The selected session takes effect only
       after the complete HelloResponse has been sent. */
    coproc->session_id = 0u;
    result = send_rpc_message(
        coproc,
        WLH_SERVICE_LINK,
        WLH_LINK_METHOD_HELLO,
        request_id,
        WLH_RPC_KIND_RESPONSE,
        WLH_STATUS_DOMAIN_NONE,
        WLH_STATUS_OK,
        wlh_protocol_v1_HelloResponse_fields,
        response
    );
    coproc->config.buffers.free(
        coproc->config.buffers.context, (uint8_t *)response
    );
    if (result == WLH_COPROC_OK) {
        coproc->session_id = selected_session;
        WLH_LOGI(
            "wlh_coproc",
            "negotiated session %lu",
            (unsigned long)selected_session
        );
        set_state(coproc, WLH_COPROC_STATE_READY);
    }
    return result;
}

static wlh_coproc_result_t send_status(
    wlh_coproc_t *coproc, const wlh_rpc_envelope_t *request, int backend_status
) {
    return send_rpc(
        coproc,
        request->service_id,
        request->method_id,
        request->request_id,
        WLH_RPC_KIND_RESPONSE,
        backend_status == 0 ? WLH_STATUS_DOMAIN_NONE : WLH_STATUS_DOMAIN_WIFI,
        backend_status == 0 ? WLH_STATUS_OK : WLH_STATUS_INTERNAL,
        NULL,
        0u
    );
}

static WLH_NOINLINE wlh_coproc_result_t handle_wifi(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    const uint8_t *payload,
    size_t payload_size
) {
    pb_istream_t stream;
    int status = -1;

    switch (request->method_id) {
    case WLH_WIFI_METHOD_INITIALIZE: {
        uint32_t operation_id;
        WLH_LOGI(
            "wlh_coproc",
            "wifi initialize request %lu",
            (unsigned long)request->request_id
        );
        wlh_protocol_v1_WifiInitializeRequest message =
            wlh_protocol_v1_WifiInitializeRequest_init_zero;
        stream = pb_istream_from_buffer(payload, payload_size);
        if (!pb_decode(
                &stream, wlh_protocol_v1_WifiInitializeRequest_fields, &message
            )) {
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        if (coproc->wifi_initialize_pending.active)
            return send_rpc(
                coproc,
                request->service_id,
                request->method_id,
                request->request_id,
                WLH_RPC_KIND_RESPONSE,
                WLH_STATUS_DOMAIN_WIFI,
                WLH_STATUS_BUSY,
                NULL,
                0u
            );
        operation_id = coproc->next_backend_operation_id++;
        if (operation_id == 0u)
            operation_id = coproc->next_backend_operation_id++;
        coproc->wifi_initialize_pending.active = true;
        coproc->wifi_initialize_pending.operation_id = operation_id;
        coproc->wifi_initialize_pending.session_id = coproc->session_id;
        coproc->wifi_initialize_pending.request_id = request->request_id;
        status = coproc->config.wifi.initialize != NULL
                     ? coproc->config.wifi.initialize(
                           coproc->config.wifi.context,
                           operation_id,
                           message.interface_flags
                       )
                     : -1;
        if (status != 0) {
            memset(
                &coproc->wifi_initialize_pending,
                0,
                sizeof(coproc->wifi_initialize_pending)
            );
            return send_status(coproc, request, status);
        }
        return WLH_COPROC_OK;
    }

    case WLH_WIFI_METHOD_SCAN_START: {
        wlh_protocol_v1_WifiScanRequest *message;
        uint32_t scan_id;
        message =
            (wlh_protocol_v1_WifiScanRequest *)coproc->config.buffers.alloc(
                coproc->config.buffers.context, sizeof(*message)
            );
        if (message == NULL)
            return WLH_COPROC_BACKEND_ERROR;
        memset(message, 0, sizeof(*message));
        stream = pb_istream_from_buffer(payload, payload_size);
        if (!pb_decode(
                &stream, wlh_protocol_v1_WifiScanRequest_fields, message
            )) {
            coproc->config.buffers.free(
                coproc->config.buffers.context, (uint8_t *)message
            );
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        scan_id = message->scan_id;
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)message
        );
        WLH_LOGI(
            "wlh_coproc",
            "wifi scan start request %lu scan_id=%lu",
            (unsigned long)request->request_id,
            (unsigned long)scan_id
        );
        status =
            coproc->config.wifi.scan != NULL
                ? coproc->config.wifi.scan(coproc->config.wifi.context, scan_id)
                : -1;
        break;
    }

    case WLH_WIFI_METHOD_CONNECT: {
        wlh_protocol_v1_WifiConnectRequest *message;
        wlh_coproc_wifi_connect_t *connect;
        message =
            (wlh_protocol_v1_WifiConnectRequest *)coproc->config.buffers.alloc(
                coproc->config.buffers.context, sizeof(*message)
            );
        if (message == NULL)
            return WLH_COPROC_BACKEND_ERROR;
        memset(message, 0, sizeof(*message));
        stream = pb_istream_from_buffer(payload, payload_size);
        if (!pb_decode(
                &stream, wlh_protocol_v1_WifiConnectRequest_fields, message
            )) {
            coproc->config.buffers.free(
                coproc->config.buffers.context, (uint8_t *)message
            );
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        connect = (wlh_coproc_wifi_connect_t *)coproc->config.buffers.alloc(
            coproc->config.buffers.context, sizeof(*connect)
        );
        if (connect == NULL) {
            coproc->config.buffers.free(
                coproc->config.buffers.context, (uint8_t *)message
            );
            return WLH_COPROC_BACKEND_ERROR;
        }
        memset(connect, 0, sizeof(*connect));
        connect->ssid_size = message->ssid.size;
        connect->credential_size = message->credential.size;
        memcpy(connect->ssid, message->ssid.bytes, connect->ssid_size);
        memcpy(
            connect->credential,
            message->credential.bytes,
            connect->credential_size
        );
        connect->security = (uint32_t)message->security;
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)message
        );
        WLH_LOGI(
            "wlh_coproc",
            "wifi connect request %lu ssid_size=%zu security=%lu",
            (unsigned long)request->request_id,
            connect->ssid_size,
            (unsigned long)connect->security
        );
        status = coproc->config.wifi.connect != NULL
                     ? coproc->config.wifi.connect(
                           coproc->config.wifi.context, connect
                       )
                     : -1;
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)connect
        );
        break;
    }

    case WLH_WIFI_METHOD_DISCONNECT:
        WLH_LOGI(
            "wlh_coproc",
            "wifi disconnect request %lu",
            (unsigned long)request->request_id
        );
        status =
            coproc->config.wifi.disconnect != NULL
                ? coproc->config.wifi.disconnect(coproc->config.wifi.context)
                : -1;
        break;

    case WLH_WIFI_METHOD_START_AP: {
        wlh_protocol_v1_WifiStartApRequest *message;
        wlh_coproc_wifi_ap_t *ap;
        message =
            (wlh_protocol_v1_WifiStartApRequest *)coproc->config.buffers.alloc(
                coproc->config.buffers.context, sizeof(*message)
            );
        if (message == NULL)
            return WLH_COPROC_BACKEND_ERROR;
        memset(message, 0, sizeof(*message));
        stream = pb_istream_from_buffer(payload, payload_size);
        if (!pb_decode(
                &stream, wlh_protocol_v1_WifiStartApRequest_fields, message
            )) {
            coproc->config.buffers.free(
                coproc->config.buffers.context, (uint8_t *)message
            );
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        ap = (wlh_coproc_wifi_ap_t *)coproc->config.buffers.alloc(
            coproc->config.buffers.context, sizeof(*ap)
        );
        if (ap == NULL) {
            coproc->config.buffers.free(
                coproc->config.buffers.context, (uint8_t *)message
            );
            return WLH_COPROC_BACKEND_ERROR;
        }
        memset(ap, 0, sizeof(*ap));
        ap->ssid_size = message->ssid.size;
        ap->credential_size = message->credential.size;
        memcpy(ap->ssid, message->ssid.bytes, ap->ssid_size);
        memcpy(ap->credential, message->credential.bytes, ap->credential_size);
        ap->security = (uint32_t)message->security;
        ap->channel = message->channel;
        ap->max_clients = message->max_clients;
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)message
        );
        WLH_LOGI(
            "wlh_coproc",
            "wifi start_ap request %lu channel=%lu max_clients=%lu",
            (unsigned long)request->request_id,
            (unsigned long)ap->channel,
            (unsigned long)ap->max_clients
        );
        status =
            coproc->config.wifi.start_ap != NULL
                ? coproc->config.wifi.start_ap(coproc->config.wifi.context, ap)
                : -1;
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)ap
        );
        break;
    }

    case WLH_WIFI_METHOD_STOP_AP:
        WLH_LOGI(
            "wlh_coproc",
            "wifi stop_ap request %lu",
            (unsigned long)request->request_id
        );
        status = coproc->config.wifi.stop_ap != NULL
                     ? coproc->config.wifi.stop_ap(coproc->config.wifi.context)
                     : -1;
        break;

    default:
        return send_rpc(
            coproc,
            request->service_id,
            request->method_id,
            request->request_id,
            WLH_RPC_KIND_RESPONSE,
            WLH_STATUS_DOMAIN_PROTOCOL,
            WLH_STATUS_NOT_SUPPORTED,
            NULL,
            0u
        );
    }

    return send_status(coproc, request, status);
}

static WLH_NOINLINE wlh_coproc_result_t handle_hello_request(
    wlh_coproc_t *coproc,
    const wlh_frame_header_t *frame_header,
    const wlh_rpc_envelope_t *request,
    const uint8_t *message,
    size_t message_size
) {
    wlh_protocol_v1_HelloRequest *hello;
    pb_istream_t stream;
    size_t i;
    bool supports_v1 = false;
    wlh_coproc_result_t result;

    hello = (wlh_protocol_v1_HelloRequest *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*hello)
    );
    if (hello == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    memset(hello, 0, sizeof(*hello));
    stream = pb_istream_from_buffer(message, message_size);
    if (frame_header->session_id != 0u ||
        !pb_decode(&stream, wlh_protocol_v1_HelloRequest_fields, hello)) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)hello
        );
        return WLH_COPROC_PROTOCOL_ERROR;
    }
    for (i = 0; i < hello->protocol_versions_count; ++i) {
        if (hello->protocol_versions[i].major == 1u)
            supports_v1 = true;
    }
    coproc->bluetooth_adv_channel = false;
    for (i = 0; i < hello->channels_count; ++i) {
        if (hello->channels[i].channel_id == WLH_CHANNEL_BLUETOOTH_HCI_ADV)
            coproc->bluetooth_adv_channel = true;
    }
    if (!supports_v1 || hello->max_frame_size < WLH_FRAME_HEADER_SIZE) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)hello
        );
        return send_rpc(
            coproc,
            WLH_SERVICE_LINK,
            WLH_LINK_METHOD_HELLO,
            request->request_id,
            WLH_RPC_KIND_RESPONSE,
            WLH_STATUS_DOMAIN_PROTOCOL,
            WLH_STATUS_VERSION_MISMATCH,
            NULL,
            0u
        );
    }
    coproc->config.buffers.free(
        coproc->config.buffers.context, (uint8_t *)hello
    );
    memset(coproc->tx_sequence, 0, sizeof(coproc->tx_sequence));
    memset(coproc->rx_sequence_valid, 0, sizeof(coproc->rx_sequence_valid));
    memset(
        &coproc->wifi_initialize_pending,
        0,
        sizeof(coproc->wifi_initialize_pending)
    );
    memset(&coproc->bluetooth_pending, 0, sizeof(coproc->bluetooth_pending));
    coproc->bluetooth_state = BT_STATE_UNSPECIFIED;
    coproc->bluetooth_tx_inflight = 0u;
    coproc->bluetooth_adv_tx_inflight = 0u;
    coproc->bluetooth_hci_stopped = false;
    result = send_hello_response(coproc, request->request_id);
    return result;
}

static WLH_NOINLINE wlh_coproc_result_t handle_device_info_request(
    wlh_coproc_t *coproc, const wlh_rpc_envelope_t *request
) {
    wlh_protocol_v1_DeviceInfoResponse *response;
    wlh_coproc_device_info_t info;
    wlh_coproc_result_t result;
    int status;

    memset(&info, 0, sizeof(info));
    status = coproc->config.device_info.get_info(
        coproc->config.device_info.context, &info
    );
    if (status != 0) {
        return send_rpc(
            coproc,
            request->service_id,
            request->method_id,
            request->request_id,
            WLH_RPC_KIND_RESPONSE,
            WLH_STATUS_DOMAIN_DEVICE_INFO,
            WLH_STATUS_INTERNAL,
            NULL,
            0u
        );
    }
    response =
        (wlh_protocol_v1_DeviceInfoResponse *)coproc->config.buffers.alloc(
            coproc->config.buffers.context, sizeof(*response)
        );
    if (response == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    memset(response, 0, sizeof(*response));

    /* The adapter contract is C strings; enforce termination and bounds
     * before handing the fields to nanopb. */
    info.vendor[sizeof(info.vendor) - 1u] = '\0';
    info.mcu_model[sizeof(info.mcu_model) - 1u] = '\0';
    info.board_profile[sizeof(info.board_profile) - 1u] = '\0';
    if (info.uid_size > sizeof(info.uid))
        info.uid_size = sizeof(info.uid);

    memcpy(response->vendor, info.vendor, sizeof(response->vendor) - 1u);
    memcpy(
        response->mcu_model, info.mcu_model, sizeof(response->mcu_model) - 1u
    );
    memcpy(
        response->board_profile,
        info.board_profile,
        sizeof(response->board_profile) - 1u
    );
    response->uid.size = info.uid_size;
    memcpy(response->uid.bytes, info.uid, info.uid_size);
    result = send_rpc_message(
        coproc,
        request->service_id,
        request->method_id,
        request->request_id,
        WLH_RPC_KIND_RESPONSE,
        WLH_STATUS_DOMAIN_NONE,
        WLH_STATUS_OK,
        wlh_protocol_v1_DeviceInfoResponse_fields,
        response
    );
    coproc->config.buffers.free(
        coproc->config.buffers.context, (uint8_t *)response
    );
    return result;
}

static WLH_NOINLINE wlh_coproc_result_t handle_user_message_request(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    const uint8_t *message,
    size_t message_size
) {
    wlh_protocol_v1_UserMessageSendRequest *send_request;
    wlh_coproc_user_message_t user_message;
    pb_istream_t stream;
    int status;

    send_request =
        (wlh_protocol_v1_UserMessageSendRequest *)coproc->config.buffers.alloc(
            coproc->config.buffers.context, sizeof(*send_request)
        );
    if (send_request == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    memset(send_request, 0, sizeof(*send_request));
    stream = pb_istream_from_buffer(message, message_size);
    if (!pb_decode(
            &stream, wlh_protocol_v1_UserMessageSendRequest_fields, send_request
        )) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)send_request
        );
        return WLH_COPROC_PROTOCOL_ERROR;
    }
    memset(&user_message, 0, sizeof(user_message));
    user_message.endpoint_id = send_request->endpoint_id;
    user_message.message_type = send_request->message_type;
    user_message.flags = send_request->flags;
    user_message.payload = send_request->payload.bytes;
    user_message.payload_size = send_request->payload.size;
    user_message.request_id = request->request_id;
    status = coproc->config.user_passthrough.on_message(
        coproc->config.user_passthrough.context, &user_message
    );
    coproc->config.buffers.free(
        coproc->config.buffers.context, (uint8_t *)send_request
    );
    return send_rpc(
        coproc,
        request->service_id,
        request->method_id,
        request->request_id,
        WLH_RPC_KIND_RESPONSE,
        status == 0 ? WLH_STATUS_DOMAIN_NONE : WLH_STATUS_DOMAIN_USER,
        status == 0 ? WLH_STATUS_OK : WLH_STATUS_INTERNAL,
        NULL,
        0u
    );
}

static wlh_coproc_result_t send_bluetooth_status(
    wlh_coproc_t *coproc,
    uint16_t method_id,
    uint32_t request_id,
    int16_t status_code
) {
    return send_rpc(
        coproc,
        WLH_SERVICE_BLUETOOTH,
        method_id,
        request_id,
        WLH_RPC_KIND_RESPONSE,
        status_code == WLH_STATUS_OK ? WLH_STATUS_DOMAIN_NONE
                                     : WLH_STATUS_DOMAIN_BLUETOOTH,
        status_code,
        NULL,
        0u
    );
}

/* Latch the ERROR state, stop session HCI in both directions and tell the
   host why via STATE_CHANGED. Recovery runs through DISABLE/DEINITIALIZE. */
static void bluetooth_enter_error(wlh_coproc_t *coproc, uint32_t reason) {
    wlh_protocol_v1_BluetoothStateChangedEvent event =
        wlh_protocol_v1_BluetoothStateChangedEvent_init_zero;

    coproc->bluetooth_state = BT_STATE_ERROR;
    coproc->bluetooth_hci_stopped = true;
    event.state = (wlh_protocol_v1_BluetoothControllerState)BT_STATE_ERROR;
    event.reason = reason;
    (void)send_rpc_message(
        coproc,
        WLH_SERVICE_BLUETOOTH,
        WLH_BLUETOOTH_EVENT_STATE_CHANGED,
        0u,
        WLH_RPC_KIND_EVENT,
        WLH_STATUS_DOMAIN_NONE,
        WLH_STATUS_OK,
        wlh_protocol_v1_BluetoothStateChangedEvent_fields,
        &event
    );
}

static uint32_t bluetooth_begin_operation(
    wlh_coproc_t *coproc, const wlh_rpc_envelope_t *request
) {
    uint32_t operation_id = coproc->next_backend_operation_id++;
    if (operation_id == 0u)
        operation_id = coproc->next_backend_operation_id++;
    coproc->bluetooth_pending.active = true;
    coproc->bluetooth_pending.operation_id = operation_id;
    coproc->bluetooth_pending.session_id = coproc->session_id;
    coproc->bluetooth_pending.request_id = request->request_id;
    coproc->bluetooth_pending.method_id = request->method_id;
    return operation_id;
}

static wlh_coproc_result_t bluetooth_submit_result(
    wlh_coproc_t *coproc, const wlh_rpc_envelope_t *request, int status
) {
    if (status != 0) {
        memset(
            &coproc->bluetooth_pending, 0, sizeof(coproc->bluetooth_pending)
        );
        return send_bluetooth_status(
            coproc, request->method_id, request->request_id, WLH_STATUS_INTERNAL
        );
    }
    return WLH_COPROC_OK;
}

static WLH_NOINLINE wlh_coproc_result_t handle_bluetooth(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    const uint8_t *payload,
    size_t payload_size
) {
    pb_istream_t stream;
    uint32_t operation_id;
    int status;

    switch (request->method_id) {
    case WLH_BLUETOOTH_METHOD_INITIALIZE: {
        wlh_protocol_v1_BluetoothInitializeRequest message =
            wlh_protocol_v1_BluetoothInitializeRequest_init_zero;
        stream = pb_istream_from_buffer(payload, payload_size);
        if (!pb_decode(
                &stream,
                wlh_protocol_v1_BluetoothInitializeRequest_fields,
                &message
            )) {
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        if (message.transport !=
            wlh_protocol_v1_BluetoothTransport_BLUETOOTH_TRANSPORT_HCI) {
            return send_bluetooth_status(
                coproc,
                request->method_id,
                request->request_id,
                WLH_STATUS_NOT_SUPPORTED
            );
        }
        if (coproc->bluetooth_pending.active) {
            return send_bluetooth_status(
                coproc, request->method_id, request->request_id, WLH_STATUS_BUSY
            );
        }
        if (coproc->bluetooth_state != BT_STATE_UNSPECIFIED) {
            return send_bluetooth_status(
                coproc, request->method_id, request->request_id, WLH_STATUS_OK
            );
        }
        operation_id = bluetooth_begin_operation(coproc, request);
        status = coproc->config.bluetooth.initialize(
            coproc->config.bluetooth.context,
            operation_id,
            message.feature_flags
        );
        return bluetooth_submit_result(coproc, request, status);
    }

    case WLH_BLUETOOTH_METHOD_ENABLE: {
        wlh_protocol_v1_BluetoothEnableRequest message =
            wlh_protocol_v1_BluetoothEnableRequest_init_zero;
        stream = pb_istream_from_buffer(payload, payload_size);
        if (!pb_decode(
                &stream, wlh_protocol_v1_BluetoothEnableRequest_fields, &message
            )) {
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        if (coproc->bluetooth_pending.active) {
            return send_bluetooth_status(
                coproc, request->method_id, request->request_id, WLH_STATUS_BUSY
            );
        }
        if (coproc->bluetooth_state == BT_STATE_UNSPECIFIED ||
            coproc->bluetooth_state == BT_STATE_ERROR) {
            return send_bluetooth_status(
                coproc,
                request->method_id,
                request->request_id,
                WLH_STATUS_NOT_READY
            );
        }
        if (coproc->bluetooth_state == BT_STATE_ENABLED) {
            return send_bluetooth_status(
                coproc, request->method_id, request->request_id, WLH_STATUS_OK
            );
        }
        operation_id = bluetooth_begin_operation(coproc, request);
        status = coproc->config.bluetooth.enable(
            coproc->config.bluetooth.context, operation_id, message.mode_flags
        );
        return bluetooth_submit_result(coproc, request, status);
    }

    case WLH_BLUETOOTH_METHOD_DISABLE: {
        if (coproc->bluetooth_pending.active) {
            return send_bluetooth_status(
                coproc, request->method_id, request->request_id, WLH_STATUS_BUSY
            );
        }
        if (coproc->bluetooth_state == BT_STATE_UNSPECIFIED ||
            coproc->bluetooth_state == BT_STATE_DISABLED) {
            return send_bluetooth_status(
                coproc, request->method_id, request->request_id, WLH_STATUS_OK
            );
        }
        operation_id = bluetooth_begin_operation(coproc, request);
        status = coproc->config.bluetooth.disable(
            coproc->config.bluetooth.context, operation_id
        );
        return bluetooth_submit_result(coproc, request, status);
    }

    case WLH_BLUETOOTH_METHOD_DEINITIALIZE: {
        wlh_protocol_v1_BluetoothDeinitializeRequest message =
            wlh_protocol_v1_BluetoothDeinitializeRequest_init_zero;
        stream = pb_istream_from_buffer(payload, payload_size);
        if (!pb_decode(
                &stream,
                wlh_protocol_v1_BluetoothDeinitializeRequest_fields,
                &message
            )) {
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        if (coproc->bluetooth_pending.active) {
            return send_bluetooth_status(
                coproc, request->method_id, request->request_id, WLH_STATUS_BUSY
            );
        }
        if (coproc->bluetooth_state == BT_STATE_UNSPECIFIED) {
            return send_bluetooth_status(
                coproc, request->method_id, request->request_id, WLH_STATUS_OK
            );
        }
        operation_id = bluetooth_begin_operation(coproc, request);
        status = coproc->config.bluetooth.deinitialize(
            coproc->config.bluetooth.context,
            operation_id,
            message.release_memory
        );
        return bluetooth_submit_result(coproc, request, status);
    }

    case WLH_BLUETOOTH_METHOD_GET_INFO: {
        if (coproc->bluetooth_pending.active) {
            return send_bluetooth_status(
                coproc, request->method_id, request->request_id, WLH_STATUS_BUSY
            );
        }
        operation_id = bluetooth_begin_operation(coproc, request);
        status = coproc->config.bluetooth.get_info(
            coproc->config.bluetooth.context, operation_id
        );
        return bluetooth_submit_result(coproc, request, status);
    }

    default:
        return send_rpc(
            coproc,
            request->service_id,
            request->method_id,
            request->request_id,
            WLH_RPC_KIND_RESPONSE,
            WLH_STATUS_DOMAIN_PROTOCOL,
            WLH_STATUS_NOT_SUPPORTED,
            NULL,
            0u
        );
    }
}

/* Map a wlh_coproc_service_status_t onto the published wire status codes. The
   status enum has no INVALID_STATE, so NOT_READY carries that meaning. */
static int16_t service_status_to_wire(int status) {
    switch (status) {
    case WLH_COPROC_SERVICE_OK:
        return WLH_STATUS_OK;
    case WLH_COPROC_SERVICE_INVALID_ARGUMENT:
        return WLH_STATUS_INVALID_ARGUMENT;
    case WLH_COPROC_SERVICE_NOT_FOUND:
        return WLH_STATUS_NOT_FOUND;
    case WLH_COPROC_SERVICE_NOT_SUPPORTED:
        return WLH_STATUS_NOT_SUPPORTED;
    case WLH_COPROC_SERVICE_INVALID_STATE:
        return WLH_STATUS_NOT_READY;
    case WLH_COPROC_SERVICE_NO_SPACE:
        return WLH_STATUS_RESOURCE_EXHAUSTED;
    default:
        return WLH_STATUS_INTERNAL;
    }
}

static wlh_coproc_result_t send_service_error(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    uint16_t status_domain,
    int status
) {
    return send_rpc(
        coproc,
        request->service_id,
        request->method_id,
        request->request_id,
        WLH_RPC_KIND_RESPONSE,
        status_domain,
        service_status_to_wire(status),
        NULL,
        0u
    );
}

static bool io_mode_valid(uint32_t mode) {
    return mode == WLH_COPROC_IO_MODE_INPUT ||
           mode == WLH_COPROC_IO_MODE_OUTPUT ||
           mode == WLH_COPROC_IO_MODE_OPEN_DRAIN;
}

static bool io_pull_valid(uint32_t pull) {
    return pull == WLH_COPROC_IO_PULL_NONE || pull == WLH_COPROC_IO_PULL_UP ||
           pull == WLH_COPROC_IO_PULL_DOWN;
}

/* KV keys and values are UTF-8 on the wire; v1 rejects anything else rather
   than persisting bytes a peer cannot decode. Rejects overlong encodings,
   surrogates and anything past U+10FFFF. */
static bool utf8_valid(const char *text, size_t size) {
    size_t i = 0u;

    while (i < size) {
        uint8_t lead = (uint8_t)text[i];
        size_t extra;
        uint32_t code_point;
        size_t j;

        if (lead < 0x80u) {
            ++i;
            continue;
        }
        if (lead >= 0xc2u && lead <= 0xdfu) {
            extra = 1u;
            code_point = lead & 0x1fu;
        } else if (lead >= 0xe0u && lead <= 0xefu) {
            extra = 2u;
            code_point = lead & 0x0fu;
        } else if (lead >= 0xf0u && lead <= 0xf4u) {
            extra = 3u;
            code_point = lead & 0x07u;
        } else {
            return false;
        }
        if (size - i <= extra)
            return false;
        for (j = 1u; j <= extra; ++j) {
            uint8_t continuation = (uint8_t)text[i + j];
            if ((continuation & 0xc0u) != 0x80u)
                return false;
            code_point = (code_point << 6) | (continuation & 0x3fu);
        }
        if (extra == 2u && code_point < 0x800u)
            return false;
        if (extra == 3u && code_point < 0x10000u)
            return false;
        if (code_point > 0x10ffffu)
            return false;
        if (code_point >= 0xd800u && code_point <= 0xdfffu)
            return false;
        i += extra + 1u;
    }
    return true;
}

static WLH_NOINLINE wlh_coproc_result_t handle_io_request(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    const uint8_t *message,
    size_t message_size
) {
    pb_istream_t stream = pb_istream_from_buffer(message, message_size);
    int status;

    switch (request->method_id) {
    case WLH_IO_METHOD_CONFIGURE: {
        wlh_protocol_v1_IoConfigureRequest configure =
            wlh_protocol_v1_IoConfigureRequest_init_zero;
        wlh_coproc_io_config_t config;

        if (!pb_decode(
                &stream, wlh_protocol_v1_IoConfigureRequest_fields, &configure
            ))
            return WLH_COPROC_PROTOCOL_ERROR;
        if (!io_mode_valid((uint32_t)configure.mode) ||
            !io_pull_valid((uint32_t)configure.pull))
            return send_service_error(
                coproc,
                request,
                WLH_STATUS_DOMAIN_PERIPHERAL,
                WLH_COPROC_SERVICE_INVALID_ARGUMENT
            );
        config.pin_id = configure.pin_id;
        config.mode = (wlh_coproc_io_mode_t)configure.mode;
        config.pull = (wlh_coproc_io_pull_t)configure.pull;
        config.initial_level = configure.initial_level;
        status =
            coproc->config.io.configure(coproc->config.io.context, &config);
        return status == WLH_COPROC_SERVICE_OK
                   ? send_rpc(
                         coproc,
                         request->service_id,
                         request->method_id,
                         request->request_id,
                         WLH_RPC_KIND_RESPONSE,
                         WLH_STATUS_DOMAIN_NONE,
                         WLH_STATUS_OK,
                         NULL,
                         0u
                     )
                   : send_service_error(
                         coproc, request, WLH_STATUS_DOMAIN_PERIPHERAL, status
                     );
    }
    case WLH_IO_METHOD_READ: {
        wlh_protocol_v1_IoReadRequest read =
            wlh_protocol_v1_IoReadRequest_init_zero;
        wlh_protocol_v1_IoReadResponse response =
            wlh_protocol_v1_IoReadResponse_init_zero;
        wlh_coproc_io_state_t state;

        if (!pb_decode(&stream, wlh_protocol_v1_IoReadRequest_fields, &read))
            return WLH_COPROC_PROTOCOL_ERROR;
        if (coproc->config.io.read == NULL)
            return send_service_error(
                coproc,
                request,
                WLH_STATUS_DOMAIN_PERIPHERAL,
                WLH_COPROC_SERVICE_NOT_SUPPORTED
            );
        memset(&state, 0, sizeof(state));
        status = coproc->config.io.read(
            coproc->config.io.context, read.pin_id, &state
        );
        if (status != WLH_COPROC_SERVICE_OK)
            return send_service_error(
                coproc, request, WLH_STATUS_DOMAIN_PERIPHERAL, status
            );
        /* The response reports the configuration in effect; an adapter that
           cannot name it has no valid answer to give. */
        if (!io_mode_valid((uint32_t)state.mode) ||
            !io_pull_valid((uint32_t)state.pull))
            return send_service_error(
                coproc,
                request,
                WLH_STATUS_DOMAIN_PERIPHERAL,
                WLH_COPROC_SERVICE_INTERNAL
            );
        response.pin_id = read.pin_id;
        response.level = state.level;
        response.mode = (wlh_protocol_v1_IoMode)state.mode;
        response.pull = (wlh_protocol_v1_IoPull)state.pull;
        return send_rpc_message(
            coproc,
            request->service_id,
            request->method_id,
            request->request_id,
            WLH_RPC_KIND_RESPONSE,
            WLH_STATUS_DOMAIN_NONE,
            WLH_STATUS_OK,
            wlh_protocol_v1_IoReadResponse_fields,
            &response
        );
    }
    case WLH_IO_METHOD_WRITE: {
        wlh_protocol_v1_IoWriteRequest write =
            wlh_protocol_v1_IoWriteRequest_init_zero;

        if (!pb_decode(&stream, wlh_protocol_v1_IoWriteRequest_fields, &write))
            return WLH_COPROC_PROTOCOL_ERROR;
        if (coproc->config.io.write == NULL)
            return send_service_error(
                coproc,
                request,
                WLH_STATUS_DOMAIN_PERIPHERAL,
                WLH_COPROC_SERVICE_NOT_SUPPORTED
            );
        status = coproc->config.io.write(
            coproc->config.io.context, write.pin_id, write.level
        );
        return status == WLH_COPROC_SERVICE_OK
                   ? send_rpc(
                         coproc,
                         request->service_id,
                         request->method_id,
                         request->request_id,
                         WLH_RPC_KIND_RESPONSE,
                         WLH_STATUS_DOMAIN_NONE,
                         WLH_STATUS_OK,
                         NULL,
                         0u
                     )
                   : send_service_error(
                         coproc, request, WLH_STATUS_DOMAIN_PERIPHERAL, status
                     );
    }
    default:
        break;
    }
    return send_rpc(
        coproc,
        request->service_id,
        request->method_id,
        request->request_id,
        WLH_RPC_KIND_RESPONSE,
        WLH_STATUS_DOMAIN_PROTOCOL,
        WLH_STATUS_NOT_SUPPORTED,
        NULL,
        0u
    );
}

static WLH_NOINLINE wlh_coproc_result_t handle_adc_request(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    const uint8_t *message,
    size_t message_size
) {
    wlh_protocol_v1_AdcReadRequest read =
        wlh_protocol_v1_AdcReadRequest_init_zero;
    wlh_protocol_v1_AdcReadResponse response =
        wlh_protocol_v1_AdcReadResponse_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(message, message_size);
    uint32_t millivolts = 0u;
    int status;

    if (request->method_id != WLH_ADC_METHOD_READ) {
        return send_rpc(
            coproc,
            request->service_id,
            request->method_id,
            request->request_id,
            WLH_RPC_KIND_RESPONSE,
            WLH_STATUS_DOMAIN_PROTOCOL,
            WLH_STATUS_NOT_SUPPORTED,
            NULL,
            0u
        );
    }
    if (!pb_decode(&stream, wlh_protocol_v1_AdcReadRequest_fields, &read))
        return WLH_COPROC_PROTOCOL_ERROR;
    status = coproc->config.adc.read(
        coproc->config.adc.context, read.pin_id, &millivolts
    );
    if (status != WLH_COPROC_SERVICE_OK)
        return send_service_error(
            coproc, request, WLH_STATUS_DOMAIN_PERIPHERAL, status
        );
    response.pin_id = read.pin_id;
    response.millivolts = millivolts;
    return send_rpc_message(
        coproc,
        request->service_id,
        request->method_id,
        request->request_id,
        WLH_RPC_KIND_RESPONSE,
        WLH_STATUS_DOMAIN_NONE,
        WLH_STATUS_OK,
        wlh_protocol_v1_AdcReadResponse_fields,
        &response
    );
}

/* One allocation covers whichever KV message the method needs; the READ path
   copies its key out before the buffer is reused for the response. */
typedef union kv_message {
    wlh_protocol_v1_KvReadRequest read;
    wlh_protocol_v1_KvReadResponse response;
    wlh_protocol_v1_KvWriteRequest write;
    wlh_protocol_v1_KvEraseRequest erase;
} kv_message_t;

static WLH_NOINLINE wlh_coproc_result_t handle_kv_request(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    const uint8_t *message,
    size_t message_size
) {
    kv_message_t *buffer;
    pb_istream_t stream = pb_istream_from_buffer(message, message_size);
    const pb_msgdesc_t *fields;
    const char *key;
    wlh_coproc_result_t result;
    int status;

    switch (request->method_id) {
    case WLH_KV_METHOD_READ:
        fields = wlh_protocol_v1_KvReadRequest_fields;
        break;
    case WLH_KV_METHOD_WRITE:
        fields = wlh_protocol_v1_KvWriteRequest_fields;
        break;
    case WLH_KV_METHOD_ERASE:
        fields = wlh_protocol_v1_KvEraseRequest_fields;
        break;
    default:
        return send_rpc(
            coproc,
            request->service_id,
            request->method_id,
            request->request_id,
            WLH_RPC_KIND_RESPONSE,
            WLH_STATUS_DOMAIN_PROTOCOL,
            WLH_STATUS_NOT_SUPPORTED,
            NULL,
            0u
        );
    }

    buffer = (kv_message_t *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*buffer)
    );
    if (buffer == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    memset(buffer, 0, sizeof(*buffer));
    if (!pb_decode(&stream, fields, buffer)) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)buffer
        );
        return WLH_COPROC_PROTOCOL_ERROR;
    }

    /* Every KV message puts the key first, so one check covers all three. */
    key = buffer->read.key;
    if (key[0] == '\0' || !utf8_valid(key, strlen(key))) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)buffer
        );
        return send_service_error(
            coproc,
            request,
            WLH_STATUS_DOMAIN_STORAGE,
            WLH_COPROC_SERVICE_INVALID_ARGUMENT
        );
    }

    switch (request->method_id) {
    case WLH_KV_METHOD_READ: {
        char requested_key[WLH_COPROC_KV_MAX_KEY_SIZE + 1u];
        size_t value_size = 0u;

        memcpy(requested_key, key, strlen(key) + 1u);
        if (coproc->config.kv.read == NULL) {
            status = WLH_COPROC_SERVICE_NOT_SUPPORTED;
            break;
        }
        memset(buffer, 0, sizeof(*buffer));
        status = coproc->config.kv.read(
            coproc->config.kv.context,
            requested_key,
            buffer->response.value,
            sizeof(buffer->response.value),
            &value_size
        );
        if (status != WLH_COPROC_SERVICE_OK)
            break;
        /* A backend that overruns its bound or returns non-UTF-8 would put
           unvalidated bytes on the wire. */
        if (value_size > WLH_COPROC_KV_MAX_VALUE_SIZE ||
            !utf8_valid(buffer->response.value, value_size)) {
            status = WLH_COPROC_SERVICE_INTERNAL;
            break;
        }
        buffer->response.value[value_size] = '\0';
        result = send_rpc_message(
            coproc,
            request->service_id,
            request->method_id,
            request->request_id,
            WLH_RPC_KIND_RESPONSE,
            WLH_STATUS_DOMAIN_NONE,
            WLH_STATUS_OK,
            wlh_protocol_v1_KvReadResponse_fields,
            &buffer->response
        );
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)buffer
        );
        return result;
    }
    case WLH_KV_METHOD_WRITE: {
        size_t value_size = strlen(buffer->write.value);

        if (!utf8_valid(buffer->write.value, value_size)) {
            status = WLH_COPROC_SERVICE_INVALID_ARGUMENT;
            break;
        }
        status = coproc->config.kv.write == NULL
                     ? WLH_COPROC_SERVICE_NOT_SUPPORTED
                     : coproc->config.kv.write(
                           coproc->config.kv.context,
                           buffer->write.key,
                           buffer->write.value,
                           value_size
                       );
        break;
    }
    default:
        status = coproc->config.kv.erase == NULL
                     ? WLH_COPROC_SERVICE_NOT_SUPPORTED
                     : coproc->config.kv.erase(
                           coproc->config.kv.context, buffer->erase.key
                       );
        break;
    }

    coproc->config.buffers.free(
        coproc->config.buffers.context, (uint8_t *)buffer
    );
    if (status != WLH_COPROC_SERVICE_OK)
        return send_service_error(
            coproc, request, WLH_STATUS_DOMAIN_STORAGE, status
        );
    return send_rpc(
        coproc,
        request->service_id,
        request->method_id,
        request->request_id,
        WLH_RPC_KIND_RESPONSE,
        WLH_STATUS_DOMAIN_NONE,
        WLH_STATUS_OK,
        NULL,
        0u
    );
}

static WLH_NOINLINE wlh_coproc_result_t handle_rpc(
    wlh_coproc_t *coproc,
    const wlh_frame_header_t *frame_header,
    const uint8_t *payload,
    size_t payload_size
) {
    wlh_rpc_envelope_t request;
    const uint8_t *message;
    size_t message_size;

    if (wlh_rpc_decode(
            &request,
            &message,
            &message_size,
            payload,
            payload_size,
            RPC_BUFFER_SIZE
        ) != WLH_WIRE_OK) {
        return WLH_COPROC_PROTOCOL_ERROR;
    }

    if (request.service_id == WLH_SERVICE_LINK &&
        request.method_id == WLH_LINK_METHOD_CREDIT_UPDATE) {
        wlh_protocol_v1_CreditUpdate update =
            wlh_protocol_v1_CreditUpdate_init_zero;
        pb_istream_t stream = pb_istream_from_buffer(message, message_size);
        if (frame_header->session_id != coproc->session_id ||
            !pb_decode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update) ||
            update.channel_id >= WLH_COPROC_CHANNEL_COUNT) {
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        if (UINT32_MAX - coproc->tx_credit[update.channel_id] < update.units) {
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        if (update.channel_id == WLH_CHANNEL_BLUETOOTH_HCI &&
            coproc->config.bluetooth.hci_tx_ready != NULL &&
            coproc->tx_credit[update.channel_id] <=
                coproc->bluetooth_tx_inflight &&
            coproc->tx_credit[update.channel_id] + update.units >
                coproc->bluetooth_tx_inflight) {
            coproc->tx_credit[update.channel_id] += update.units;
            coproc->config.bluetooth.hci_tx_ready(
                coproc->config.bluetooth.context
            );
        } else {
            coproc->tx_credit[update.channel_id] += update.units;
        }
        if (coproc->state == WLH_COPROC_STATE_CONGESTED) {
            set_state(coproc, WLH_COPROC_STATE_READY);
        }
        return request.kind == WLH_RPC_KIND_REQUEST
                   ? send_status(coproc, &request, 0)
                   : WLH_COPROC_OK;
    }

    if (request.service_id == WLH_SERVICE_LINK &&
        request.method_id == WLH_LINK_METHOD_HEARTBEAT) {
        wlh_protocol_v1_Heartbeat *heartbeat;
        pb_istream_t stream = pb_istream_from_buffer(message, message_size);
        bool valid;
        heartbeat = (wlh_protocol_v1_Heartbeat *)coproc->config.buffers.alloc(
            coproc->config.buffers.context, sizeof(*heartbeat)
        );
        if (heartbeat == NULL)
            return WLH_COPROC_BACKEND_ERROR;
        memset(heartbeat, 0, sizeof(*heartbeat));
        valid =
            frame_header->session_id == coproc->session_id &&
            pb_decode(&stream, wlh_protocol_v1_Heartbeat_fields, heartbeat) &&
            heartbeat->session_id == coproc->session_id;
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)heartbeat
        );
        if (!valid) {
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        return request.kind == WLH_RPC_KIND_REQUEST
                   ? send_status(coproc, &request, 0)
                   : WLH_COPROC_OK;
    }

    if (request.kind != WLH_RPC_KIND_REQUEST) {
        return WLH_COPROC_PROTOCOL_ERROR;
    }

    ++coproc->diagnostics.rpc_requests;
    if (request.service_id == WLH_SERVICE_LINK &&
        request.method_id == WLH_LINK_METHOD_HELLO) {
        return handle_hello_request(
            coproc, frame_header, &request, message, message_size
        );
    }

    if (coproc->state != WLH_COPROC_STATE_READY ||
        frame_header->session_id != coproc->session_id) {
        return WLH_COPROC_INVALID_STATE;
    }

    if (request.service_id == WLH_SERVICE_WIFI) {
        return handle_wifi(coproc, &request, message, message_size);
    }

    if (request.service_id == WLH_SERVICE_BLUETOOTH &&
        bluetooth_backend_present(coproc)) {
        return handle_bluetooth(coproc, &request, message, message_size);
    }

    if (request.service_id == WLH_SERVICE_DIAGNOSTICS &&
        request.method_id == WLH_DIAGNOSTICS_METHOD_PING) {
        uint8_t response_data[64];
        size_t response_size = 0;
        wlh_protocol_v1_DiagnosticsPingRequest ping =
            wlh_protocol_v1_DiagnosticsPingRequest_init_zero;
        wlh_protocol_v1_DiagnosticsPingResponse response =
            wlh_protocol_v1_DiagnosticsPingResponse_init_zero;
        pb_istream_t stream = pb_istream_from_buffer(message, message_size);
        if (!pb_decode(
                &stream, wlh_protocol_v1_DiagnosticsPingRequest_fields, &ping
            )) {
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        response.cookie = ping.cookie;
        response.host_time_us = ping.host_time_us;
        response.coprocessor_uptime_us =
            (now_ms(coproc) - coproc->started_ms) * 1000u;
        if (!encode_message(
                response_data,
                sizeof(response_data),
                &response_size,
                wlh_protocol_v1_DiagnosticsPingResponse_fields,
                &response
            )) {
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        return send_rpc(
            coproc,
            request.service_id,
            request.method_id,
            request.request_id,
            WLH_RPC_KIND_RESPONSE,
            0u,
            0,
            response_data,
            response_size
        );
    }

    if (request.service_id == WLH_SERVICE_DEVICE_INFO &&
        request.method_id == WLH_DEVICE_INFO_METHOD_GET_INFO &&
        coproc->config.device_info.get_info != NULL) {
        return handle_device_info_request(coproc, &request);
    }

    if (request.service_id == WLH_SERVICE_USER_PASSTHROUGH &&
        request.method_id == WLH_USER_PASSTHROUGH_METHOD_SEND &&
        coproc->config.user_passthrough.on_message != NULL) {
        return handle_user_message_request(
            coproc, &request, message, message_size
        );
    }

    /* Optional services answer only once an adapter has supplied a backend;
       otherwise the request falls through to NOT_SUPPORTED below. */
    if (request.service_id == WLH_SERVICE_IO &&
        coproc->config.io.configure != NULL) {
        return handle_io_request(coproc, &request, message, message_size);
    }

    if (request.service_id == WLH_SERVICE_ADC &&
        coproc->config.adc.read != NULL) {
        return handle_adc_request(coproc, &request, message, message_size);
    }

    if (request.service_id == WLH_SERVICE_KV &&
        coproc->config.kv.read != NULL) {
        return handle_kv_request(coproc, &request, message, message_size);
    }

    return send_rpc(
        coproc,
        request.service_id,
        request.method_id,
        request.request_id,
        WLH_RPC_KIND_RESPONSE,
        WLH_STATUS_DOMAIN_PROTOCOL,
        WLH_STATUS_NOT_SUPPORTED,
        NULL,
        0u
    );
}

static WLH_NOINLINE wlh_coproc_result_t
coproc_emit_due_heartbeat(wlh_coproc_t *coproc);
static uint32_t coproc_next_wait_ms(const wlh_coproc_t *coproc);
static WLH_NOINLINE wlh_coproc_result_t
process_frame(wlh_coproc_t *coproc, const uint8_t *frame, size_t size);

/* Both completion paths run on the core task with the state mutex held. A
   completion that does not match the single pending operation (stale session,
   wrong kind or unknown id) is ignored and counted. */
static WLH_NOINLINE void bluetooth_operation_completed(
    wlh_coproc_t *coproc, const coproc_bluetooth_complete_job_t *completed
) {
    uint16_t method_id;
    uint32_t request_id;

    if (!coproc->bluetooth_pending.active ||
        coproc->bluetooth_pending.method_id == WLH_BLUETOOTH_METHOD_GET_INFO ||
        completed->operation_id != coproc->bluetooth_pending.operation_id ||
        coproc->bluetooth_pending.session_id != coproc->session_id) {
        ++coproc->diagnostics.bluetooth_mismatches;
        return;
    }
    method_id = coproc->bluetooth_pending.method_id;
    request_id = coproc->bluetooth_pending.request_id;
    memset(&coproc->bluetooth_pending, 0, sizeof(coproc->bluetooth_pending));
    if (completed->backend_status != 0) {
        (void)send_bluetooth_status(
            coproc, method_id, request_id, WLH_STATUS_INTERNAL
        );
        return;
    }
    switch (method_id) {
    case WLH_BLUETOOTH_METHOD_INITIALIZE:
        coproc->bluetooth_state = BT_STATE_DISABLED;
        break;
    case WLH_BLUETOOTH_METHOD_ENABLE:
        coproc->bluetooth_state = BT_STATE_ENABLED;
        break;
    case WLH_BLUETOOTH_METHOD_DISABLE:
        coproc->bluetooth_state = BT_STATE_DISABLED;
        coproc->bluetooth_hci_stopped = false;
        break;
    default:
        coproc->bluetooth_state = BT_STATE_UNSPECIFIED;
        coproc->bluetooth_hci_stopped = false;
        break;
    }
    (void)send_bluetooth_status(coproc, method_id, request_id, WLH_STATUS_OK);
}

static WLH_NOINLINE void bluetooth_info_completed(
    wlh_coproc_t *coproc, const coproc_bluetooth_info_job_t *completed
) {
    uint32_t request_id;
    wlh_protocol_v1_BluetoothControllerInfo response =
        wlh_protocol_v1_BluetoothControllerInfo_init_zero;

    if (!coproc->bluetooth_pending.active ||
        coproc->bluetooth_pending.method_id != WLH_BLUETOOTH_METHOD_GET_INFO ||
        completed->operation_id != coproc->bluetooth_pending.operation_id ||
        coproc->bluetooth_pending.session_id != coproc->session_id) {
        ++coproc->diagnostics.bluetooth_mismatches;
        return;
    }
    request_id = coproc->bluetooth_pending.request_id;
    memset(&coproc->bluetooth_pending, 0, sizeof(coproc->bluetooth_pending));
    if (completed->backend_status != 0) {
        (void)send_bluetooth_status(
            coproc,
            WLH_BLUETOOTH_METHOD_GET_INFO,
            request_id,
            WLH_STATUS_INTERNAL
        );
        return;
    }
    response.state =
        (wlh_protocol_v1_BluetoothControllerState)coproc->bluetooth_state;
    if (completed->info.has_public_address) {
        response.public_address.size =
            (pb_size_t)sizeof(completed->info.public_address);
        memcpy(
            response.public_address.bytes,
            completed->info.public_address,
            sizeof(completed->info.public_address)
        );
    }
    response.hci_version = completed->info.hci_version;
    response.manufacturer_id = completed->info.manufacturer_id;
    response.feature_bits = completed->info.feature_bits;
    response.max_hci_packet = completed->info.max_hci_packet;
    (void)send_rpc_message(
        coproc,
        WLH_SERVICE_BLUETOOTH,
        WLH_BLUETOOTH_METHOD_GET_INFO,
        request_id,
        WLH_RPC_KIND_RESPONSE,
        WLH_STATUS_DOMAIN_NONE,
        WLH_STATUS_OK,
        wlh_protocol_v1_BluetoothControllerInfo_fields,
        &response
    );
}

static void coproc_worker(void *argument) {
    wlh_coproc_t *coproc = argument;
    coproc_job_t job;
    (void)coproc->config.osal.mutex_lock(
        coproc->config.osal.context, &coproc->state_mutex, WLH_OSAL_WAIT_FOREVER
    );
    set_state(coproc, WLH_COPROC_STATE_WAITING_FOR_HELLO);
    coproc->started_ms = now_ms(coproc);
    coproc->last_heartbeat_ms = coproc->started_ms;
    coproc->config.osal.mutex_unlock(
        coproc->config.osal.context, &coproc->state_mutex
    );

    while (!coproc->worker_stopping) {
        uint32_t wait_ms;
        (void)coproc->config.osal.mutex_lock(
            coproc->config.osal.context,
            &coproc->state_mutex,
            WLH_OSAL_WAIT_FOREVER
        );
        (void)coproc_emit_due_heartbeat(coproc);
        wait_ms = coproc_next_wait_ms(coproc);
        coproc->config.osal.mutex_unlock(
            coproc->config.osal.context, &coproc->state_mutex
        );

        if (coproc->config.osal.queue_receive(
                coproc->config.osal.context, &coproc->core_queue, &job, wait_ms
            ) == 0) {
            (void)coproc->config.osal.mutex_lock(
                coproc->config.osal.context,
                &coproc->state_mutex,
                WLH_OSAL_WAIT_FOREVER
            );
            if (job.kind == COPROC_JOB_STOP) {
                coproc->worker_stopping = true;
            } else if (job.kind == COPROC_JOB_RX_FRAME) {
                coproc_data_job_t *data = job.payload;
                wlh_coproc_result_t result =
                    process_frame(coproc, data->data, data->size);
                if (result != WLH_COPROC_OK) {
                    WLH_LOGW(
                        "wlh_coproc",
                        "RX frame processing failed: %d",
                        (int)result
                    );
                }
                coproc->config.buffers.free(
                    coproc->config.buffers.context, (uint8_t *)data
                );
            } else if (job.kind == COPROC_JOB_WIFI_INITIALIZED) {
                coproc_wifi_initialized_job_t *completed = job.payload;
                if (coproc->wifi_initialize_pending.active &&
                    completed->operation_id ==
                        coproc->wifi_initialize_pending.operation_id &&
                    coproc->wifi_initialize_pending.session_id ==
                        coproc->session_id) {
                    wlh_rpc_envelope_t request;
                    memset(&request, 0, sizeof(request));
                    request.service_id = WLH_SERVICE_WIFI;
                    request.method_id = WLH_WIFI_METHOD_INITIALIZE;
                    request.request_id =
                        coproc->wifi_initialize_pending.request_id;
                    request.kind = WLH_RPC_KIND_REQUEST;
                    memset(
                        &coproc->wifi_initialize_pending,
                        0,
                        sizeof(coproc->wifi_initialize_pending)
                    );
                    (void)send_status(
                        coproc, &request, completed->backend_status
                    );
                }
                coproc->config.buffers.free(
                    coproc->config.buffers.context, (uint8_t *)completed
                );
            } else if (job.kind == COPROC_JOB_RPC_EVENT) {
                coproc_data_job_t *data = job.payload;
                (void)send_rpc(
                    coproc,
                    data->service_id,
                    data->method_id,
                    0u,
                    WLH_RPC_KIND_EVENT,
                    0u,
                    0,
                    data->data,
                    data->size
                );
                coproc->config.buffers.free(
                    coproc->config.buffers.context, (uint8_t *)data
                );
            } else if (job.kind == COPROC_JOB_ETHERNET_TX) {
                coproc_data_job_t *data = job.payload;
                (void)send_payload(
                    coproc, data->channel, data->data, data->size
                );
                coproc->config.buffers.free(
                    coproc->config.buffers.context, (uint8_t *)data
                );
            } else if (job.kind == COPROC_JOB_BLUETOOTH_COMPLETE) {
                coproc_bluetooth_complete_job_t *completed = job.payload;
                bluetooth_operation_completed(coproc, completed);
                coproc->config.buffers.free(
                    coproc->config.buffers.context, (uint8_t *)completed
                );
            } else if (job.kind == COPROC_JOB_BLUETOOTH_INFO) {
                coproc_bluetooth_info_job_t *completed = job.payload;
                bluetooth_info_completed(coproc, completed);
                coproc->config.buffers.free(
                    coproc->config.buffers.context, (uint8_t *)completed
                );
            } else if (job.kind == COPROC_JOB_BLUETOOTH_TX) {
                coproc_data_job_t *data = job.payload;
                if (data->channel == WLH_CHANNEL_BLUETOOTH_HCI_ADV) {
                    if (coproc->bluetooth_adv_tx_inflight > 0u)
                        --coproc->bluetooth_adv_tx_inflight;
                } else if (coproc->bluetooth_tx_inflight > 0u) {
                    --coproc->bluetooth_tx_inflight;
                }
                if (coproc->bluetooth_hci_stopped) {
                    ++coproc->diagnostics.hci_drops;
                } else {
                    (void)send_payload(
                        coproc, data->channel, data->data, data->size
                    );
                }
                coproc->config.buffers.free(
                    coproc->config.buffers.context, (uint8_t *)data
                );
            } else if (job.kind == COPROC_JOB_BLUETOOTH_FATAL) {
                coproc_bluetooth_fatal_job_t *fatal = job.payload;
                bluetooth_enter_error(coproc, fatal->reason);
                coproc->config.buffers.free(
                    coproc->config.buffers.context, (uint8_t *)fatal
                );
            } else if (job.kind == COPROC_JOB_TRANSPORT_FAILED) {
                WLH_LOGW("wlh_coproc", "transport failed");
                set_state(coproc, WLH_COPROC_STATE_FAILED);
            }
            coproc->config.osal.mutex_unlock(
                coproc->config.osal.context, &coproc->state_mutex
            );
        }
    }

    (void)coproc->config.osal.mutex_lock(
        coproc->config.osal.context, &coproc->state_mutex, WLH_OSAL_WAIT_FOREVER
    );
    set_state(coproc, WLH_COPROC_STATE_STOPPED);
    coproc->session_id = 0u;
    coproc->config.osal.mutex_unlock(
        coproc->config.osal.context, &coproc->state_mutex
    );
}

wlh_coproc_result_t wlh_coproc_init(
    wlh_coproc_t *coproc, const wlh_coproc_config_t *config
) {
    if (coproc == NULL || config == NULL || config->port.submit_tx == NULL ||
        config->buffers.alloc == NULL || config->buffers.free == NULL ||
        !wlh_osal_ops_valid(&config->osal) ||
        config->heartbeat_interval_ms == 0u ||
        config->max_frame_size < WLH_FRAME_HEADER_SIZE ||
        config->max_frame_size > WLH_COPROC_MAX_FRAME_SIZE) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }

    /* The Bluetooth backend is all-or-none: advertising the service with a
       partial ops table would strand lifecycle requests. hci_tx_ready stays
       optional. */
    if ((config->bluetooth.initialize != NULL ||
         config->bluetooth.enable != NULL ||
         config->bluetooth.disable != NULL ||
         config->bluetooth.deinitialize != NULL ||
         config->bluetooth.get_info != NULL ||
         config->bluetooth.hci_send != NULL) &&
        (config->bluetooth.initialize == NULL ||
         config->bluetooth.enable == NULL ||
         config->bluetooth.disable == NULL ||
         config->bluetooth.deinitialize == NULL ||
         config->bluetooth.get_info == NULL ||
         config->bluetooth.hci_send == NULL)) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }

    memset(coproc, 0, sizeof(*coproc));
    coproc->config = *config;
    if (coproc->config.core_queue_depth == 0u)
        coproc->config.core_queue_depth = 16u;
    if (coproc->config.core_queue_depth > WLH_COPROC_MAX_QUEUE_DEPTH)
        return WLH_COPROC_INVALID_ARGUMENT;
    if (coproc->config.stop_timeout_ms == 0u)
        coproc->config.stop_timeout_ms = 3000u;
    coproc->next_session_id =
        config->initial_session_id != 0u ? config->initial_session_id : 1u;
    coproc->next_backend_operation_id = 1u;
    return WLH_COPROC_OK;
}

wlh_coproc_result_t wlh_coproc_start(wlh_coproc_t *coproc) {
    wlh_osal_task_attributes_t attributes;
    if (coproc == NULL || coproc->state != WLH_COPROC_STATE_STOPPED) {
        return WLH_COPROC_INVALID_STATE;
    }

    if (coproc->config.osal.mutex_create(
            coproc->config.osal.context, &coproc->state_mutex
        ) != 0)
        return WLH_COPROC_INVALID_STATE;
    if (coproc->config.osal.queue_create(
            coproc->config.osal.context,
            &coproc->core_queue,
            coproc->core_queue_storage,
            sizeof(coproc_job_t),
            coproc->config.core_queue_depth
        ) != 0) {
        coproc->config.osal.mutex_destroy(
            coproc->config.osal.context, &coproc->state_mutex
        );
        return WLH_COPROC_INVALID_STATE;
    }
    coproc->worker_stopping = false;
    coproc->worker_started = true;
    attributes = coproc->config.core_task;
    if (attributes.name == NULL)
        attributes.name = "wlh-coproc-core";
    if (coproc->config.osal.task_create(
            coproc->config.osal.context,
            &coproc->core_task,
            &attributes,
            coproc_worker,
            coproc
        ) != 0) {
        coproc->worker_started = false;
        coproc->config.osal.queue_destroy(
            coproc->config.osal.context, &coproc->core_queue
        );
        coproc->config.osal.mutex_destroy(
            coproc->config.osal.context, &coproc->state_mutex
        );
        return WLH_COPROC_INVALID_STATE;
    }
    return WLH_COPROC_OK;
}

wlh_coproc_result_t wlh_coproc_stop(wlh_coproc_t *coproc) {
    if (coproc == NULL) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }
    if (!coproc->worker_started ||
        enqueue_job(coproc, COPROC_JOB_STOP, NULL, 100u) != 0)
        return WLH_COPROC_INVALID_STATE;
    if (coproc->config.osal.task_join(
            coproc->config.osal.context,
            &coproc->core_task,
            coproc->config.stop_timeout_ms
        ) != 0)
        return WLH_COPROC_BACKEND_ERROR;
    coproc->worker_started = false;
    coproc->config.osal.queue_destroy(
        coproc->config.osal.context, &coproc->core_queue
    );
    coproc->config.osal.mutex_destroy(
        coproc->config.osal.context, &coproc->state_mutex
    );
    return WLH_COPROC_OK;
}

static WLH_NOINLINE wlh_coproc_result_t
coproc_emit_due_heartbeat(wlh_coproc_t *coproc) {
    wlh_protocol_v1_Heartbeat *heartbeat;
    uint64_t now;

    if (coproc == NULL) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }

    now = now_ms(coproc);
    if (coproc->state == WLH_COPROC_STATE_READY &&
        now - coproc->last_heartbeat_ms >=
            coproc->config.heartbeat_interval_ms) {
        heartbeat = (wlh_protocol_v1_Heartbeat *)coproc->config.buffers.alloc(
            coproc->config.buffers.context, sizeof(*heartbeat)
        );
        if (heartbeat != NULL) {
            memset(heartbeat, 0, sizeof(*heartbeat));
            heartbeat->session_id = coproc->session_id;
            heartbeat->state = wlh_protocol_v1_LinkState_LINK_STATE_HEALTHY;
            heartbeat->uptime_ms = now - coproc->started_ms;
            heartbeat->monotonic_ms = now;
            (void)send_rpc_message(
                coproc,
                WLH_SERVICE_LINK,
                WLH_LINK_METHOD_HEARTBEAT,
                0u,
                WLH_RPC_KIND_EVENT,
                0u,
                0,
                wlh_protocol_v1_Heartbeat_fields,
                heartbeat
            );
            coproc->config.buffers.free(
                coproc->config.buffers.context, (uint8_t *)heartbeat
            );
        }
        coproc->last_heartbeat_ms = now;
    }
    return WLH_COPROC_OK;
}

static uint32_t coproc_next_wait_ms(const wlh_coproc_t *coproc) {
    uint64_t current;
    uint64_t deadline;
    uint64_t remaining;

    if (coproc->state != WLH_COPROC_STATE_READY)
        return WLH_OSAL_WAIT_FOREVER;
    current = now_ms(coproc);
    deadline = coproc->last_heartbeat_ms + coproc->config.heartbeat_interval_ms;
    if (deadline <= current)
        return WLH_OSAL_NO_WAIT;
    remaining = deadline - current;
    if (remaining >= WLH_OSAL_WAIT_FOREVER)
        return WLH_OSAL_WAIT_FOREVER - 1u;
    return (uint32_t)remaining;
}

/* Host->Controller H4 shape checks. `payload` excludes the H4 type octet. */
static bool hci_record_valid(const wlh_raw_record_view_t *record) {
    const uint8_t *packet = record->payload;
    size_t size = record->payload_size;

    if (size > WLH_COPROC_MAX_HCI_PACKET)
        return false;
    switch (record->record_type) {
    case WLH_H4_TYPE_COMMAND:
        return size >= 3u && (size_t)packet[2] + 3u == size;
    case WLH_H4_TYPE_ACL:
        return size >= 4u &&
               ((size_t)packet[2] | ((size_t)packet[3] << 8)) + 4u == size;
    default:
        /* Events flow only Controller->Host; SCO and ISO are unsupported. */
        return false;
    }
}

static WLH_NOINLINE wlh_coproc_result_t process_hci_frame(
    wlh_coproc_t *coproc, const uint8_t *payload, size_t payload_size
) {
    wlh_raw_record_iterator_t iterator;
    wlh_raw_record_view_t record;
    wlh_wire_result_t record_result = WLH_WIRE_INVALID_ARGUMENT;

    if (!bluetooth_backend_present(coproc))
        return WLH_COPROC_PROTOCOL_ERROR;
    if (coproc->bluetooth_hci_stopped) {
        ++coproc->diagnostics.hci_drops;
        return WLH_COPROC_INVALID_STATE;
    }

    /* Validate every aggregated record before delivering any of them. A
       malformed record poisons the whole frame: it is dropped, counted and
       latches the ERROR state so no further HCI flows this session. */
    if (payload_size != 0u &&
        wlh_raw_record_iterator_init(&iterator, payload, payload_size) ==
            WLH_WIRE_OK) {
        while ((record_result =
                    wlh_raw_record_iterator_next(&iterator, &record)) ==
               WLH_WIRE_OK) {
            if (!hci_record_valid(&record)) {
                record_result = WLH_WIRE_INVALID_ARGUMENT;
                break;
            }
        }
    }
    if (record_result != WLH_WIRE_END) {
        ++coproc->diagnostics.hci_malformed;
        WLH_LOGW("wlh_coproc", "malformed HCI frame (%zu bytes)", payload_size);
        bluetooth_enter_error(
            coproc, WLH_COPROC_BLUETOOTH_REASON_MALFORMED_HCI
        );
        return WLH_COPROC_PROTOCOL_ERROR;
    }

    (void)wlh_raw_record_iterator_init(&iterator, payload, payload_size);
    while (wlh_raw_record_iterator_next(&iterator, &record) == WLH_WIRE_OK) {
        if (coproc->config.bluetooth.hci_send(
                coproc->config.bluetooth.context,
                (uint8_t)record.record_type,
                record.payload,
                record.payload_size
            ) != 0) {
            /* Withhold the credit so the rejection backpressures the host
               instead of silently dropping the rest of the frame. */
            ++coproc->diagnostics.hci_drops;
            return WLH_COPROC_BACKEND_ERROR;
        }
    }
    return send_credit_update(coproc, WLH_CHANNEL_BLUETOOTH_HCI);
}

static WLH_NOINLINE wlh_coproc_result_t
process_frame(wlh_coproc_t *coproc, const uint8_t *frame, size_t size) {
    wlh_frame_header_t header;
    const uint8_t *payload;
    size_t payload_size;
    wlh_wire_result_t wire;

    if (coproc == NULL || frame == NULL) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }

    wire = wlh_frame_decode(
        &header,
        &payload,
        &payload_size,
        frame,
        size,
        coproc->config.max_frame_size
    );
    if (wire != WLH_WIRE_OK) {
        if (wire == WLH_WIRE_CHECKSUM_MISMATCH) {
            ++coproc->diagnostics.checksum_errors;
        }
        WLH_LOGW("wlh_coproc", "frame decode error %d", (int)wire);
        return WLH_COPROC_PROTOCOL_ERROR;
    }

    if (coproc->rx_sequence_valid[header.channel] &&
        header.sequence != coproc->rx_sequence[header.channel]) {
        ++coproc->diagnostics.sequence_gaps;
        WLH_LOGW(
            "wlh_coproc",
            "sequence gap on channel %u: expected %lu, got %lu",
            (unsigned)header.channel,
            (unsigned long)coproc->rx_sequence[header.channel],
            (unsigned long)header.sequence
        );
    }
    coproc->rx_sequence[header.channel] = header.sequence + 1u;
    coproc->rx_sequence_valid[header.channel] = true;
    ++coproc->diagnostics.rx_frames;
    coproc->diagnostics.last_peer_activity_ms = now_ms(coproc);

    if (header.channel == WLH_CHANNEL_LINK_CONTROL ||
        header.channel == WLH_CHANNEL_CONTROL_RPC) {
        return handle_rpc(coproc, &header, payload, payload_size);
    }

    if (header.channel == WLH_CHANNEL_BLUETOOTH_HCI) {
        if ((coproc->state != WLH_COPROC_STATE_READY &&
             coproc->state != WLH_COPROC_STATE_CONGESTED) ||
            header.session_id != coproc->session_id) {
            return WLH_COPROC_INVALID_STATE;
        }
        return process_hci_frame(coproc, payload, payload_size);
    }

    if (header.channel == WLH_CHANNEL_ETHERNET_STA ||
        header.channel == WLH_CHANNEL_ETHERNET_AP) {
        bool payload_valid = payload_size != 0u;
        wlh_raw_record_iterator_t iterator;
        wlh_raw_record_view_t record;
        wlh_wire_result_t record_result = WLH_WIRE_INVALID_ARGUMENT;

        /* Validate every aggregated record before delivering any of them,
           so a malformed tail cannot partially deliver a frame. */
        if (payload_valid &&
            wlh_raw_record_iterator_init(&iterator, payload, payload_size) ==
                WLH_WIRE_OK) {
            while ((record_result =
                        wlh_raw_record_iterator_next(&iterator, &record)) ==
                   WLH_WIRE_OK) {
            }
        }
        payload_valid = payload_valid && record_result == WLH_WIRE_END;
        if (payload_valid) {
            wlh_coproc_ethernet_rx_fn receive =
                header.channel == WLH_CHANNEL_ETHERNET_STA
                    ? coproc->config.port.ethernet_rx
                    : coproc->config.port.ethernet_ap_rx;
            (void)wlh_raw_record_iterator_init(
                &iterator, payload, payload_size
            );
            while (wlh_raw_record_iterator_next(&iterator, &record) ==
                   WLH_WIRE_OK) {
                if (record.record_type != 1u || receive == NULL) {
                    continue;
                }
                receive(
                    coproc->config.port.context,
                    record.payload,
                    record.payload_size
                );
            }
        } else {
            WLH_LOGW(
                "wlh_coproc",
                "malformed raw record on channel %u (%zu bytes)",
                (unsigned)header.channel,
                payload_size
            );
        }
        /* Return the credit even when the payload is rejected, so a transient
           fault cannot permanently strand the peer in CONGESTED. */
        (void)send_credit_update(coproc, header.channel);
        return payload_valid ? WLH_COPROC_OK : WLH_COPROC_PROTOCOL_ERROR;
    }

    return WLH_COPROC_PROTOCOL_ERROR;
}

wlh_coproc_result_t wlh_coproc_on_frame(
    wlh_coproc_t *coproc, const uint8_t *frame, size_t size
) {
    coproc_data_job_t *job;
    if (coproc == NULL || frame == NULL || size < WLH_FRAME_HEADER_SIZE ||
        size > coproc->config.max_frame_size || !coproc->worker_started)
        return WLH_COPROC_INVALID_ARGUMENT;
    job = (coproc_data_job_t *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*job) + size
    );
    if (job == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    memset(job, 0, sizeof(*job));
    job->size = size;
    memcpy(job->data, frame, size);
    if (enqueue_job(coproc, COPROC_JOB_RX_FRAME, job, WLH_OSAL_NO_WAIT) != 0) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)job
        );
        return WLH_COPROC_BACKEND_ERROR;
    }
    return WLH_COPROC_OK;
}

static wlh_coproc_result_t send_event_message(
    wlh_coproc_t *coproc,
    uint16_t service_id,
    uint16_t method_id,
    const pb_msgdesc_t *fields,
    const void *message
) {
    size_t size = 0;
    size_t encoded_size = 0;
    coproc_data_job_t *job;

    if (!pb_get_encoded_size(&size, fields, message) ||
        size > RPC_BUFFER_SIZE) {
        return WLH_COPROC_PROTOCOL_ERROR;
    }
    job = (coproc_data_job_t *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*job) + size
    );
    if (job == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    memset(job, 0, sizeof(*job));
    if (!encode_message(job->data, size, &encoded_size, fields, message) ||
        encoded_size != size) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)job
        );
        return WLH_COPROC_PROTOCOL_ERROR;
    }
    job->method_id = method_id;
    job->service_id = service_id;
    job->size = size;
    if (enqueue_job(coproc, COPROC_JOB_RPC_EVENT, job, WLH_OSAL_NO_WAIT) != 0) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)job
        );
        return WLH_COPROC_BACKEND_ERROR;
    }
    return WLH_COPROC_OK;
}

static wlh_coproc_result_t send_wifi_message(
    wlh_coproc_t *coproc,
    uint16_t method_id,
    const pb_msgdesc_t *fields,
    const void *message
) {
    return send_event_message(
        coproc, WLH_SERVICE_WIFI, method_id, fields, message
    );
}

wlh_coproc_result_t wlh_coproc_wifi_initialized(
    wlh_coproc_t *coproc, uint32_t operation_id, int backend_status
) {
    coproc_wifi_initialized_job_t *job;
    if (coproc == NULL || operation_id == 0u || !coproc->worker_started)
        return WLH_COPROC_INVALID_ARGUMENT;
    job = (coproc_wifi_initialized_job_t *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*job)
    );
    if (job == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    job->operation_id = operation_id;
    job->backend_status = backend_status;
    if (enqueue_job(
            coproc, COPROC_JOB_WIFI_INITIALIZED, job, WLH_OSAL_NO_WAIT
        ) != 0) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)job
        );
        return WLH_COPROC_BACKEND_ERROR;
    }
    return WLH_COPROC_OK;
}

wlh_coproc_result_t wlh_coproc_wifi_scan_result(
    wlh_coproc_t *coproc, uint32_t scan_id, const wlh_coproc_bss_t *bss
) {
    wlh_protocol_v1_WifiScanResultEvent *event;
    wlh_protocol_v1_WifiNetwork *network;
    wlh_coproc_result_t result;

    if (coproc == NULL || bss == NULL || !bss_ssid_valid(bss)) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }

    event = (wlh_protocol_v1_WifiScanResultEvent *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*event)
    );
    if (event == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    memset(event, 0, sizeof(*event));
    event->scan_id = scan_id;
    event->networks_count = 1u;
    network = &event->networks[0];

    network->ssid.size = bss->ssid_size;
    memcpy(network->ssid.bytes, bss->ssid, bss->ssid_size);

    network->bssid.size = 6u;
    memcpy(network->bssid.bytes, bss->bssid, 6u);

    network->channel = bss->channel;
    network->rssi_dbm = bss->rssi_dbm;
    network->security = (wlh_protocol_v1_WifiSecurity)bss->security;
    result = send_wifi_message(
        coproc,
        WLH_WIFI_EVENT_SCAN_RESULT,
        wlh_protocol_v1_WifiScanResultEvent_fields,
        event
    );
    coproc->config.buffers.free(
        coproc->config.buffers.context, (uint8_t *)event
    );
    return result;
}

wlh_coproc_result_t wlh_coproc_wifi_scan_completed(
    wlh_coproc_t *coproc,
    uint32_t scan_id,
    uint32_t result_count,
    bool cancelled
) {
    wlh_protocol_v1_WifiScanCompletedEvent event =
        wlh_protocol_v1_WifiScanCompletedEvent_init_zero;
    event.scan_id = scan_id;
    event.result_count = result_count;
    event.cancelled = cancelled;
    return send_wifi_message(
        coproc,
        WLH_WIFI_EVENT_SCAN_COMPLETED,
        wlh_protocol_v1_WifiScanCompletedEvent_fields,
        &event
    );
}

wlh_coproc_result_t wlh_coproc_wifi_connected(
    wlh_coproc_t *coproc, const wlh_coproc_bss_t *bss
) {
    wlh_protocol_v1_WifiConnectedEvent event =
        wlh_protocol_v1_WifiConnectedEvent_init_zero;

    if (coproc == NULL || bss == NULL || !bss_ssid_valid(bss)) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }

    event.has_link = true;
    event.link.interface = wlh_protocol_v1_WifiInterface_WIFI_INTERFACE_STA;
    event.link.connected = true;

    event.link.ssid.size = bss->ssid_size;
    memcpy(event.link.ssid.bytes, bss->ssid, bss->ssid_size);

    event.link.bssid.size = 6u;
    memcpy(event.link.bssid.bytes, bss->bssid, 6u);

    event.link.mac.size = 6u;
    memcpy(event.link.mac.bytes, bss->interface_mac, 6u);

    event.link.channel = bss->channel;
    event.link.rssi_dbm = bss->rssi_dbm;
    event.link.security = (wlh_protocol_v1_WifiSecurity)bss->security;
    return send_wifi_message(
        coproc,
        WLH_WIFI_EVENT_CONNECTED,
        wlh_protocol_v1_WifiConnectedEvent_fields,
        &event
    );
}

wlh_coproc_result_t wlh_coproc_wifi_disconnected(
    wlh_coproc_t *coproc, uint32_t reason, bool locally_initiated
) {
    wlh_protocol_v1_WifiDisconnectedEvent event =
        wlh_protocol_v1_WifiDisconnectedEvent_init_zero;
    event.interface = wlh_protocol_v1_WifiInterface_WIFI_INTERFACE_STA;
    event.reason = (wlh_protocol_v1_WifiDisconnectReason)reason;
    event.locally_initiated = locally_initiated;
    return send_wifi_message(
        coproc,
        WLH_WIFI_EVENT_DISCONNECTED,
        wlh_protocol_v1_WifiDisconnectedEvent_fields,
        &event
    );
}

wlh_coproc_result_t wlh_coproc_wifi_ap_started(
    wlh_coproc_t *coproc, const wlh_coproc_bss_t *ap
) {
    wlh_protocol_v1_WifiConnectedEvent event =
        wlh_protocol_v1_WifiConnectedEvent_init_zero;

    if (coproc == NULL || ap == NULL || !bss_ssid_valid(ap))
        return WLH_COPROC_INVALID_ARGUMENT;
    event.has_link = true;
    event.link.interface = wlh_protocol_v1_WifiInterface_WIFI_INTERFACE_AP;
    event.link.connected = true;
    event.link.ssid.size = ap->ssid_size;
    memcpy(event.link.ssid.bytes, ap->ssid, ap->ssid_size);
    event.link.bssid.size = 6u;
    memcpy(event.link.bssid.bytes, ap->bssid, 6u);
    event.link.mac.size = 6u;
    memcpy(event.link.mac.bytes, ap->interface_mac, 6u);
    event.link.channel = ap->channel;
    event.link.security = (wlh_protocol_v1_WifiSecurity)ap->security;
    return send_wifi_message(
        coproc,
        WLH_WIFI_EVENT_CONNECTED,
        wlh_protocol_v1_WifiConnectedEvent_fields,
        &event
    );
}

wlh_coproc_result_t wlh_coproc_wifi_ap_stopped(
    wlh_coproc_t *coproc, uint32_t reason, bool locally_initiated
) {
    wlh_protocol_v1_WifiDisconnectedEvent event =
        wlh_protocol_v1_WifiDisconnectedEvent_init_zero;
    if (coproc == NULL)
        return WLH_COPROC_INVALID_ARGUMENT;
    event.interface = wlh_protocol_v1_WifiInterface_WIFI_INTERFACE_AP;
    event.reason = (wlh_protocol_v1_WifiDisconnectReason)reason;
    event.locally_initiated = locally_initiated;
    return send_wifi_message(
        coproc,
        WLH_WIFI_EVENT_DISCONNECTED,
        wlh_protocol_v1_WifiDisconnectedEvent_fields,
        &event
    );
}

wlh_coproc_result_t wlh_coproc_wifi_ap_client_joined(
    wlh_coproc_t *coproc,
    const uint8_t mac[6],
    int32_t rssi_dbm,
    uint32_t association_id
) {
    wlh_protocol_v1_WifiApClientJoinedEvent event =
        wlh_protocol_v1_WifiApClientJoinedEvent_init_zero;

    if (coproc == NULL || mac == NULL) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }

    event.has_client = true;
    event.client.mac.size = 6u;
    memcpy(event.client.mac.bytes, mac, 6u);
    event.client.rssi_dbm = rssi_dbm;
    event.client.association_id = association_id;
    return send_wifi_message(
        coproc,
        WLH_WIFI_EVENT_AP_CLIENT_JOINED,
        wlh_protocol_v1_WifiApClientJoinedEvent_fields,
        &event
    );
}

wlh_coproc_result_t wlh_coproc_wifi_ap_client_left(
    wlh_coproc_t *coproc,
    const uint8_t mac[6],
    uint32_t association_id,
    uint32_t ieee80211_reason
) {
    wlh_protocol_v1_WifiApClientLeftEvent event =
        wlh_protocol_v1_WifiApClientLeftEvent_init_zero;

    if (coproc == NULL || mac == NULL) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }

    event.mac.size = 6u;
    memcpy(event.mac.bytes, mac, 6u);
    event.association_id = association_id;
    event.ieee80211_reason = ieee80211_reason;
    return send_wifi_message(
        coproc,
        WLH_WIFI_EVENT_AP_CLIENT_LEFT,
        wlh_protocol_v1_WifiApClientLeftEvent_fields,
        &event
    );
}

static wlh_coproc_result_t ethernet_send(
    wlh_coproc_t *coproc, uint8_t channel, const uint8_t *frame, size_t size
) {
    coproc_data_job_t *job;

    if (coproc == NULL || frame == NULL ||
        size + RAW_HEADER_SIZE >
            WLH_COPROC_MAX_FRAME_SIZE - WLH_FRAME_HEADER_SIZE ||
        !coproc->worker_started) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }
    job = (coproc_data_job_t *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*job) + RAW_HEADER_SIZE + size
    );
    if (job == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    memset(job, 0, sizeof(*job));
    job->channel = channel;
    {
        size_t record_size = 0;
        if (wlh_raw_record_encode(
                job->data,
                RAW_HEADER_SIZE + size,
                &record_size,
                1u,
                0u,
                frame,
                size
            ) != WLH_WIRE_OK) {
            coproc->config.buffers.free(
                coproc->config.buffers.context, (uint8_t *)job
            );
            return WLH_COPROC_INVALID_ARGUMENT;
        }
        job->size = record_size;
    }
    if (enqueue_job(coproc, COPROC_JOB_ETHERNET_TX, job, WLH_OSAL_NO_WAIT) !=
        0) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)job
        );
        return WLH_COPROC_BACKEND_ERROR;
    }
    return WLH_COPROC_OK;
}

wlh_coproc_result_t wlh_coproc_ethernet_sta_send(
    wlh_coproc_t *coproc, const uint8_t *frame, size_t size
) {
    return ethernet_send(coproc, WLH_CHANNEL_ETHERNET_STA, frame, size);
}

wlh_coproc_result_t wlh_coproc_ethernet_ap_send(
    wlh_coproc_t *coproc, const uint8_t *frame, size_t size
) {
    return ethernet_send(coproc, WLH_CHANNEL_ETHERNET_AP, frame, size);
}

wlh_coproc_result_t wlh_coproc_user_message_result(
    wlh_coproc_t *coproc,
    uint32_t endpoint_id,
    uint32_t message_type,
    uint32_t correlation_id,
    int32_t result,
    const uint8_t *payload,
    size_t payload_size
) {
    wlh_protocol_v1_UserMessageResultEvent *event;
    wlh_coproc_result_t send_result;

    if (coproc == NULL ||
        payload_size >
            sizeof(
                ((wlh_protocol_v1_UserMessageResultEvent *)0)->payload.bytes
            ) ||
        (payload == NULL && payload_size != 0u)) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }
    event =
        (wlh_protocol_v1_UserMessageResultEvent *)coproc->config.buffers.alloc(
            coproc->config.buffers.context, sizeof(*event)
        );
    if (event == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    memset(event, 0, sizeof(*event));
    event->endpoint_id = endpoint_id;
    event->message_type = message_type;
    event->correlation_id = correlation_id;
    event->result = result;
    event->payload.size = payload_size;
    if (payload_size != 0u)
        memcpy(event->payload.bytes, payload, payload_size);
    send_result = send_event_message(
        coproc,
        WLH_SERVICE_USER_PASSTHROUGH,
        WLH_USER_PASSTHROUGH_EVENT_RESULT,
        wlh_protocol_v1_UserMessageResultEvent_fields,
        event
    );
    coproc->config.buffers.free(
        coproc->config.buffers.context, (uint8_t *)event
    );
    return send_result;
}

wlh_coproc_result_t wlh_coproc_bluetooth_operation_complete(
    wlh_coproc_t *coproc, uint32_t operation_id, int backend_status
) {
    coproc_bluetooth_complete_job_t *job;
    if (coproc == NULL || operation_id == 0u || !coproc->worker_started)
        return WLH_COPROC_INVALID_ARGUMENT;
    job = (coproc_bluetooth_complete_job_t *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*job)
    );
    if (job == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    job->operation_id = operation_id;
    job->backend_status = backend_status;
    if (enqueue_job(
            coproc, COPROC_JOB_BLUETOOTH_COMPLETE, job, WLH_OSAL_NO_WAIT
        ) != 0) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)job
        );
        return WLH_COPROC_BACKEND_ERROR;
    }
    return WLH_COPROC_OK;
}

wlh_coproc_result_t wlh_coproc_bluetooth_info_result(
    wlh_coproc_t *coproc,
    uint32_t operation_id,
    int backend_status,
    const wlh_coproc_bluetooth_info_t *info
) {
    coproc_bluetooth_info_job_t *job;
    if (coproc == NULL || operation_id == 0u || !coproc->worker_started ||
        (info == NULL && backend_status == 0))
        return WLH_COPROC_INVALID_ARGUMENT;
    job = (coproc_bluetooth_info_job_t *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*job)
    );
    if (job == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    memset(job, 0, sizeof(*job));
    job->operation_id = operation_id;
    job->backend_status = backend_status;
    if (info != NULL)
        job->info = *info;
    if (enqueue_job(coproc, COPROC_JOB_BLUETOOTH_INFO, job, WLH_OSAL_NO_WAIT) !=
        0) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)job
        );
        return WLH_COPROC_BACKEND_ERROR;
    }
    return WLH_COPROC_OK;
}

static void bluetooth_release_tx_reservation(
    wlh_coproc_t *coproc, uint8_t channel
) {
    (void)coproc->config.osal.mutex_lock(
        coproc->config.osal.context, &coproc->state_mutex, WLH_OSAL_WAIT_FOREVER
    );
    if (channel == WLH_CHANNEL_BLUETOOTH_HCI_ADV) {
        if (coproc->bluetooth_adv_tx_inflight > 0u)
            --coproc->bluetooth_adv_tx_inflight;
    } else if (coproc->bluetooth_tx_inflight > 0u) {
        --coproc->bluetooth_tx_inflight;
    }
    coproc->config.osal.mutex_unlock(
        coproc->config.osal.context, &coproc->state_mutex
    );
}

wlh_coproc_result_t wlh_coproc_bluetooth_hci_send(
    wlh_coproc_t *coproc,
    uint8_t h4_type,
    const uint8_t *packet,
    size_t packet_size
) {
    coproc_data_job_t *job;
    uint8_t channel = WLH_CHANNEL_BLUETOOTH_HCI;

    if (coproc == NULL || packet == NULL || !coproc->worker_started ||
        packet_size > WLH_COPROC_MAX_HCI_PACKET)
        return WLH_COPROC_INVALID_ARGUMENT;
    if (!bluetooth_backend_present(coproc))
        return WLH_COPROC_NOT_SUPPORTED;
    switch (h4_type) {
    case WLH_H4_TYPE_EVENT:
        if (packet_size < 2u || (size_t)packet[1] + 2u != packet_size)
            return WLH_COPROC_INVALID_ARGUMENT;
        break;
    case WLH_H4_TYPE_ACL:
        if (packet_size < 4u ||
            ((size_t)packet[2] | ((size_t)packet[3] << 8)) + 4u != packet_size)
            return WLH_COPROC_INVALID_ARGUMENT;
        break;
    case WLH_H4_TYPE_SCO:
    case WLH_H4_TYPE_ISO:
        return WLH_COPROC_NOT_SUPPORTED;
    default:
        /* Commands flow only Host->Controller. */
        return WLH_COPROC_INVALID_ARGUMENT;
    }

    /* Reserve a credit up front so a NO_CREDIT result leaves the packet with
       the backend (queue head retained) instead of dropping it. The worker
       releases the reservation when the queued record is actually sent. */
    (void)coproc->config.osal.mutex_lock(
        coproc->config.osal.context, &coproc->state_mutex, WLH_OSAL_WAIT_FOREVER
    );
    if (coproc->bluetooth_hci_stopped) {
        coproc->config.osal.mutex_unlock(
            coproc->config.osal.context, &coproc->state_mutex
        );
        return WLH_COPROC_INVALID_STATE;
    }
    if (h4_type == WLH_H4_TYPE_EVENT && coproc->bluetooth_adv_channel &&
        wlh_hci_event_is_adv_report(packet, packet_size)) {
        /* Best-effort reports never backpressure the backend: shedding here
           keeps reliable HCI flowing behind them in the backend queue. */
        channel = WLH_CHANNEL_BLUETOOTH_HCI_ADV;
        if (coproc->tx_credit[WLH_CHANNEL_BLUETOOTH_HCI_ADV] <=
            coproc->bluetooth_adv_tx_inflight) {
            ++coproc->diagnostics.hci_adv_drops;
            coproc->config.osal.mutex_unlock(
                coproc->config.osal.context, &coproc->state_mutex
            );
            return WLH_COPROC_OK;
        }
        ++coproc->bluetooth_adv_tx_inflight;
    } else {
        if (coproc->tx_credit[WLH_CHANNEL_BLUETOOTH_HCI] <=
            coproc->bluetooth_tx_inflight) {
            coproc->config.osal.mutex_unlock(
                coproc->config.osal.context, &coproc->state_mutex
            );
            return WLH_COPROC_NO_CREDIT;
        }
        ++coproc->bluetooth_tx_inflight;
    }
    coproc->config.osal.mutex_unlock(
        coproc->config.osal.context, &coproc->state_mutex
    );

    job = (coproc_data_job_t *)coproc->config.buffers.alloc(
        coproc->config.buffers.context,
        sizeof(*job) + RAW_HEADER_SIZE + packet_size
    );
    if (job == NULL) {
        bluetooth_release_tx_reservation(coproc, channel);
        return WLH_COPROC_BACKEND_ERROR;
    }
    memset(job, 0, sizeof(*job));
    job->channel = channel;
    {
        size_t record_size = 0;
        if (wlh_raw_record_encode(
                job->data,
                RAW_HEADER_SIZE + packet_size,
                &record_size,
                h4_type,
                0u,
                packet,
                packet_size
            ) != WLH_WIRE_OK) {
            coproc->config.buffers.free(
                coproc->config.buffers.context, (uint8_t *)job
            );
            bluetooth_release_tx_reservation(coproc, channel);
            return WLH_COPROC_INVALID_ARGUMENT;
        }
        job->size = record_size;
    }
    if (enqueue_job(coproc, COPROC_JOB_BLUETOOTH_TX, job, WLH_OSAL_NO_WAIT) !=
        0) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)job
        );
        bluetooth_release_tx_reservation(coproc, channel);
        return WLH_COPROC_BACKEND_ERROR;
    }
    return WLH_COPROC_OK;
}

wlh_coproc_result_t wlh_coproc_bluetooth_fatal_error(
    wlh_coproc_t *coproc, uint32_t reason
) {
    coproc_bluetooth_fatal_job_t *job;
    if (coproc == NULL || !coproc->worker_started)
        return WLH_COPROC_INVALID_ARGUMENT;
    job = (coproc_bluetooth_fatal_job_t *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*job)
    );
    if (job == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    job->reason = reason;
    if (enqueue_job(
            coproc, COPROC_JOB_BLUETOOTH_FATAL, job, WLH_OSAL_NO_WAIT
        ) != 0) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)job
        );
        return WLH_COPROC_BACKEND_ERROR;
    }
    return WLH_COPROC_OK;
}

void wlh_coproc_get_diagnostics(
    const wlh_coproc_t *coproc, wlh_coproc_diagnostics_t *diagnostics
) {
    if (coproc != NULL && diagnostics != NULL) {
        if (coproc->worker_started)
            (void)coproc->config.osal.mutex_lock(
                coproc->config.osal.context,
                (wlh_osal_mutex_t *)&coproc->state_mutex,
                WLH_OSAL_WAIT_FOREVER
            );
        *diagnostics = coproc->diagnostics;
        diagnostics->state = coproc->state;
        diagnostics->session_id = coproc->session_id;
        if (coproc->worker_started)
            coproc->config.osal.mutex_unlock(
                coproc->config.osal.context,
                (wlh_osal_mutex_t *)&coproc->state_mutex
            );
    }
}

#ifdef WLH_ENABLE_TEST_HOOKS
void wlh_coproc_test_set_credit(
    wlh_coproc_t *coproc, uint8_t channel, uint32_t credit
) {
    if (coproc != NULL && coproc->worker_started) {
        (void)coproc->config.osal.mutex_lock(
            coproc->config.osal.context,
            &coproc->state_mutex,
            WLH_OSAL_WAIT_FOREVER
        );
        coproc->tx_credit[channel] = credit;
        coproc->config.osal.mutex_unlock(
            coproc->config.osal.context, &coproc->state_mutex
        );
    }
}

void wlh_coproc_test_reset_channel(wlh_coproc_t *coproc, uint8_t channel) {
    if (coproc != NULL && coproc->worker_started) {
        (void)coproc->config.osal.mutex_lock(
            coproc->config.osal.context,
            &coproc->state_mutex,
            WLH_OSAL_WAIT_FOREVER
        );
        coproc->tx_sequence[channel] = 0u;
        coproc->rx_sequence[channel] = 0u;
        coproc->rx_sequence_valid[channel] = false;
        coproc->config.osal.mutex_unlock(
            coproc->config.osal.context, &coproc->state_mutex
        );
    }
}

void wlh_coproc_test_reset_session(wlh_coproc_t *coproc, uint32_t reason) {
    if (coproc != NULL && coproc->worker_started) {
        uint8_t payload[64];
        size_t payload_size = 0;
        wlh_protocol_v1_SessionChangedEvent event =
            wlh_protocol_v1_SessionChangedEvent_init_zero;

        (void)coproc->config.osal.mutex_lock(
            coproc->config.osal.context,
            &coproc->state_mutex,
            WLH_OSAL_WAIT_FOREVER
        );

        event.old_session_id = coproc->session_id;
        event.new_session_id = coproc->next_session_id;
        event.reset_reason = reason;
        event.boot_id = coproc->next_session_id;

        if (coproc->state == WLH_COPROC_STATE_READY &&
            encode_message(
                payload,
                sizeof(payload),
                &payload_size,
                wlh_protocol_v1_SessionChangedEvent_fields,
                &event
            )) {
            (void)send_rpc(
                coproc,
                WLH_SERVICE_LINK,
                WLH_LINK_EVENT_SESSION_CHANGED,
                0u,
                WLH_RPC_KIND_EVENT,
                0u,
                0,
                payload,
                payload_size
            );
        }

        ++coproc->diagnostics.peer_resets;
        coproc->session_id = 0u;
        memset(
            &coproc->bluetooth_pending, 0, sizeof(coproc->bluetooth_pending)
        );
        coproc->bluetooth_state = BT_STATE_UNSPECIFIED;
        coproc->bluetooth_tx_inflight = 0u;
        coproc->bluetooth_hci_stopped = false;
        set_state(coproc, WLH_COPROC_STATE_WAITING_FOR_HELLO);
        coproc->config.osal.mutex_unlock(
            coproc->config.osal.context, &coproc->state_mutex
        );
    }
}
#endif
