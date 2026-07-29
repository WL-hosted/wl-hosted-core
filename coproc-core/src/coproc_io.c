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

static bool io_mode_valid(uint32_t mode) {
    return mode == WLH_COPROC_IO_MODE_INPUT ||
           mode == WLH_COPROC_IO_MODE_OUTPUT ||
           mode == WLH_COPROC_IO_MODE_OPEN_DRAIN;
}

static bool io_pull_valid(uint32_t pull) {
    return pull == WLH_COPROC_IO_PULL_NONE || pull == WLH_COPROC_IO_PULL_UP ||
           pull == WLH_COPROC_IO_PULL_DOWN;
}

WLH_NOINLINE wlh_coproc_result_t handle_io_request(
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
