#include "coproc_internal.h"
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
#include "user_passthrough.pb.h"
#include "wifi.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>

static void tx_complete(
    void *completion_context, uint8_t *frame, size_t size, int status
) {
    wlh_coproc_t *coproc = completion_context;
    bool ethernet_frame = size >= WLH_FRAME_HEADER_SIZE &&
                          (frame[4] == WLH_CHANNEL_ETHERNET_STA ||
                           frame[4] == WLH_CHANNEL_ETHERNET_AP);
    coproc->config.buffers.free(coproc->config.buffers.context, frame);
    if (ethernet_frame) {
        (void)coproc->config.osal.semaphore_give(
            coproc->config.osal.context, &coproc->ethernet_tx_slots
        );
    }
    if (status != 0 && status != WLH_COPROC_TX_CANCELLED)
        (void)enqueue_job(
            coproc, COPROC_JOB_TRANSPORT_FAILED, NULL, WLH_OSAL_NO_WAIT
        );
}

wlh_coproc_result_t send_payload(
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
    header.sequence = coproc->tx_sequence[channel];
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

    /* The transport owns frame completion after a successful submission. Do
     * not consume a sequence number when admission failed synchronously: the
     * next accepted frame must remain contiguous on this reliable channel. */
    ++coproc->tx_sequence[channel];

    if (channel != WLH_CHANNEL_LINK_CONTROL) {
        --coproc->tx_credit[channel];
    }
    ++coproc->diagnostics.tx_frames;
    return WLH_COPROC_OK;
}

WLH_NOINLINE wlh_coproc_result_t send_rpc(
    wlh_coproc_t *coproc,
    uint16_t service_id,
    uint16_t method_id,
    uint32_t request_id,
    uint8_t kind,
    uint16_t status_domain,
    int16_t status_code,
    const uint8_t *payload,
    size_t payload_size
) {
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

bool encode_message(
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

WLH_NOINLINE wlh_coproc_result_t send_rpc_message(
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

wlh_coproc_result_t send_credit_update(
    wlh_coproc_t *coproc, uint8_t channel, uint32_t units
) {
    wlh_protocol_v1_CreditUpdate update =
        wlh_protocol_v1_CreditUpdate_init_zero;

    if (units == 0u)
        return WLH_COPROC_OK;
    update.channel_id = channel;
    update.units = units;
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

wlh_coproc_result_t send_status(
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

wlh_coproc_result_t send_service_error(
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
