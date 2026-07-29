#include "host_internal.h"

#include <string.h>

#include "adc.pb.h"
#include "bluetooth.pb.h"
#include "common.pb.h"
#include "device_info.pb.h"
#include "io.pb.h"
#include "kv.pb.h"
#include "user_passthrough.pb.h"
#include "wifi.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>

wlh_host_result_t wlh_host_user_message_send(
    wlh_host_t *host,
    uint32_t endpoint_id,
    uint32_t message_type,
    uint32_t flags,
    const uint8_t *payload,
    size_t payload_size,
    wlh_rpc_completion_fn completion,
    void *context
) {
    wlh_protocol_v1_UserMessageSendRequest *request;
    wlh_host_result_t result;

    if (host == NULL || endpoint_id == 0u ||
        payload_size > WLH_HOST_MAX_USER_PAYLOAD_SIZE ||
        (payload_size != 0u && payload == NULL))
        return WLH_HOST_INVALID_ARGUMENT;

    request =
        (wlh_protocol_v1_UserMessageSendRequest *)host->config.buffers.alloc(
            host->config.buffers.context, sizeof(*request)
        );
    if (request == NULL)
        return WLH_HOST_NO_MEMORY;
    memset(request, 0, sizeof(*request));
    request->endpoint_id = endpoint_id;
    request->message_type = message_type;
    request->flags = flags;
    request->payload.size = payload_size;
    if (payload_size != 0u)
        memcpy(request->payload.bytes, payload, payload_size);

    result = rpc_message_request(
        host,
        WLH_SERVICE_USER_PASSTHROUGH,
        WLH_USER_PASSTHROUGH_METHOD_SEND,
        wlh_protocol_v1_UserMessageSendRequest_fields,
        request,
        completion,
        context
    );
    host->config.buffers.free(host->config.buffers.context, (uint8_t *)request);
    return result;
}
