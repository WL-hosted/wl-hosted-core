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

static bool io_mode_valid(uint32_t mode) {
    return mode == WLH_HOST_IO_MODE_INPUT || mode == WLH_HOST_IO_MODE_OUTPUT ||
           mode == WLH_HOST_IO_MODE_OPEN_DRAIN;
}

static bool io_pull_valid(uint32_t pull) {
    return pull == WLH_HOST_IO_PULL_NONE || pull == WLH_HOST_IO_PULL_UP ||
           pull == WLH_HOST_IO_PULL_DOWN;
}

wlh_host_result_t wlh_host_io_configure(
    wlh_host_t *host,
    const wlh_host_io_config_t *config,
    wlh_rpc_completion_fn completion,
    void *context
) {
    wlh_protocol_v1_IoConfigureRequest message =
        wlh_protocol_v1_IoConfigureRequest_init_zero;

    if (host == NULL || config == NULL ||
        !io_mode_valid((uint32_t)config->mode) ||
        !io_pull_valid((uint32_t)config->pull))
        return WLH_HOST_INVALID_ARGUMENT;
    message.pin_id = config->pin_id;
    message.mode = (wlh_protocol_v1_IoMode)config->mode;
    message.pull = (wlh_protocol_v1_IoPull)config->pull;
    message.initial_level = config->initial_level;
    return rpc_message_request(
        host,
        WLH_SERVICE_IO,
        WLH_IO_METHOD_CONFIGURE,
        wlh_protocol_v1_IoConfigureRequest_fields,
        &message,
        completion,
        context
    );
}

typedef struct wlh_io_read_request {
    wlh_host_t *host;
    wlh_host_io_read_fn completion;
    void *context;
} wlh_io_read_request_t;

static void io_read_completion(
    void *context,
    wlh_host_result_t result,
    uint16_t status_domain,
    int16_t status_code,
    const uint8_t *payload,
    size_t payload_size
) {
    wlh_io_read_request_t *request = context;
    wlh_host_t *host = request->host;
    wlh_host_io_state_t state;
    const wlh_host_io_state_t *decoded = NULL;

    if (result == WLH_HOST_OK) {
        wlh_protocol_v1_IoReadResponse message =
            wlh_protocol_v1_IoReadResponse_init_zero;
        pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
        if (pb_decode(
                &stream, wlh_protocol_v1_IoReadResponse_fields, &message
            ) &&
            io_mode_valid((uint32_t)message.mode) &&
            io_pull_valid((uint32_t)message.pull)) {
            memset(&state, 0, sizeof(state));
            state.pin_id = message.pin_id;
            state.level = message.level;
            state.mode = (wlh_host_io_mode_t)message.mode;
            state.pull = (wlh_host_io_pull_t)message.pull;
            decoded = &state;
        } else {
            result = WLH_HOST_PROTOCOL_ERROR;
        }
    }
    request->completion(
        request->context, result, status_domain, status_code, decoded
    );
    host->config.buffers.free(host->config.buffers.context, (uint8_t *)request);
}

wlh_host_result_t wlh_host_io_read(
    wlh_host_t *host,
    uint32_t pin_id,
    wlh_host_io_read_fn completion,
    void *context
) {
    wlh_protocol_v1_IoReadRequest message =
        wlh_protocol_v1_IoReadRequest_init_zero;
    wlh_io_read_request_t *request;
    wlh_host_result_t result;

    if (host == NULL || completion == NULL)
        return WLH_HOST_INVALID_ARGUMENT;
    request = (wlh_io_read_request_t *)host->config.buffers.alloc(
        host->config.buffers.context, sizeof(*request)
    );
    if (request == NULL)
        return WLH_HOST_NO_MEMORY;
    request->host = host;
    request->completion = completion;
    request->context = context;
    message.pin_id = pin_id;

    result = rpc_message_request(
        host,
        WLH_SERVICE_IO,
        WLH_IO_METHOD_READ,
        wlh_protocol_v1_IoReadRequest_fields,
        &message,
        io_read_completion,
        request
    );
    if (result != WLH_HOST_OK)
        host->config.buffers.free(
            host->config.buffers.context, (uint8_t *)request
        );
    return result;
}

wlh_host_result_t wlh_host_io_write(
    wlh_host_t *host,
    uint32_t pin_id,
    bool level,
    wlh_rpc_completion_fn completion,
    void *context
) {
    wlh_protocol_v1_IoWriteRequest message =
        wlh_protocol_v1_IoWriteRequest_init_zero;

    if (host == NULL)
        return WLH_HOST_INVALID_ARGUMENT;
    message.pin_id = pin_id;
    message.level = level;
    return rpc_message_request(
        host,
        WLH_SERVICE_IO,
        WLH_IO_METHOD_WRITE,
        wlh_protocol_v1_IoWriteRequest_fields,
        &message,
        completion,
        context
    );
}
