#include "host_internal.h"
#include "wlh/log.h"

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

static wlh_host_result_t bluetooth_check_supported(wlh_host_t *host) {
    bool supported;
    if (!host->worker_started)
        return WLH_HOST_INVALID_STATE;
    (void)host->config.osal.mutex_lock(
        host->config.osal.context, &host->state_mutex, WLH_OSAL_WAIT_FOREVER
    );
    supported = host->bluetooth_supported;
    host->config.osal.mutex_unlock(
        host->config.osal.context, &host->state_mutex
    );
    return supported ? WLH_HOST_OK : WLH_HOST_NOT_SUPPORTED;
}

static wlh_host_result_t bluetooth_request(
    wlh_host_t *host,
    uint16_t method,
    const pb_msgdesc_t *fields,
    const void *message,
    wlh_rpc_completion_fn completion,
    void *context
) {
    wlh_host_result_t result;
    if (host == NULL)
        return WLH_HOST_INVALID_ARGUMENT;
    result = bluetooth_check_supported(host);
    if (result != WLH_HOST_OK)
        return result;
    return rpc_message_request(
        host,
        WLH_SERVICE_BLUETOOTH,
        method,
        fields,
        message,
        completion,
        context
    );
}

wlh_host_result_t wlh_host_bluetooth_initialize(
    wlh_host_t *host,
    uint32_t feature_flags,
    wlh_rpc_completion_fn completion,
    void *context
) {
    wlh_protocol_v1_BluetoothInitializeRequest request =
        wlh_protocol_v1_BluetoothInitializeRequest_init_zero;
    request.transport =
        wlh_protocol_v1_BluetoothTransport_BLUETOOTH_TRANSPORT_HCI;
    request.feature_flags = feature_flags;
    return bluetooth_request(
        host,
        WLH_BLUETOOTH_METHOD_INITIALIZE,
        wlh_protocol_v1_BluetoothInitializeRequest_fields,
        &request,
        completion,
        context
    );
}

wlh_host_result_t wlh_host_bluetooth_enable(
    wlh_host_t *host,
    uint32_t mode_flags,
    wlh_rpc_completion_fn completion,
    void *context
) {
    wlh_protocol_v1_BluetoothEnableRequest request =
        wlh_protocol_v1_BluetoothEnableRequest_init_zero;
    request.mode_flags = mode_flags;
    return bluetooth_request(
        host,
        WLH_BLUETOOTH_METHOD_ENABLE,
        wlh_protocol_v1_BluetoothEnableRequest_fields,
        &request,
        completion,
        context
    );
}

wlh_host_result_t wlh_host_bluetooth_disable(
    wlh_host_t *host, wlh_rpc_completion_fn completion, void *context
) {
    wlh_protocol_v1_Empty request = wlh_protocol_v1_Empty_init_zero;
    return bluetooth_request(
        host,
        WLH_BLUETOOTH_METHOD_DISABLE,
        wlh_protocol_v1_Empty_fields,
        &request,
        completion,
        context
    );
}

wlh_host_result_t wlh_host_bluetooth_deinitialize(
    wlh_host_t *host,
    bool release_memory,
    wlh_rpc_completion_fn completion,
    void *context
) {
    wlh_protocol_v1_BluetoothDeinitializeRequest request =
        wlh_protocol_v1_BluetoothDeinitializeRequest_init_zero;
    request.release_memory = release_memory;
    return bluetooth_request(
        host,
        WLH_BLUETOOTH_METHOD_DEINITIALIZE,
        wlh_protocol_v1_BluetoothDeinitializeRequest_fields,
        &request,
        completion,
        context
    );
}

typedef struct wlh_bluetooth_info_request {
    wlh_host_t *host;
    wlh_host_bluetooth_info_fn completion;
    void *context;
} wlh_bluetooth_info_request_t;

