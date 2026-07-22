#include "wlh/coproc.h"
#include "wlh/log.h"

#include <limits.h>
#include <string.h>

#include "device_info.pb.h"
#include "diagnostics.pb.h"
#include "link.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "user_passthrough.pb.h"
#include "wifi.pb.h"

#define RPC_BUFFER_SIZE 1536u
#define RAW_HEADER_SIZE 8u

typedef enum coproc_job_kind {
    COPROC_JOB_STOP = 1,
    COPROC_JOB_RX_FRAME,
    COPROC_JOB_WIFI_INITIALIZED,
    COPROC_JOB_RPC_EVENT,
    COPROC_JOB_ETHERNET_TX,
    COPROC_JOB_TRANSPORT_FAILED
} coproc_job_kind_t;

typedef struct coproc_job {
    uint32_t kind;
    void *payload;
} coproc_job_t;

typedef struct coproc_data_job {
    uint16_t method_id;
    uint16_t service_id;
    size_t size;
    uint8_t data[];
} coproc_data_job_t;

typedef struct coproc_wifi_initialized_job {
    uint32_t operation_id;
    int backend_status;
} coproc_wifi_initialized_job_t;

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

static void log_state_transition(wlh_coproc_state_t previous, wlh_coproc_state_t state) {
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
static wlh_coproc_result_t send_rpc(
    wlh_coproc_t *coproc,
    uint16_t service_id, uint16_t method_id, uint32_t request_id, uint8_t kind,
    uint16_t status_domain, int16_t status_code,
    const uint8_t *payload, size_t payload_size) {
    // clang-format on
    uint8_t rpc[RPC_BUFFER_SIZE];
    wlh_rpc_envelope_t envelope;
    size_t rpc_size = 0;

    memset(&envelope, 0, sizeof(envelope));
    envelope.service_id = service_id;
    envelope.method_id = method_id;
    envelope.request_id = request_id;
    envelope.kind = kind;
    envelope.status_domain = status_domain;
    envelope.status_code = status_code;

    if (wlh_rpc_encode(
            rpc, sizeof(rpc), &rpc_size, &envelope, payload, payload_size
        ) != WLH_WIRE_OK) {
        return WLH_COPROC_PROTOCOL_ERROR;
    }
    return send_payload(
        coproc,
        service_id == WLH_SERVICE_LINK ? WLH_CHANNEL_LINK_CONTROL
                                       : WLH_CHANNEL_CONTROL_RPC,
        rpc,
        rpc_size
    );
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

static wlh_coproc_result_t send_hello_response(
    wlh_coproc_t *coproc, uint32_t request_id
) {
    uint8_t payload[RPC_BUFFER_SIZE];
    size_t payload_size = 0;
    size_t i;
    uint32_t selected_session;
    wlh_coproc_result_t result;
    wlh_protocol_v1_HelloResponse response =
        wlh_protocol_v1_HelloResponse_init_zero;

    selected_session = coproc->next_session_id++;
    if (selected_session == 0u) {
        selected_session = coproc->next_session_id++;
    }

    response.has_selected_protocol = true;
    response.selected_protocol.major = 1u;
    response.session_id = selected_session;
    response.boot_id = selected_session;
    memcpy(
        response.implementation, "wlh-coproc-core", sizeof("wlh-coproc-core")
    );
    memcpy(response.implementation_version, "0.1.0", sizeof("0.1.0"));
    response.max_frame_size = coproc->config.max_frame_size;
    response.alignment = 1u;
    response.checksum_mode = wlh_protocol_v1_ChecksumMode_CHECKSUM_MODE_SUM32;
    response.initial_credits_count = 3u;

    for (i = 0; i < 3u; ++i) {
        response.initial_credits[i].channel_id = (uint32_t)i;
        response.initial_credits[i].units = coproc->config.initial_credit;
        response.initial_credits[i].unit_bytes = 1u;
        coproc->tx_credit[i] = coproc->config.initial_credit;
    }

    if (!encode_message(
            payload,
            sizeof(payload),
            &payload_size,
            wlh_protocol_v1_HelloResponse_fields,
            &response
        )) {
        return WLH_COPROC_PROTOCOL_ERROR;
    }

    /* Negotiation frames use session 0. The selected session takes effect only
       after the complete HelloResponse has been sent. */
    coproc->session_id = 0u;
    result = send_rpc(
        coproc,
        WLH_SERVICE_LINK,
        WLH_LINK_METHOD_HELLO,
        request_id,
        WLH_RPC_KIND_RESPONSE,
        WLH_STATUS_DOMAIN_NONE,
        WLH_STATUS_OK,
        payload,
        payload_size
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

static wlh_coproc_result_t handle_wifi(
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
        WLH_LOGI("wlh_coproc", "wifi initialize request %lu", (unsigned long)request->request_id);
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
                           coproc->config.wifi.context, operation_id
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
        wlh_protocol_v1_WifiScanRequest message =
            wlh_protocol_v1_WifiScanRequest_init_zero;
        stream = pb_istream_from_buffer(payload, payload_size);
        if (!pb_decode(
                &stream, wlh_protocol_v1_WifiScanRequest_fields, &message
            )) {
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        WLH_LOGI(
            "wlh_coproc",
            "wifi scan start request %lu scan_id=%lu",
            (unsigned long)request->request_id,
            (unsigned long)message.scan_id
        );
        status = coproc->config.wifi.scan != NULL
                     ? coproc->config.wifi.scan(
                           coproc->config.wifi.context, message.scan_id
                       )
                     : -1;
        break;
    }

    case WLH_WIFI_METHOD_CONNECT: {
        wlh_protocol_v1_WifiConnectRequest message =
            wlh_protocol_v1_WifiConnectRequest_init_zero;
        wlh_coproc_wifi_connect_t connect;
        stream = pb_istream_from_buffer(payload, payload_size);
        if (!pb_decode(
                &stream, wlh_protocol_v1_WifiConnectRequest_fields, &message
            )) {
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        memset(&connect, 0, sizeof(connect));
        connect.ssid_size = message.ssid.size;
        connect.credential_size = message.credential.size;
        memcpy(connect.ssid, message.ssid.bytes, connect.ssid_size);
        memcpy(
            connect.credential,
            message.credential.bytes,
            connect.credential_size
        );
        connect.security = (uint32_t)message.security;
        WLH_LOGI(
            "wlh_coproc",
            "wifi connect request %lu ssid_size=%zu security=%lu",
            (unsigned long)request->request_id,
            connect.ssid_size,
            (unsigned long)connect.security
        );
        status = coproc->config.wifi.connect != NULL
                     ? coproc->config.wifi.connect(
                           coproc->config.wifi.context, &connect
                       )
                     : -1;
        break;
    }

    case WLH_WIFI_METHOD_DISCONNECT:
        WLH_LOGI("wlh_coproc", "wifi disconnect request %lu", (unsigned long)request->request_id);
        status =
            coproc->config.wifi.disconnect != NULL
                ? coproc->config.wifi.disconnect(coproc->config.wifi.context)
                : -1;
        break;

    case WLH_WIFI_METHOD_START_AP: {
        wlh_protocol_v1_WifiStartApRequest message =
            wlh_protocol_v1_WifiStartApRequest_init_zero;
        wlh_coproc_wifi_ap_t ap;
        stream = pb_istream_from_buffer(payload, payload_size);
        if (!pb_decode(
                &stream, wlh_protocol_v1_WifiStartApRequest_fields, &message
            )) {
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        memset(&ap, 0, sizeof(ap));
        ap.ssid_size = message.ssid.size;
        ap.credential_size = message.credential.size;
        memcpy(ap.ssid, message.ssid.bytes, ap.ssid_size);
        memcpy(ap.credential, message.credential.bytes, ap.credential_size);
        ap.security = (uint32_t)message.security;
        ap.channel = message.channel;
        ap.max_clients = message.max_clients;
        WLH_LOGI(
            "wlh_coproc",
            "wifi start_ap request %lu channel=%lu max_clients=%lu",
            (unsigned long)request->request_id,
            (unsigned long)ap.channel,
            (unsigned long)ap.max_clients
        );
        status =
            coproc->config.wifi.start_ap != NULL
                ? coproc->config.wifi.start_ap(coproc->config.wifi.context, &ap)
                : -1;
        break;
    }

    case WLH_WIFI_METHOD_STOP_AP:
        WLH_LOGI("wlh_coproc", "wifi stop_ap request %lu", (unsigned long)request->request_id);
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

static wlh_coproc_result_t handle_rpc(
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
        coproc->tx_credit[update.channel_id] += update.units;
        if (coproc->state == WLH_COPROC_STATE_CONGESTED) {
            set_state(coproc, WLH_COPROC_STATE_READY);
        }
        return request.kind == WLH_RPC_KIND_REQUEST
                   ? send_status(coproc, &request, 0)
                   : WLH_COPROC_OK;
    }

    if (request.service_id == WLH_SERVICE_LINK &&
        request.method_id == WLH_LINK_METHOD_HEARTBEAT) {
        wlh_protocol_v1_Heartbeat heartbeat =
            wlh_protocol_v1_Heartbeat_init_zero;
        pb_istream_t stream = pb_istream_from_buffer(message, message_size);
        if (frame_header->session_id != coproc->session_id ||
            !pb_decode(&stream, wlh_protocol_v1_Heartbeat_fields, &heartbeat) ||
            heartbeat.session_id != coproc->session_id) {
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
        wlh_protocol_v1_HelloRequest hello =
            wlh_protocol_v1_HelloRequest_init_zero;
        pb_istream_t stream = pb_istream_from_buffer(message, message_size);
        size_t i;
        bool supports_v1 = false;
        if (frame_header->session_id != 0u ||
            !pb_decode(&stream, wlh_protocol_v1_HelloRequest_fields, &hello)) {
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        for (i = 0; i < hello.protocol_versions_count; ++i) {
            if (hello.protocol_versions[i].major == 1u)
                supports_v1 = true;
        }
        if (!supports_v1 || hello.max_frame_size < WLH_FRAME_HEADER_SIZE) {
            return send_rpc(
                coproc,
                WLH_SERVICE_LINK,
                WLH_LINK_METHOD_HELLO,
                request.request_id,
                WLH_RPC_KIND_RESPONSE,
                WLH_STATUS_DOMAIN_PROTOCOL,
                WLH_STATUS_VERSION_MISMATCH,
                NULL,
                0u
            );
        }
        memset(coproc->tx_sequence, 0, sizeof(coproc->tx_sequence));
        memset(coproc->rx_sequence_valid, 0, sizeof(coproc->rx_sequence_valid));
        memset(
            &coproc->wifi_initialize_pending,
            0,
            sizeof(coproc->wifi_initialize_pending)
        );
        return send_hello_response(coproc, request.request_id);
    }

    if (coproc->state != WLH_COPROC_STATE_READY ||
        frame_header->session_id != coproc->session_id) {
        return WLH_COPROC_INVALID_STATE;
    }

    if (request.service_id == WLH_SERVICE_WIFI) {
        return handle_wifi(coproc, &request, message, message_size);
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
        uint8_t response_data[wlh_protocol_v1_DeviceInfoResponse_size];
        size_t response_size = 0;
        int status;
        wlh_coproc_device_info_t info;
        wlh_protocol_v1_DeviceInfoResponse response =
            wlh_protocol_v1_DeviceInfoResponse_init_zero;

        memset(&info, 0, sizeof(info));
        status = coproc->config.device_info.get_info(
            coproc->config.device_info.context, &info
        );
        if (status != 0) {
            return send_rpc(
                coproc,
                request.service_id,
                request.method_id,
                request.request_id,
                WLH_RPC_KIND_RESPONSE,
                WLH_STATUS_DOMAIN_DEVICE_INFO,
                WLH_STATUS_INTERNAL,
                NULL,
                0u
            );
        }
        /* The adapter contract is C strings; enforce termination and bounds
         * before handing the fields to nanopb. */
        info.vendor[sizeof(info.vendor) - 1u] = '\0';
        info.mcu_model[sizeof(info.mcu_model) - 1u] = '\0';
        info.board_profile[sizeof(info.board_profile) - 1u] = '\0';
        if (info.uid_size > sizeof(info.uid))
            info.uid_size = sizeof(info.uid);

        memcpy(response.vendor, info.vendor, sizeof(response.vendor) - 1u);
        memcpy(
            response.mcu_model, info.mcu_model, sizeof(response.mcu_model) - 1u
        );
        memcpy(
            response.board_profile,
            info.board_profile,
            sizeof(response.board_profile) - 1u
        );
        response.uid.size = info.uid_size;
        memcpy(response.uid.bytes, info.uid, info.uid_size);
        if (!encode_message(
                response_data,
                sizeof(response_data),
                &response_size,
                wlh_protocol_v1_DeviceInfoResponse_fields,
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

    if (request.service_id == WLH_SERVICE_USER_PASSTHROUGH &&
        request.method_id == WLH_USER_PASSTHROUGH_METHOD_SEND &&
        coproc->config.user_passthrough.on_message != NULL) {
        int status;
        wlh_coproc_user_message_t user_message;
        wlh_protocol_v1_UserMessageSendRequest send_request =
            wlh_protocol_v1_UserMessageSendRequest_init_zero;
        pb_istream_t stream = pb_istream_from_buffer(message, message_size);
        if (!pb_decode(
                &stream,
                wlh_protocol_v1_UserMessageSendRequest_fields,
                &send_request
            )) {
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        memset(&user_message, 0, sizeof(user_message));
        user_message.endpoint_id = send_request.endpoint_id;
        user_message.message_type = send_request.message_type;
        user_message.flags = send_request.flags;
        user_message.payload = send_request.payload.bytes;
        user_message.payload_size = send_request.payload.size;
        user_message.request_id = request.request_id;
        status = coproc->config.user_passthrough.on_message(
            coproc->config.user_passthrough.context, &user_message
        );
        return send_rpc(
            coproc,
            request.service_id,
            request.method_id,
            request.request_id,
            WLH_RPC_KIND_RESPONSE,
            status == 0 ? WLH_STATUS_DOMAIN_NONE : WLH_STATUS_DOMAIN_USER,
            status == 0 ? WLH_STATUS_OK : WLH_STATUS_INTERNAL,
            NULL,
            0u
        );
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

static wlh_coproc_result_t coproc_emit_due_heartbeat(wlh_coproc_t *coproc);
static uint32_t coproc_next_wait_ms(const wlh_coproc_t *coproc);
static wlh_coproc_result_t process_frame(
    wlh_coproc_t *coproc, const uint8_t *frame, size_t size
);

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
                (void)process_frame(coproc, data->data, data->size);
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
                    coproc, WLH_CHANNEL_ETHERNET_STA, data->data, data->size
                );
                coproc->config.buffers.free(
                    coproc->config.buffers.context, (uint8_t *)data
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

static wlh_coproc_result_t coproc_emit_due_heartbeat(wlh_coproc_t *coproc) {
    uint8_t payload[128];
    size_t size = 0;
    uint64_t now;
    wlh_protocol_v1_Heartbeat heartbeat = wlh_protocol_v1_Heartbeat_init_zero;

    if (coproc == NULL) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }

    now = now_ms(coproc);
    if (coproc->state == WLH_COPROC_STATE_READY &&
        now - coproc->last_heartbeat_ms >=
            coproc->config.heartbeat_interval_ms) {
        heartbeat.session_id = coproc->session_id;
        heartbeat.state = wlh_protocol_v1_LinkState_LINK_STATE_HEALTHY;
        heartbeat.uptime_ms = now - coproc->started_ms;
        heartbeat.monotonic_ms = now;

        if (encode_message(
                payload,
                sizeof(payload),
                &size,
                wlh_protocol_v1_Heartbeat_fields,
                &heartbeat
            )) {
            (void)send_rpc(
                coproc,
                WLH_SERVICE_LINK,
                WLH_LINK_METHOD_HEARTBEAT,
                0u,
                WLH_RPC_KIND_EVENT,
                0u,
                0,
                payload,
                size
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

static wlh_coproc_result_t process_frame(
    wlh_coproc_t *coproc, const uint8_t *frame, size_t size
) {
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

    if (header.channel == WLH_CHANNEL_ETHERNET_STA &&
        payload_size >= RAW_HEADER_SIZE && payload[0] == 1u &&
        payload[2] == 8u && payload[3] == 0u) {
        uint32_t raw_size = (uint32_t)payload[4] | ((uint32_t)payload[5] << 8) |
                            ((uint32_t)payload[6] << 16) |
                            ((uint32_t)payload[7] << 24);
        if ((size_t)raw_size + RAW_HEADER_SIZE != payload_size) {
            return WLH_COPROC_PROTOCOL_ERROR;
        }

        if (coproc->config.port.ethernet_rx != NULL) {
            coproc->config.port.ethernet_rx(
                coproc->config.port.context, payload + RAW_HEADER_SIZE, raw_size
            );
        }
        return WLH_COPROC_OK;
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
    uint8_t payload[RPC_BUFFER_SIZE];
    size_t size = 0;
    coproc_data_job_t *job;

    if (!encode_message(payload, sizeof(payload), &size, fields, message)) {
        return WLH_COPROC_PROTOCOL_ERROR;
    }
    job = (coproc_data_job_t *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*job) + size
    );
    if (job == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    memset(job, 0, sizeof(*job));
    job->method_id = method_id;
    job->service_id = service_id;
    job->size = size;
    memcpy(job->data, payload, size);
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
    wlh_protocol_v1_WifiScanResultEvent event =
        wlh_protocol_v1_WifiScanResultEvent_init_zero;
    wlh_protocol_v1_WifiNetwork *network;

    if (coproc == NULL || bss == NULL || bss->ssid_size > 32u) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }

    event.scan_id = scan_id;
    event.networks_count = 1u;
    network = &event.networks[0];

    network->ssid.size = bss->ssid_size;
    memcpy(network->ssid.bytes, bss->ssid, bss->ssid_size);

    network->bssid.size = 6u;
    memcpy(network->bssid.bytes, bss->bssid, 6u);

    network->channel = bss->channel;
    network->rssi_dbm = bss->rssi_dbm;
    network->security = (wlh_protocol_v1_WifiSecurity)bss->security;
    return send_wifi_message(
        coproc,
        WLH_WIFI_EVENT_SCAN_RESULT,
        wlh_protocol_v1_WifiScanResultEvent_fields,
        &event
    );
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

    if (coproc == NULL || bss == NULL) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }

    event.has_link = true;
    event.link.interface = wlh_protocol_v1_WifiInterface_WIFI_INTERFACE_STA;
    event.link.connected = true;

    event.link.ssid.size = bss->ssid_size;
    memcpy(event.link.ssid.bytes, bss->ssid, bss->ssid_size);

    event.link.bssid.size = 6u;
    memcpy(event.link.bssid.bytes, bss->bssid, 6u);

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

wlh_coproc_result_t wlh_coproc_ethernet_sta_send(
    wlh_coproc_t *coproc, const uint8_t *frame, size_t size
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
    job->size = RAW_HEADER_SIZE + size;
    job->data[0] = 1u;
    job->data[2] = 8u;
    job->data[4] = (uint8_t)size;
    job->data[5] = (uint8_t)(size >> 8);
    job->data[6] = (uint8_t)(size >> 16);
    job->data[7] = (uint8_t)(size >> 24);
    memcpy(job->data + RAW_HEADER_SIZE, frame, size);
    if (enqueue_job(coproc, COPROC_JOB_ETHERNET_TX, job, WLH_OSAL_NO_WAIT) !=
        0) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)job
        );
        return WLH_COPROC_BACKEND_ERROR;
    }
    return WLH_COPROC_OK;
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
    wlh_protocol_v1_UserMessageResultEvent event =
        wlh_protocol_v1_UserMessageResultEvent_init_zero;

    if (coproc == NULL || payload_size > sizeof(event.payload.bytes) ||
        (payload == NULL && payload_size != 0u)) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }
    event.endpoint_id = endpoint_id;
    event.message_type = message_type;
    event.correlation_id = correlation_id;
    event.result = result;
    event.payload.size = payload_size;
    if (payload_size != 0u)
        memcpy(event.payload.bytes, payload, payload_size);
    return send_event_message(
        coproc,
        WLH_SERVICE_USER_PASSTHROUGH,
        WLH_USER_PASSTHROUGH_EVENT_RESULT,
        wlh_protocol_v1_UserMessageResultEvent_fields,
        &event
    );
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
        set_state(coproc, WLH_COPROC_STATE_WAITING_FOR_HELLO);
        coproc->config.osal.mutex_unlock(
            coproc->config.osal.context, &coproc->state_mutex
        );
    }
}
#endif
