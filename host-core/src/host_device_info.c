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

typedef struct wlh_device_info_request {
    wlh_host_t *host;
    wlh_host_device_info_fn completion;
    void *context;
} wlh_device_info_request_t;

static void device_info_completion(
    void *context,
    wlh_host_result_t result,
    uint16_t status_domain,
    int16_t status_code,
    const uint8_t *payload,
    size_t payload_size
) {
    wlh_device_info_request_t *request = context;
    wlh_host_t *host = request->host;
    wlh_host_device_info_t info;
    const wlh_host_device_info_t *decoded = NULL;

    if (result == WLH_HOST_OK) {
        wlh_protocol_v1_DeviceInfoResponse message =
            wlh_protocol_v1_DeviceInfoResponse_init_zero;
        pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
        if (pb_decode(
                &stream, wlh_protocol_v1_DeviceInfoResponse_fields, &message
            )) {
            memset(&info, 0, sizeof(info));
            memcpy(info.vendor, message.vendor, sizeof(info.vendor) - 1u);
            memcpy(
                info.mcu_model, message.mcu_model, sizeof(info.mcu_model) - 1u
            );
            memcpy(
                info.board_profile,
                message.board_profile,
                sizeof(info.board_profile) - 1u
            );
            info.uid_size = message.uid.size;
            memcpy(info.uid, message.uid.bytes, message.uid.size);
            decoded = &info;
        } else {
            result = WLH_HOST_PROTOCOL_ERROR;
        }
    }
    request->completion(
        request->context, result, status_domain, status_code, decoded
    );
    host->config.buffers.free(host->config.buffers.context, (uint8_t *)request);
}

wlh_host_result_t wlh_host_get_device_info(
    wlh_host_t *host, wlh_host_device_info_fn completion, void *context
) {
    wlh_device_info_request_t *request;
    wlh_host_result_t result;
    wlh_protocol_v1_Empty message = wlh_protocol_v1_Empty_init_zero;

    if (host == NULL || completion == NULL || !host->worker_started)
        return WLH_HOST_INVALID_ARGUMENT;
    request = (wlh_device_info_request_t *)host->config.buffers.alloc(
        host->config.buffers.context, sizeof(*request)
    );
    if (request == NULL)
        return WLH_HOST_NO_MEMORY;
    request->host = host;
    request->completion = completion;
    request->context = context;

    result = rpc_message_request(
        host,
        WLH_SERVICE_DEVICE_INFO,
        WLH_DEVICE_INFO_METHOD_GET_INFO,
        wlh_protocol_v1_Empty_fields,
        &message,
        device_info_completion,
        request
    );
    if (result != WLH_HOST_OK) {
        host->config.buffers.free(
            host->config.buffers.context, (uint8_t *)request
        );
    }
    return result;
}