static void bluetooth_info_completion(
    void *context,
    wlh_host_result_t result,
    uint16_t status_domain,
    int16_t status_code,
    const uint8_t *payload,
    size_t payload_size
) {
    wlh_bluetooth_info_request_t *request = context;
    wlh_host_t *host = request->host;
    wlh_bluetooth_controller_info_t info;
    const wlh_bluetooth_controller_info_t *decoded = NULL;

    if (result == WLH_HOST_OK) {
        wlh_protocol_v1_BluetoothControllerInfo message =
            wlh_protocol_v1_BluetoothControllerInfo_init_zero;
        pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
        if (pb_decode(
                &stream,
                wlh_protocol_v1_BluetoothControllerInfo_fields,
                &message
            ) &&
            (uint32_t)message.state <= WLH_BLUETOOTH_STATE_ERROR &&
            (message.public_address.size == 0u ||
             message.public_address.size == 6u)) {
            memset(&info, 0, sizeof(info));
            info.state = (wlh_bluetooth_state_t)message.state;
            info.has_public_address = message.public_address.size == 6u;
            if (info.has_public_address)
                memcpy(info.public_address, message.public_address.bytes, 6u);
            info.hci_version = (uint8_t)message.hci_version;
            info.manufacturer_id = (uint16_t)message.manufacturer_id;
            info.feature_bits = message.feature_bits;
            info.max_hci_packet = message.max_hci_packet;
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

wlh_host_result_t wlh_host_bluetooth_get_info(
    wlh_host_t *host, wlh_host_bluetooth_info_fn completion, void *context
) {
    wlh_protocol_v1_Empty message = wlh_protocol_v1_Empty_init_zero;
    wlh_bluetooth_info_request_t *request;
    wlh_host_result_t result;

    if (host == NULL || completion == NULL)
        return WLH_HOST_INVALID_ARGUMENT;
    result = bluetooth_check_supported(host);
    if (result != WLH_HOST_OK)
        return result;
    request = (wlh_bluetooth_info_request_t *)host->config.buffers.alloc(
        host->config.buffers.context, sizeof(*request)
    );
    if (request == NULL)
        return WLH_HOST_NO_MEMORY;
    request->host = host;
    request->completion = completion;
    request->context = context;

    result = rpc_message_request(
        host,
        WLH_SERVICE_BLUETOOTH,
        WLH_BLUETOOTH_METHOD_GET_INFO,
        wlh_protocol_v1_Empty_fields,
        &message,
        bluetooth_info_completion,
        request
    );
    if (result != WLH_HOST_OK)
        host->config.buffers.free(
            host->config.buffers.context, (uint8_t *)request
        );
    return result;
}

static void bluetooth_release_inflight(wlh_host_t *host) {
    (void)host->config.osal.mutex_lock(
        host->config.osal.context, &host->state_mutex, WLH_OSAL_WAIT_FOREVER
    );
    if (host->bluetooth_tx_inflight > 0u)
        host->bluetooth_tx_inflight--;
    host->config.osal.mutex_unlock(
        host->config.osal.context, &host->state_mutex
    );
}

wlh_host_result_t wlh_host_bluetooth_hci_send(
    wlh_host_t *host, uint8_t h4_type, const uint8_t *packet, size_t size
) {
    uint8_t *record;
    wlh_host_result_t result = WLH_HOST_OK;

    if (host == NULL || packet == NULL || size == 0u || !host->worker_started)
        return WLH_HOST_INVALID_ARGUMENT;
    if (h4_type == WLH_H4_TYPE_SCO || h4_type == WLH_H4_TYPE_ISO)
        return WLH_HOST_NOT_SUPPORTED;
    if (h4_type == WLH_H4_TYPE_COMMAND) {
        if (size < 3u || (size_t)packet[2] + 3u != size)
            return WLH_HOST_INVALID_ARGUMENT;
    } else if (h4_type == WLH_H4_TYPE_ACL) {
        if (size < 4u ||
            (size_t)((uint16_t)packet[2] | ((uint16_t)packet[3] << 8)) + 4u !=
                size)
            return WLH_HOST_INVALID_ARGUMENT;
    } else {
        return WLH_HOST_INVALID_ARGUMENT;
    }

    (void)host->config.osal.mutex_lock(
        host->config.osal.context, &host->state_mutex, WLH_OSAL_WAIT_FOREVER
    );
    if (!host->bluetooth_supported)
        result = WLH_HOST_NOT_SUPPORTED;
    else if (host->bluetooth_hci_stopped)
        result = WLH_HOST_INVALID_STATE;
    else if (size > host->bluetooth_max_record)
        result = WLH_HOST_INVALID_ARGUMENT;
    else if (host->tx_credit[WLH_CHANNEL_BLUETOOTH_HCI] <=
             host->bluetooth_tx_inflight)
        result = WLH_HOST_NO_CREDIT;
    else
        host->bluetooth_tx_inflight++;
    host->config.osal.mutex_unlock(
        host->config.osal.context, &host->state_mutex
    );
    if (result != WLH_HOST_OK)
        return result;

    record = host->config.buffers.alloc(
        host->config.buffers.context,
        sizeof(wlh_host_data_job_t) + WLH_RAW_RECORD_HEADER_SIZE + size
    );
    if (record == NULL) {
        bluetooth_release_inflight(host);
        return WLH_HOST_NO_MEMORY;
    }
    {
        wlh_host_data_job_t *job = (wlh_host_data_job_t *)record;
        size_t record_size = 0;
        job->channel = WLH_CHANNEL_BLUETOOTH_HCI;
        if (wlh_raw_record_encode(
                job->data,
                WLH_RAW_RECORD_HEADER_SIZE + size,
                &record_size,
                h4_type,
                0u,
                packet,
                size
            ) != WLH_WIRE_OK) {
            host->config.buffers.free(
                host->config.buffers.context, (uint8_t *)job
            );
            bluetooth_release_inflight(host);
            return WLH_HOST_INVALID_ARGUMENT;
        }
        job->size = record_size;
        if (enqueue_job(
                host, WLH_HOST_JOB_BLUETOOTH_TX, job, WLH_OSAL_NO_WAIT
            ) != 0) {
            host->config.buffers.free(
                host->config.buffers.context, (uint8_t *)job
            );
            bluetooth_release_inflight(host);
            return WLH_HOST_PENDING_FULL;
        }
    }
    return WLH_HOST_OK;
}

static bool hci_rx_record_valid(
    const wlh_host_t *host, uint8_t channel, const wlh_raw_record_view_t *record
) {
    if (record->payload_size > host->bluetooth_max_record)
        return false;
    switch (record->record_type) {
    case WLH_H4_TYPE_ACL:
        /* The best-effort ADV channel only carries advertising events. */
        if (channel == WLH_CHANNEL_BLUETOOTH_HCI_ADV)
            return false;
        return record->payload_size >= 4u &&
               (size_t)((uint16_t)record->payload[2] |
                        ((uint16_t)record->payload[3] << 8)) +
                       4u ==
                   record->payload_size;
    case WLH_H4_TYPE_EVENT:
        return record->payload_size >= 2u &&
               (size_t)record->payload[1] + 2u == record->payload_size;
    default:
        /* Commands only flow Host -> Controller; SCO/ISO are unsupported. */
        return false;
    }
}

static void bluetooth_fault(wlh_host_t *host, uint32_t reason) {
    wlh_host_bluetooth_state_event_t event;
    host->diagnostics.hci_malformed++;
    host->bluetooth_hci_stopped = true;
    host->bluetooth_state = WLH_BLUETOOTH_STATE_ERROR;
    WLH_LOGW(
        "wlh_host",
        "bluetooth fault, HCI stopped (reason %lu)",
        (unsigned long)reason
    );
    memset(&event, 0, sizeof(event));
    event.state = WLH_BLUETOOTH_STATE_ERROR;
    event.reason = reason;
    (void)dispatch_event(
        host,
        WLH_HOST_EVENT_BLUETOOTH_STATE_CHANGED,
        WLH_SERVICE_BLUETOOTH,
        WLH_BLUETOOTH_EVENT_STATE_CHANGED,
        (const uint8_t *)&event,
        sizeof(event)
    );
}

WLH_NOINLINE wlh_host_result_t process_hci_frame(
    wlh_host_t *host,
    uint8_t channel,
    const uint8_t *payload,
    size_t payload_size
) {
    wlh_raw_record_iterator_t iterator;
    wlh_raw_record_view_t record;
    wlh_wire_result_t record_result = WLH_WIRE_INVALID_ARGUMENT;
    bool delivered = true;

    if (host->bluetooth_hci_stopped) {
        host->diagnostics.hci_drops++;
        return WLH_HOST_INVALID_STATE;
    }
    /* Validate the whole frame before delivering anything: a malformed record
       must never be truncated or partially delivered. */
    if (payload_size != 0u &&
        wlh_raw_record_iterator_init(&iterator, payload, payload_size) ==
            WLH_WIRE_OK) {
        while ((record_result =
                    wlh_raw_record_iterator_next(&iterator, &record)) ==
               WLH_WIRE_OK) {
            if (!hci_rx_record_valid(host, channel, &record)) {
                record_result = WLH_WIRE_INVALID_ARGUMENT;
                break;
            }
        }
    }
    if (record_result != WLH_WIRE_END) {
        bluetooth_fault(host, WLH_HOST_BLUETOOTH_REASON_MALFORMED_HCI);
        return WLH_HOST_PROTOCOL_ERROR;
    }
    (void)wlh_raw_record_iterator_init(&iterator, payload, payload_size);
    while (wlh_raw_record_iterator_next(&iterator, &record) == WLH_WIRE_OK) {
        if (host->config.bluetooth_hci_rx == NULL ||
            host->config.bluetooth_hci_rx(
                host->config.bluetooth_context,
                record.record_type,
                record.payload,
                record.payload_size
            ) != WLH_HOST_OK) {
            host->diagnostics.hci_drops++;
            delivered = false;
            break;
        }
    }
    /* Return the credit even when the adapter rejects a packet. Withholding
       it would permanently shrink the channel window: nothing re-runs Hello
       after a local drop, so every withheld credit turns a transient overload
       into a smaller pipe until HCI stalls entirely. Reliable delivery is the
       adapter's job (bounded queue with reserved control-event slots); the
       link layer only guarantees the window stays intact. */
    if (!delivered)
        WLH_LOGW(
            "wlh_host",
            "HCI rx dropped by adapter on channel %u",
            (unsigned)channel
        );
    if (send_credit_update(host, channel) != WLH_HOST_OK)
        WLH_LOGW("wlh_host", "HCI credit update failed");
    return delivered ? WLH_HOST_OK : WLH_HOST_PENDING_FULL;
}
