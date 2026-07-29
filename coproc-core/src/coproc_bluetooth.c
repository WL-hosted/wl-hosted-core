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

bool bluetooth_backend_present(const wlh_coproc_t *coproc) {
    return coproc->config.bluetooth.hci_send != NULL;
}

static wlh_coproc_result_t send_bluetooth_status(
    wlh_coproc_t *coproc,
    uint16_t method_id,
    uint32_t request_id,
    int16_t status_code
) {
    return send_rpc(
        coproc,
        WLH_SERVICE_BLUETOOTH,
        method_id,
        request_id,
        WLH_RPC_KIND_RESPONSE,
        status_code == WLH_STATUS_OK ? WLH_STATUS_DOMAIN_NONE
                                     : WLH_STATUS_DOMAIN_BLUETOOTH,
        status_code,
        NULL,
        0u
    );
}

void bluetooth_enter_error(wlh_coproc_t *coproc, uint32_t reason) {
    wlh_protocol_v1_BluetoothStateChangedEvent event =
        wlh_protocol_v1_BluetoothStateChangedEvent_init_zero;

    coproc->bluetooth_state = BT_STATE_ERROR;
    coproc->bluetooth_hci_stopped = true;
    event.state = (wlh_protocol_v1_BluetoothControllerState)BT_STATE_ERROR;
    event.reason = reason;
    (void)send_rpc_message(
        coproc,
        WLH_SERVICE_BLUETOOTH,
        WLH_BLUETOOTH_EVENT_STATE_CHANGED,
        0u,
        WLH_RPC_KIND_EVENT,
        WLH_STATUS_DOMAIN_NONE,
        WLH_STATUS_OK,
        wlh_protocol_v1_BluetoothStateChangedEvent_fields,
        &event
    );
}

static uint32_t bluetooth_begin_operation(
    wlh_coproc_t *coproc, const wlh_rpc_envelope_t *request
) {
    uint32_t operation_id = coproc->next_backend_operation_id++;
    if (operation_id == 0u)
        operation_id = coproc->next_backend_operation_id++;
    coproc->bluetooth_pending.active = true;
    coproc->bluetooth_pending.operation_id = operation_id;
    coproc->bluetooth_pending.session_id = coproc->session_id;
    coproc->bluetooth_pending.request_id = request->request_id;
    coproc->bluetooth_pending.method_id = request->method_id;
    return operation_id;
}

static wlh_coproc_result_t bluetooth_submit_result(
    wlh_coproc_t *coproc, const wlh_rpc_envelope_t *request, int status
) {
    if (status != 0) {
        memset(
            &coproc->bluetooth_pending, 0, sizeof(coproc->bluetooth_pending)
        );
        return send_bluetooth_status(
            coproc, request->method_id, request->request_id, WLH_STATUS_INTERNAL
        );
    }
    return WLH_COPROC_OK;
}

