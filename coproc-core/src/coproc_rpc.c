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

WLH_NOINLINE wlh_coproc_result_t handle_rpc(
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
        if (update.channel_id == WLH_CHANNEL_BLUETOOTH_HCI &&
            coproc->config.bluetooth.hci_tx_ready != NULL &&
            coproc->tx_credit[update.channel_id] <=
                coproc->bluetooth_tx_inflight &&
            coproc->tx_credit[update.channel_id] + update.units >
                coproc->bluetooth_tx_inflight) {
            coproc->tx_credit[update.channel_id] += update.units;
            coproc->config.bluetooth.hci_tx_ready(
                coproc->config.bluetooth.context
            );
        } else {
            coproc->tx_credit[update.channel_id] += update.units;
        }
        if (coproc->state == WLH_COPROC_STATE_CONGESTED) {
            set_state(coproc, WLH_COPROC_STATE_READY);
        }
        return request.kind == WLH_RPC_KIND_REQUEST
                   ? send_status(coproc, &request, 0)
                   : WLH_COPROC_OK;
    }

    if (request.service_id == WLH_SERVICE_LINK &&
        request.method_id == WLH_LINK_METHOD_HEARTBEAT) {
        wlh_protocol_v1_Heartbeat *heartbeat;
        pb_istream_t stream = pb_istream_from_buffer(message, message_size);
        bool valid;
        heartbeat = (wlh_protocol_v1_Heartbeat *)coproc->config.buffers.alloc(
            coproc->config.buffers.context, sizeof(*heartbeat)
        );
        if (heartbeat == NULL)
            return WLH_COPROC_BACKEND_ERROR;
        memset(heartbeat, 0, sizeof(*heartbeat));
        valid =
            frame_header->session_id == coproc->session_id &&
            pb_decode(&stream, wlh_protocol_v1_Heartbeat_fields, heartbeat) &&
            heartbeat->session_id == coproc->session_id;
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)heartbeat
        );
        if (!valid) {
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
        return handle_hello_request(
            coproc, frame_header, &request, message, message_size
        );
    }

    if (coproc->state != WLH_COPROC_STATE_READY ||
        frame_header->session_id != coproc->session_id) {
        return WLH_COPROC_INVALID_STATE;
    }

    if (request.service_id == WLH_SERVICE_WIFI) {
        return handle_wifi(coproc, &request, message, message_size);
    }

    if (request.service_id == WLH_SERVICE_BLUETOOTH &&
        bluetooth_backend_present(coproc)) {
        return handle_bluetooth(coproc, &request, message, message_size);
    }

    if (request.service_id == WLH_SERVICE_OTA && ota_backend_present(coproc)) {
        return handle_ota(coproc, &request, message, message_size);
    }

    if (request.service_id == WLH_SERVICE_ETH && eth_backend_present(coproc)) {
        return handle_eth(coproc, &request, message, message_size);
    }

    if (request.service_id == WLH_SERVICE_DIAGNOSTICS &&
        request.method_id == WLH_DIAGNOSTICS_METHOD_PING) {
        return handle_diagnostics_request(
            coproc, &request, message, message_size
        );
    }

    if (request.service_id == WLH_SERVICE_DEVICE_INFO &&
        request.method_id == WLH_DEVICE_INFO_METHOD_GET_INFO &&
        coproc->config.device_info.get_info != NULL) {
        return handle_device_info_request(coproc, &request);
    }

    if (request.service_id == WLH_SERVICE_USER_PASSTHROUGH &&
        request.method_id == WLH_USER_PASSTHROUGH_METHOD_SEND &&
        coproc->config.user_passthrough.on_message != NULL) {
        return handle_user_message_request(
            coproc, &request, message, message_size
        );
    }

    /* Optional services answer only once an adapter has supplied a backend;
       otherwise the request falls through to NOT_SUPPORTED below. */
    if (request.service_id == WLH_SERVICE_IO &&
        coproc->config.io.configure != NULL) {
        return handle_io_request(coproc, &request, message, message_size);
    }

    if (request.service_id == WLH_SERVICE_ADC &&
        coproc->config.adc.read != NULL) {
        return handle_adc_request(coproc, &request, message, message_size);
    }

    if (request.service_id == WLH_SERVICE_KV &&
        coproc->config.kv.read != NULL) {
        return handle_kv_request(coproc, &request, message, message_size);
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

wlh_coproc_result_t send_event_message(
    wlh_coproc_t *coproc,
    uint16_t service_id,
    uint16_t method_id,
    const pb_msgdesc_t *fields,
    const void *message
) {
    size_t size = 0;
    size_t encoded_size = 0;
    coproc_data_job_t *job;

    if (!pb_get_encoded_size(&size, fields, message) ||
        size > RPC_BUFFER_SIZE) {
        return WLH_COPROC_PROTOCOL_ERROR;
    }
    job = (coproc_data_job_t *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*job) + size
    );
    if (job == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    memset(job, 0, sizeof(*job));
    if (!encode_message(job->data, size, &encoded_size, fields, message) ||
        encoded_size != size) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)job
        );
        return WLH_COPROC_PROTOCOL_ERROR;
    }
    job->method_id = method_id;
    job->service_id = service_id;
    job->size = size;
    if (enqueue_job(coproc, COPROC_JOB_RPC_EVENT, job, WLH_OSAL_NO_WAIT) != 0) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)job
        );
        return WLH_COPROC_BACKEND_ERROR;
    }
    return WLH_COPROC_OK;
}
