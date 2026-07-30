#include "host_internal.h"
#include "wlh/log.h"

#include <limits.h>
#include <string.h>

#include "bluetooth.pb.h"
#include "link.pb.h"
#include "user_passthrough.pb.h"
#include "wifi.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>

static void tx_complete(
    void *completion_context, uint8_t *frame, size_t size, int status
) {
    wlh_host_t *host = completion_context;
    (void)size;
    host->config.buffers.free(host->config.buffers.context, frame);
    if (status != 0)
        wlh_host_transport_lost(host);
}

wlh_host_result_t encode_pb(
    uint8_t *output,
    size_t capacity,
    size_t *size,
    const pb_msgdesc_t *fields,
    const void *message
) {
    pb_ostream_t stream = pb_ostream_from_buffer(output, capacity);
    if (!pb_encode(&stream, fields, message)) {
        return WLH_HOST_PROTOCOL_ERROR;
    }
    *size = stream.bytes_written;
    return WLH_HOST_OK;
}

wlh_host_result_t send_payload_frame(
    wlh_host_t *host,
    uint8_t channel,
    const uint8_t *payload,
    size_t payload_size,
    bool reserved
) {
    wlh_frame_header_t header;
    uint8_t *frame;
    size_t frame_size = WLH_FRAME_HEADER_SIZE + payload_size;
    size_t encoded_size = 0u;
    if (frame_size > host->config.max_frame_size || payload_size > UINT16_MAX) {
        return WLH_HOST_INVALID_ARGUMENT;
    }
    if (!reserved && host->tx_credit[channel] == 0u) {
        set_state(host, WLH_HOST_STATE_CONGESTED);
        WLH_LOGW("wlh_host", "no credit on channel %u", (unsigned)channel);
        return WLH_HOST_NO_CREDIT;
    }
    frame =
        host->config.buffers.alloc(host->config.buffers.context, frame_size);
    if (frame == NULL) {
        host->diagnostics.buffer_allocation_failures++;
        WLH_LOGW(
            "wlh_host", "frame buffer allocation failed (%zu bytes)", frame_size
        );
        return WLH_HOST_NO_MEMORY;
    }
    wlh_frame_header_init(&header, channel);
    header.session_id =
        channel == WLH_CHANNEL_LINK_CONTROL && host->session_id == 0u
            ? 0u
            : host->session_id;
    header.sequence = host->tx_sequence[channel]++;
    if (wlh_frame_encode(
            frame, frame_size, &encoded_size, &header, payload, payload_size
        ) != WLH_WIRE_OK) {
        host->config.buffers.free(host->config.buffers.context, frame);
        return WLH_HOST_PROTOCOL_ERROR;
    }
    if (host->config.transport.submit_tx(
            host->config.transport.context,
            frame,
            encoded_size,
            tx_complete,
            host
        ) != 0) {
        host->config.buffers.free(host->config.buffers.context, frame);
        return WLH_HOST_TRANSPORT_ERROR;
    }
    if (!reserved) {
        host->tx_credit[channel]--;
    }
    host->diagnostics.tx_frames++;
    return WLH_HOST_OK;
}

wlh_host_result_t send_rpc(
    wlh_host_t *host,
    const wlh_rpc_envelope_t *envelope,
    const uint8_t *payload,
    size_t payload_size,
    bool reserved
) {
    uint8_t *rpc;
    wlh_host_result_t result;
    size_t rpc_capacity;
    size_t rpc_size = 0u;
    uint8_t channel = envelope->service_id == WLH_SERVICE_LINK
                          ? WLH_CHANNEL_LINK_CONTROL
                          : WLH_CHANNEL_CONTROL_RPC;

    if (payload_size > WLH_HOST_PROTOBUF_LIMIT)
        return WLH_HOST_INVALID_ARGUMENT;
    rpc_capacity = WLH_RPC_ENVELOPE_SIZE + payload_size;
    rpc =
        host->config.buffers.alloc(host->config.buffers.context, rpc_capacity);
    if (rpc == NULL)
        return WLH_HOST_NO_MEMORY;
    if (wlh_rpc_encode(
            rpc, rpc_capacity, &rpc_size, envelope, payload, payload_size
        ) != WLH_WIRE_OK) {
        host->config.buffers.free(host->config.buffers.context, rpc);
        return WLH_HOST_PROTOCOL_ERROR;
    }
    result = send_payload_frame(host, channel, rpc, rpc_size, reserved);
    host->config.buffers.free(host->config.buffers.context, rpc);
    return result;
}

wlh_host_result_t send_rpc_message(
    wlh_host_t *host,
    const wlh_rpc_envelope_t *envelope,
    const pb_msgdesc_t *fields,
    const void *message,
    bool reserved
) {
    uint8_t *payload;
    size_t payload_size = 0u;
    size_t encoded_size = 0u;
    wlh_host_result_t result;

    if (!pb_get_encoded_size(&payload_size, fields, message) ||
        payload_size > WLH_HOST_PROTOBUF_LIMIT) {
        return WLH_HOST_PROTOCOL_ERROR;
    }
    if (payload_size == 0u)
        return send_rpc(host, envelope, NULL, 0u, reserved);
    payload =
        host->config.buffers.alloc(host->config.buffers.context, payload_size);
    if (payload == NULL)
        return WLH_HOST_NO_MEMORY;
    result = encode_pb(payload, payload_size, &encoded_size, fields, message);
    if (result == WLH_HOST_OK && encoded_size == payload_size) {
        result = send_rpc(host, envelope, payload, payload_size, reserved);
    } else {
        result = WLH_HOST_PROTOCOL_ERROR;
    }
    host->config.buffers.free(host->config.buffers.context, payload);
    return result;
}

wlh_host_result_t send_credit_update(
    wlh_host_t *host, uint8_t channel, uint32_t units
) {
    wlh_rpc_envelope_t envelope;
    wlh_protocol_v1_CreditUpdate update =
        wlh_protocol_v1_CreditUpdate_init_zero;

    if (units == 0u)
        return WLH_HOST_OK;
    memset(&envelope, 0, sizeof(envelope));
    envelope.service_id = WLH_SERVICE_LINK;
    envelope.method_id = WLH_LINK_METHOD_CREDIT_UPDATE;
    envelope.kind = WLH_RPC_KIND_EVENT;
    update.channel_id = channel;
    update.units = units;
    return send_rpc_message(
        host, &envelope, wlh_protocol_v1_CreditUpdate_fields, &update, true
    );
}
