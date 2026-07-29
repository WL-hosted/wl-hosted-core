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

WLH_NOINLINE wlh_coproc_result_t handle_user_message_request(
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
