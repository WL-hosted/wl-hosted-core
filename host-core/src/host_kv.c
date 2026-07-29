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

/* Non-empty, within the negotiated bound, and NUL-terminated by the caller. */
static bool kv_key_valid(const char *key) {
    return key != NULL && key[0] != '\0' &&
           strlen(key) <= WLH_HOST_MAX_KV_KEY_SIZE;
}

typedef struct wlh_kv_read_request {
    wlh_host_t *host;
    wlh_host_kv_read_fn completion;
    void *context;
} wlh_kv_read_request_t;

static void kv_read_completion(
    void *context,
    wlh_host_result_t result,
    uint16_t status_domain,
    int16_t status_code,
    const uint8_t *payload,
    size_t payload_size
) {
    wlh_kv_read_request_t *request = context;
    wlh_host_t *host = request->host;
    wlh_protocol_v1_KvReadResponse *message = NULL;
    const char *value = NULL;
    size_t value_size = 0u;

    if (result == WLH_HOST_OK) {
        message = (wlh_protocol_v1_KvReadResponse *)host->config.buffers.alloc(
            host->config.buffers.context, sizeof(*message)
        );
        if (message == NULL) {
            result = WLH_HOST_NO_MEMORY;
        } else {
            pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
            memset(message, 0, sizeof(*message));
            if (pb_decode(
                    &stream, wlh_protocol_v1_KvReadResponse_fields, message
                )) {
                value = message->value;
                value_size = strlen(message->value);
            } else {
                result = WLH_HOST_PROTOCOL_ERROR;
            }
        }
    }
    request->completion(
        request->context, result, status_domain, status_code, value, value_size
    );
    if (message != NULL)
        host->config.buffers.free(
            host->config.buffers.context, (uint8_t *)message
        );
    host->config.buffers.free(host->config.buffers.context, (uint8_t *)request);
}

wlh_host_result_t wlh_host_kv_read(
    wlh_host_t *host,
    const char *key,
    wlh_host_kv_read_fn completion,
    void *context
) {
    wlh_protocol_v1_KvReadRequest *message;
    wlh_kv_read_request_t *request;
    wlh_host_result_t result;

    if (host == NULL || completion == NULL || !kv_key_valid(key))
        return WLH_HOST_INVALID_ARGUMENT;
    request = (wlh_kv_read_request_t *)host->config.buffers.alloc(
        host->config.buffers.context, sizeof(*request)
    );
    if (request == NULL)
        return WLH_HOST_NO_MEMORY;
    message = (wlh_protocol_v1_KvReadRequest *)host->config.buffers.alloc(
        host->config.buffers.context, sizeof(*message)
    );
    if (message == NULL) {
        host->config.buffers.free(
            host->config.buffers.context, (uint8_t *)request
        );
        return WLH_HOST_NO_MEMORY;
    }
    request->host = host;
    request->completion = completion;
    request->context = context;
    memset(message, 0, sizeof(*message));
    memcpy(message->key, key, strlen(key));

    result = rpc_message_request(
        host,
        WLH_SERVICE_KV,
        WLH_KV_METHOD_READ,
        wlh_protocol_v1_KvReadRequest_fields,
        message,
        kv_read_completion,
        request
    );
    host->config.buffers.free(host->config.buffers.context, (uint8_t *)message);
    if (result != WLH_HOST_OK)
        host->config.buffers.free(
            host->config.buffers.context, (uint8_t *)request
        );
    return result;
}

wlh_host_result_t wlh_host_kv_write(
    wlh_host_t *host,
    const char *key,
    const char *value,
    wlh_rpc_completion_fn completion,
    void *context
) {
    wlh_protocol_v1_KvWriteRequest *message;
    wlh_host_result_t result;

    if (host == NULL || !kv_key_valid(key) || value == NULL ||
        strlen(value) > WLH_HOST_MAX_KV_VALUE_SIZE)
        return WLH_HOST_INVALID_ARGUMENT;
    message = (wlh_protocol_v1_KvWriteRequest *)host->config.buffers.alloc(
        host->config.buffers.context, sizeof(*message)
    );
    if (message == NULL)
        return WLH_HOST_NO_MEMORY;
    memset(message, 0, sizeof(*message));
    memcpy(message->key, key, strlen(key));
    memcpy(message->value, value, strlen(value));

    result = rpc_message_request(
        host,
        WLH_SERVICE_KV,
        WLH_KV_METHOD_WRITE,
        wlh_protocol_v1_KvWriteRequest_fields,
        message,
        completion,
        context
    );
    host->config.buffers.free(host->config.buffers.context, (uint8_t *)message);
    return result;
}

wlh_host_result_t wlh_host_kv_erase(
    wlh_host_t *host,
    const char *key,
    wlh_rpc_completion_fn completion,
    void *context
) {
    wlh_protocol_v1_KvEraseRequest *message;
    wlh_host_result_t result;

    if (host == NULL || !kv_key_valid(key))
        return WLH_HOST_INVALID_ARGUMENT;
    message = (wlh_protocol_v1_KvEraseRequest *)host->config.buffers.alloc(
        host->config.buffers.context, sizeof(*message)
    );
    if (message == NULL)
        return WLH_HOST_NO_MEMORY;
    memset(message, 0, sizeof(*message));
    memcpy(message->key, key, strlen(key));

    result = rpc_message_request(
        host,
        WLH_SERVICE_KV,
        WLH_KV_METHOD_ERASE,
        wlh_protocol_v1_KvEraseRequest_fields,
        message,
        completion,
        context
    );
    host->config.buffers.free(host->config.buffers.context, (uint8_t *)message);
    return result;
}