WLH_NOINLINE wlh_coproc_result_t handle_bluetooth(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    const uint8_t *payload,
    size_t payload_size
) {
    pb_istream_t stream;
    uint32_t operation_id;
    int status;

    switch (request->method_id) {
    case WLH_BLUETOOTH_METHOD_INITIALIZE: {
        wlh_protocol_v1_BluetoothInitializeRequest message =
            wlh_protocol_v1_BluetoothInitializeRequest_init_zero;
        stream = pb_istream_from_buffer(payload, payload_size);
        if (!pb_decode(
                &stream,
                wlh_protocol_v1_BluetoothInitializeRequest_fields,
                &message
            )) {
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        if (message.transport !=
            wlh_protocol_v1_BluetoothTransport_BLUETOOTH_TRANSPORT_HCI) {
            return send_bluetooth_status(
                coproc,
                request->method_id,
                request->request_id,
                WLH_STATUS_NOT_SUPPORTED
            );
        }
        if (coproc->bluetooth_pending.active) {
            return send_bluetooth_status(
                coproc, request->method_id, request->request_id, WLH_STATUS_BUSY
            );
        }
        if (coproc->bluetooth_state != BT_STATE_UNSPECIFIED) {
            return send_bluetooth_status(
                coproc, request->method_id, request->request_id, WLH_STATUS_OK
            );
        }
        operation_id = bluetooth_begin_operation(coproc, request);
        status = coproc->config.bluetooth.initialize(
            coproc->config.bluetooth.context,
            operation_id,
            message.feature_flags
        );
        return bluetooth_submit_result(coproc, request, status);
    }

    case WLH_BLUETOOTH_METHOD_ENABLE: {
        wlh_protocol_v1_BluetoothEnableRequest message =
            wlh_protocol_v1_BluetoothEnableRequest_init_zero;
        stream = pb_istream_from_buffer(payload, payload_size);
        if (!pb_decode(
                &stream, wlh_protocol_v1_BluetoothEnableRequest_fields, &message
            )) {
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        if (coproc->bluetooth_pending.active) {
            return send_bluetooth_status(
                coproc, request->method_id, request->request_id, WLH_STATUS_BUSY
            );
        }
        if (coproc->bluetooth_state == BT_STATE_UNSPECIFIED ||
            coproc->bluetooth_state == BT_STATE_ERROR) {
            return send_bluetooth_status(
                coproc,
                request->method_id,
                request->request_id,
                WLH_STATUS_NOT_READY
            );
        }
        if (coproc->bluetooth_state == BT_STATE_ENABLED) {
            return send_bluetooth_status(
                coproc, request->method_id, request->request_id, WLH_STATUS_OK
            );
        }
        operation_id = bluetooth_begin_operation(coproc, request);
        status = coproc->config.bluetooth.enable(
            coproc->config.bluetooth.context, operation_id, message.mode_flags
        );
        return bluetooth_submit_result(coproc, request, status);
    }

    case WLH_BLUETOOTH_METHOD_DISABLE: {
        if (coproc->bluetooth_pending.active) {
            return send_bluetooth_status(
                coproc, request->method_id, request->request_id, WLH_STATUS_BUSY
            );
        }
        if (coproc->bluetooth_state == BT_STATE_UNSPECIFIED ||
            coproc->bluetooth_state == BT_STATE_DISABLED) {
            return send_bluetooth_status(
                coproc, request->method_id, request->request_id, WLH_STATUS_OK
            );
        }
        operation_id = bluetooth_begin_operation(coproc, request);
        status = coproc->config.bluetooth.disable(
            coproc->config.bluetooth.context, operation_id
        );
        return bluetooth_submit_result(coproc, request, status);
    }

    case WLH_BLUETOOTH_METHOD_DEINITIALIZE: {
        wlh_protocol_v1_BluetoothDeinitializeRequest message =
            wlh_protocol_v1_BluetoothDeinitializeRequest_init_zero;
        stream = pb_istream_from_buffer(payload, payload_size);
        if (!pb_decode(
                &stream,
                wlh_protocol_v1_BluetoothDeinitializeRequest_fields,
                &message
            )) {
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        if (coproc->bluetooth_pending.active) {
            return send_bluetooth_status(
                coproc, request->method_id, request->request_id, WLH_STATUS_BUSY
            );
        }
        if (coproc->bluetooth_state == BT_STATE_UNSPECIFIED) {
            return send_bluetooth_status(
                coproc, request->method_id, request->request_id, WLH_STATUS_OK
            );
        }
        operation_id = bluetooth_begin_operation(coproc, request);
        status = coproc->config.bluetooth.deinitialize(
            coproc->config.bluetooth.context,
            operation_id,
            message.release_memory
        );
        return bluetooth_submit_result(coproc, request, status);
    }

    case WLH_BLUETOOTH_METHOD_GET_INFO: {
        if (coproc->bluetooth_pending.active) {
            return send_bluetooth_status(
                coproc, request->method_id, request->request_id, WLH_STATUS_BUSY
            );
        }
        operation_id = bluetooth_begin_operation(coproc, request);
        status = coproc->config.bluetooth.get_info(
            coproc->config.bluetooth.context, operation_id
        );
        return bluetooth_submit_result(coproc, request, status);
    }

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
}

WLH_NOINLINE void bluetooth_operation_completed(
    wlh_coproc_t *coproc, const coproc_bluetooth_complete_job_t *completed
) {
    uint16_t method_id;
    uint32_t request_id;

    if (!coproc->bluetooth_pending.active ||
        coproc->bluetooth_pending.method_id == WLH_BLUETOOTH_METHOD_GET_INFO ||
        completed->operation_id != coproc->bluetooth_pending.operation_id ||
        coproc->bluetooth_pending.session_id != coproc->session_id) {
        ++coproc->diagnostics.bluetooth_mismatches;
        return;
    }
    method_id = coproc->bluetooth_pending.method_id;
    request_id = coproc->bluetooth_pending.request_id;
    memset(&coproc->bluetooth_pending, 0, sizeof(coproc->bluetooth_pending));
    if (completed->backend_status != 0) {
        (void)send_bluetooth_status(
            coproc, method_id, request_id, WLH_STATUS_INTERNAL
        );
        return;
    }
    switch (method_id) {
    case WLH_BLUETOOTH_METHOD_INITIALIZE:
        coproc->bluetooth_state = BT_STATE_DISABLED;
        break;
    case WLH_BLUETOOTH_METHOD_ENABLE:
        coproc->bluetooth_state = BT_STATE_ENABLED;
        break;
    case WLH_BLUETOOTH_METHOD_DISABLE:
        coproc->bluetooth_state = BT_STATE_DISABLED;
        coproc->bluetooth_hci_stopped = false;
        break;
    default:
        coproc->bluetooth_state = BT_STATE_UNSPECIFIED;
        coproc->bluetooth_hci_stopped = false;
        break;
    }
    (void)send_bluetooth_status(coproc, method_id, request_id, WLH_STATUS_OK);
}

WLH_NOINLINE void bluetooth_info_completed(
    wlh_coproc_t *coproc, const coproc_bluetooth_info_job_t *completed
) {
    uint32_t request_id;
    wlh_protocol_v1_BluetoothControllerInfo response =
        wlh_protocol_v1_BluetoothControllerInfo_init_zero;

    if (!coproc->bluetooth_pending.active ||
        coproc->bluetooth_pending.method_id != WLH_BLUETOOTH_METHOD_GET_INFO ||
        completed->operation_id != coproc->bluetooth_pending.operation_id ||
        coproc->bluetooth_pending.session_id != coproc->session_id) {
        ++coproc->diagnostics.bluetooth_mismatches;
        return;
    }
    request_id = coproc->bluetooth_pending.request_id;
    memset(&coproc->bluetooth_pending, 0, sizeof(coproc->bluetooth_pending));
    if (completed->backend_status != 0) {
        (void)send_bluetooth_status(
            coproc,
            WLH_BLUETOOTH_METHOD_GET_INFO,
            request_id,
            WLH_STATUS_INTERNAL
        );
        return;
    }
    response.state =
        (wlh_protocol_v1_BluetoothControllerState)coproc->bluetooth_state;
    if (completed->info.has_public_address) {
        response.public_address.size =
            (pb_size_t)sizeof(completed->info.public_address);
        memcpy(
            response.public_address.bytes,
            completed->info.public_address,
            sizeof(completed->info.public_address)
        );
    }
    response.hci_version = completed->info.hci_version;
    response.manufacturer_id = completed->info.manufacturer_id;
    response.feature_bits = completed->info.feature_bits;
    response.max_hci_packet = completed->info.max_hci_packet;
    (void)send_rpc_message(
        coproc,
        WLH_SERVICE_BLUETOOTH,
        WLH_BLUETOOTH_METHOD_GET_INFO,
        request_id,
        WLH_RPC_KIND_RESPONSE,
        WLH_STATUS_DOMAIN_NONE,
        WLH_STATUS_OK,
        wlh_protocol_v1_BluetoothControllerInfo_fields,
        &response
    );
}

static bool hci_record_valid(const wlh_raw_record_view_t *record) {
    const uint8_t *packet = record->payload;
    size_t size = record->payload_size;

    if (size > WLH_COPROC_MAX_HCI_PACKET)
        return false;
    switch (record->record_type) {
    case WLH_H4_TYPE_COMMAND:
        return size >= 3u && (size_t)packet[2] + 3u == size;
    case WLH_H4_TYPE_ACL:
        return size >= 4u &&
               ((size_t)packet[2] | ((size_t)packet[3] << 8)) + 4u == size;
    default:
        /* Events flow only Controller->Host; SCO and ISO are unsupported. */
        return false;
    }
}

WLH_NOINLINE wlh_coproc_result_t process_hci_frame(
    wlh_coproc_t *coproc, const uint8_t *payload, size_t payload_size
) {
    wlh_raw_record_iterator_t iterator;
    wlh_raw_record_view_t record;
    wlh_wire_result_t record_result = WLH_WIRE_INVALID_ARGUMENT;

    if (!bluetooth_backend_present(coproc))
        return WLH_COPROC_PROTOCOL_ERROR;
    if (coproc->bluetooth_hci_stopped) {
        ++coproc->diagnostics.hci_drops;
        return WLH_COPROC_INVALID_STATE;
    }

    /* Validate every aggregated record before delivering any of them. A
       malformed record poisons the whole frame: it is dropped, counted and
       latches the ERROR state so no further HCI flows this session. */
    if (payload_size != 0u &&
        wlh_raw_record_iterator_init(&iterator, payload, payload_size) ==
            WLH_WIRE_OK) {
        while ((record_result =
                    wlh_raw_record_iterator_next(&iterator, &record)) ==
               WLH_WIRE_OK) {
            if (!hci_record_valid(&record)) {
                record_result = WLH_WIRE_INVALID_ARGUMENT;
                break;
            }
        }
    }
    if (record_result != WLH_WIRE_END) {
        ++coproc->diagnostics.hci_malformed;
        WLH_LOGW("wlh_coproc", "malformed HCI frame (%zu bytes)", payload_size);
        bluetooth_enter_error(
            coproc, WLH_COPROC_BLUETOOTH_REASON_MALFORMED_HCI
        );
        return WLH_COPROC_PROTOCOL_ERROR;
    }

    (void)wlh_raw_record_iterator_init(&iterator, payload, payload_size);
    while (wlh_raw_record_iterator_next(&iterator, &record) == WLH_WIRE_OK) {
        if (coproc->config.bluetooth.hci_send(
                coproc->config.bluetooth.context,
                (uint8_t)record.record_type,
                record.payload,
                record.payload_size
            ) != 0) {
            /* Withhold the credit so the rejection backpressures the host
               instead of silently dropping the rest of the frame. */
            ++coproc->diagnostics.hci_drops;
            return WLH_COPROC_BACKEND_ERROR;
        }
    }
    return send_credit_update(coproc, WLH_CHANNEL_BLUETOOTH_HCI);
}

wlh_coproc_result_t wlh_coproc_bluetooth_operation_complete(
    wlh_coproc_t *coproc, uint32_t operation_id, int backend_status
) {
    coproc_bluetooth_complete_job_t *job;
    if (coproc == NULL || operation_id == 0u || !coproc->worker_started)
        return WLH_COPROC_INVALID_ARGUMENT;
    job = (coproc_bluetooth_complete_job_t *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*job)
    );
    if (job == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    job->operation_id = operation_id;
    job->backend_status = backend_status;
    if (enqueue_job(
            coproc, COPROC_JOB_BLUETOOTH_COMPLETE, job, WLH_OSAL_NO_WAIT
        ) != 0) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)job
        );
        return WLH_COPROC_BACKEND_ERROR;
    }
    return WLH_COPROC_OK;
}

wlh_coproc_result_t wlh_coproc_bluetooth_info_result(
    wlh_coproc_t *coproc,
    uint32_t operation_id,
    int backend_status,
    const wlh_coproc_bluetooth_info_t *info
) {
    coproc_bluetooth_info_job_t *job;
    if (coproc == NULL || operation_id == 0u || !coproc->worker_started ||
        (info == NULL && backend_status == 0))
        return WLH_COPROC_INVALID_ARGUMENT;
    job = (coproc_bluetooth_info_job_t *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*job)
    );
    if (job == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    memset(job, 0, sizeof(*job));
    job->operation_id = operation_id;
    job->backend_status = backend_status;
    if (info != NULL)
        job->info = *info;
    if (enqueue_job(coproc, COPROC_JOB_BLUETOOTH_INFO, job, WLH_OSAL_NO_WAIT) !=
        0) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)job
        );
        return WLH_COPROC_BACKEND_ERROR;
    }
    return WLH_COPROC_OK;
}

