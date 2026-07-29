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

/* One allocation covers whichever KV message the method needs; the READ path
   copies its key out before the buffer is reused for the response. */
typedef union kv_message {
    wlh_protocol_v1_KvReadRequest read;
    wlh_protocol_v1_KvReadResponse response;
    wlh_protocol_v1_KvWriteRequest write;
    wlh_protocol_v1_KvEraseRequest erase;
} kv_message_t;

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

WLH_NOINLINE wlh_coproc_result_t handle_kv_request(
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