static void bluetooth_release_tx_reservation(
    wlh_coproc_t *coproc, uint8_t channel
) {
    (void)coproc->config.osal.mutex_lock(
        coproc->config.osal.context, &coproc->state_mutex, WLH_OSAL_WAIT_FOREVER
    );
    if (channel == WLH_CHANNEL_BLUETOOTH_HCI_ADV) {
        if (coproc->bluetooth_adv_tx_inflight > 0u)
            --coproc->bluetooth_adv_tx_inflight;
    } else if (coproc->bluetooth_tx_inflight > 0u) {
        --coproc->bluetooth_tx_inflight;
    }
    coproc->config.osal.mutex_unlock(
        coproc->config.osal.context, &coproc->state_mutex
    );
}

wlh_coproc_result_t wlh_coproc_bluetooth_hci_send(
    wlh_coproc_t *coproc,
    uint8_t h4_type,
    const uint8_t *packet,
    size_t packet_size
) {
    coproc_data_job_t *job;
    uint8_t channel = WLH_CHANNEL_BLUETOOTH_HCI;

    if (coproc == NULL || packet == NULL || !coproc->worker_started ||
        packet_size > WLH_COPROC_MAX_HCI_PACKET)
        return WLH_COPROC_INVALID_ARGUMENT;
    if (!bluetooth_backend_present(coproc))
        return WLH_COPROC_NOT_SUPPORTED;
    switch (h4_type) {
    case WLH_H4_TYPE_EVENT:
        if (packet_size < 2u || (size_t)packet[1] + 2u != packet_size)
            return WLH_COPROC_INVALID_ARGUMENT;
        break;
    case WLH_H4_TYPE_ACL:
        if (packet_size < 4u ||
            ((size_t)packet[2] | ((size_t)packet[3] << 8)) + 4u != packet_size)
            return WLH_COPROC_INVALID_ARGUMENT;
        break;
    case WLH_H4_TYPE_SCO:
    case WLH_H4_TYPE_ISO:
        return WLH_COPROC_NOT_SUPPORTED;
    default:
        /* Commands flow only Host->Controller. */
        return WLH_COPROC_INVALID_ARGUMENT;
    }

    /* Reserve a credit up front so a NO_CREDIT result leaves the packet with
       the backend (queue head retained) instead of dropping it. The worker
       releases the reservation when the queued record is actually sent. */
    (void)coproc->config.osal.mutex_lock(
        coproc->config.osal.context, &coproc->state_mutex, WLH_OSAL_WAIT_FOREVER
    );
    if (coproc->bluetooth_hci_stopped) {
        coproc->config.osal.mutex_unlock(
            coproc->config.osal.context, &coproc->state_mutex
        );
        return WLH_COPROC_INVALID_STATE;
    }
    if (h4_type == WLH_H4_TYPE_EVENT && coproc->bluetooth_adv_channel &&
        wlh_hci_event_is_adv_report(packet, packet_size)) {
        /* Best-effort reports never backpressure the backend: shedding here
           keeps reliable HCI flowing behind them in the backend queue. */
        channel = WLH_CHANNEL_BLUETOOTH_HCI_ADV;
        if (coproc->tx_credit[WLH_CHANNEL_BLUETOOTH_HCI_ADV] <=
            coproc->bluetooth_adv_tx_inflight) {
            ++coproc->diagnostics.hci_adv_drops;
            coproc->config.osal.mutex_unlock(
                coproc->config.osal.context, &coproc->state_mutex
            );
            return WLH_COPROC_OK;
        }
        ++coproc->bluetooth_adv_tx_inflight;
    } else {
        if (coproc->tx_credit[WLH_CHANNEL_BLUETOOTH_HCI] <=
            coproc->bluetooth_tx_inflight) {
            coproc->config.osal.mutex_unlock(
                coproc->config.osal.context, &coproc->state_mutex
            );
            return WLH_COPROC_NO_CREDIT;
        }
        ++coproc->bluetooth_tx_inflight;
    }
    coproc->config.osal.mutex_unlock(
        coproc->config.osal.context, &coproc->state_mutex
    );

    job = (coproc_data_job_t *)coproc->config.buffers.alloc(
        coproc->config.buffers.context,
        sizeof(*job) + RAW_HEADER_SIZE + packet_size
    );
    if (job == NULL) {
        bluetooth_release_tx_reservation(coproc, channel);
        return WLH_COPROC_BACKEND_ERROR;
    }
    memset(job, 0, sizeof(*job));
    job->channel = channel;
    {
        size_t record_size = 0;
        if (wlh_raw_record_encode(
                job->data,
                RAW_HEADER_SIZE + packet_size,
                &record_size,
                h4_type,
                0u,
                packet,
                packet_size
            ) != WLH_WIRE_OK) {
            coproc->config.buffers.free(
                coproc->config.buffers.context, (uint8_t *)job
            );
            bluetooth_release_tx_reservation(coproc, channel);
            return WLH_COPROC_INVALID_ARGUMENT;
        }
        job->size = record_size;
    }
    if (enqueue_job(coproc, COPROC_JOB_BLUETOOTH_TX, job, WLH_OSAL_NO_WAIT) !=
        0) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)job
        );
        bluetooth_release_tx_reservation(coproc, channel);
        return WLH_COPROC_BACKEND_ERROR;
    }
    return WLH_COPROC_OK;
}

wlh_coproc_result_t wlh_coproc_bluetooth_fatal_error(
    wlh_coproc_t *coproc, uint32_t reason
) {
    coproc_bluetooth_fatal_job_t *job;
    if (coproc == NULL || !coproc->worker_started)
        return WLH_COPROC_INVALID_ARGUMENT;
    job = (coproc_bluetooth_fatal_job_t *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*job)
    );
    if (job == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    job->reason = reason;
    if (enqueue_job(
            coproc, COPROC_JOB_BLUETOOTH_FATAL, job, WLH_OSAL_NO_WAIT
        ) != 0) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)job
        );
        return WLH_COPROC_BACKEND_ERROR;
    }
    return WLH_COPROC_OK;
}
