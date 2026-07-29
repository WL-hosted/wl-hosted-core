#include "host_internal.h"
#include "wlh/log.h"

#include <limits.h>
#include <string.h>

#include "bluetooth.pb.h"
#include "link.pb.h"
#include "user_passthrough.pb.h"
#include "wifi.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>

WLH_NOINLINE wlh_host_result_t
process_frame(wlh_host_t *host, const uint8_t *frame, size_t size) {
    wlh_frame_header_t header;
    const uint8_t *frame_payload;
    size_t frame_payload_size;
    wlh_wire_result_t wire;
    if (host == NULL || frame == NULL) {
        return WLH_HOST_INVALID_ARGUMENT;
    }
    wire = wlh_frame_decode(
        &header,
        &frame_payload,
        &frame_payload_size,
        frame,
        size,
        host->config.max_frame_size
    );
    if (wire != WLH_WIRE_OK) {
        host->diagnostics.checksum_errors +=
            wire == WLH_WIRE_CHECKSUM_MISMATCH ? 1u : 0u;
        WLH_LOGW("wlh_host", "frame decode error %d", (int)wire);
        dispatch_event(host, WLH_HOST_EVENT_PROTOCOL_FAULT, 0u, 0u, NULL, 0u);
        return WLH_HOST_PROTOCOL_ERROR;
    }

    if (host->session_id != 0u && header.session_id != host->session_id) {
        host->diagnostics.peer_resets++;
        WLH_LOGW(
            "wlh_host",
            "session mismatch (expected %lu, got %lu), recovering",
            (unsigned long)host->session_id,
            (unsigned long)header.session_id
        );
        cancel_pending(host, WLH_HOST_SESSION_CHANGED);
        host->session_id = 0u;
        host->bluetooth_supported = false;
        host->bluetooth_hci_stopped = false;
        host->bluetooth_tx_inflight = 0u;
        host->bluetooth_state = WLH_BLUETOOTH_STATE_UNSPECIFIED;
        host->ota_supported = false;
        host->ota_tx_inflight = 0u;
        host->peer_version[0] = '\0';
        set_state(host, WLH_HOST_STATE_RECOVERING);
        (void)send_hello(host);
        return WLH_HOST_SESSION_CHANGED;
    }

    if (host->rx_sequence_valid[header.channel] &&
        header.sequence != host->expected_rx_sequence[header.channel]) {
        host->diagnostics.sequence_gaps++;
        WLH_LOGW(
            "wlh_host",
            "sequence gap on channel %u: expected %lu, got %lu",
            (unsigned)header.channel,
            (unsigned long)host->expected_rx_sequence[header.channel],
            (unsigned long)header.sequence
        );
    }
    host->expected_rx_sequence[header.channel] = header.sequence + 1u;
    host->rx_sequence_valid[header.channel] = true;
    host->diagnostics.rx_frames++;
    host->diagnostics.last_peer_activity_ms = now_ms(host);

    // Dispatch by channel.
    if (header.channel == WLH_CHANNEL_BLUETOOTH_HCI ||
        header.channel == WLH_CHANNEL_BLUETOOTH_HCI_ADV)
        return process_hci_frame(
            host, header.channel, frame_payload, frame_payload_size
        );
    if (header.channel == WLH_CHANNEL_OTA_STREAM)
        return process_ota_frame(host, frame_payload, frame_payload_size);
    if (header.channel == WLH_CHANNEL_ETHERNET_STA ||
        header.channel == WLH_CHANNEL_ETHERNET_AP) {
        bool dispatch_failed = false;
        bool payload_valid = frame_payload_size != 0u;
        wlh_raw_record_iterator_t iterator;
        wlh_raw_record_view_t record;
        wlh_wire_result_t record_result = WLH_WIRE_INVALID_ARGUMENT;
        wlh_host_result_t credit_result;

        /* Validate every aggregated record before dispatching any of them,
           so a malformed tail cannot partially deliver a frame. */
        if (payload_valid && wlh_raw_record_iterator_init(
                                 &iterator, frame_payload, frame_payload_size
                             ) == WLH_WIRE_OK) {
            while ((record_result =
                        wlh_raw_record_iterator_next(&iterator, &record)) ==
                   WLH_WIRE_OK) {
            }
        }
        payload_valid = payload_valid && record_result == WLH_WIRE_END;
        if (payload_valid) {
            (void)wlh_raw_record_iterator_init(
                &iterator, frame_payload, frame_payload_size
            );
            while (wlh_raw_record_iterator_next(&iterator, &record) ==
                   WLH_WIRE_OK) {
                if (record.record_type != 1u) {
                    continue;
                }
                if (!dispatch_event(
                        host,
                        header.channel == WLH_CHANNEL_ETHERNET_STA
                            ? WLH_HOST_EVENT_ETHERNET_STA_RX
                            : WLH_HOST_EVENT_ETHERNET_AP_RX,
                        0u,
                        0u,
                        record.payload,
                        record.payload_size
                    )) {
                    dispatch_failed = true;
                }
            }
        }
        /* Return the credit even when the payload is rejected. The peer spent
           one credit to deliver this frame; withholding it on a drop turns a
           transient fault into a permanent CONGESTED stall. */
        credit_result = send_credit_update(host, header.channel);
        if (credit_result != WLH_HOST_OK) {
            WLH_LOGW(
                "wlh_host",
                "credit update failed on channel %u: %d",
                (unsigned)header.channel,
                (int)credit_result
            );
        }
        if (!payload_valid) {
            WLH_LOGW(
                "wlh_host",
                "malformed raw record on channel %u (%zu bytes)",
                (unsigned)header.channel,
                frame_payload_size
            );
            return WLH_HOST_PROTOCOL_ERROR;
        }
        return dispatch_failed ? WLH_HOST_NO_MEMORY : WLH_HOST_OK;
    }
    if (header.channel == WLH_CHANNEL_LINK_CONTROL ||
        header.channel == WLH_CHANNEL_CONTROL_RPC) {
        wlh_rpc_envelope_t envelope;
        const uint8_t *payload;
        size_t payload_size;
        if (wlh_rpc_decode(
                &envelope,
                &payload,
                &payload_size,
                frame_payload,
                frame_payload_size,
                WLH_HOST_PROTOBUF_LIMIT
            ) != WLH_WIRE_OK) {
            return WLH_HOST_PROTOCOL_ERROR;
        }
        if (envelope.kind == WLH_RPC_KIND_RESPONSE &&
            envelope.service_id == WLH_SERVICE_LINK &&
            envelope.method_id == WLH_LINK_METHOD_HELLO &&
            host->state == WLH_HOST_STATE_NEGOTIATING) {
            if (envelope.status_code != WLH_STATUS_OK) {
                set_state(host, WLH_HOST_STATE_FAILED);
                return WLH_HOST_PROTOCOL_ERROR;
            }
            return handle_hello_response(host, payload, payload_size);
        }

        if (envelope.kind == WLH_RPC_KIND_RESPONSE) {
            wlh_pending_rpc_t *pending = find_pending(host, &envelope);
            wlh_pending_rpc_t copy;
            if (pending == NULL) {
                return WLH_HOST_NOT_FOUND;
            }
            copy = *pending;
            memset(pending, 0, sizeof(*pending));
            dispatch_completion(
                host,
                copy.completion,
                copy.completion_context,
                envelope.status_code == WLH_STATUS_OK ? WLH_HOST_OK
                                                      : WLH_HOST_PROTOCOL_ERROR,
                envelope.status_domain,
                envelope.status_code,
                payload,
                payload_size
            );
            return WLH_HOST_OK;
        }

        if (envelope.kind == WLH_RPC_KIND_EVENT &&
            envelope.service_id == WLH_SERVICE_LINK) {
            handle_link_event(host, &envelope, payload, payload_size);
            return WLH_HOST_OK;
        }

        if (envelope.kind == WLH_RPC_KIND_EVENT &&
            envelope.service_id == WLH_SERVICE_WIFI) {
            wlh_host_event_kind_t kind = WLH_HOST_EVENT_PROTOCOL_FAULT;
            if (envelope.method_id == WLH_WIFI_EVENT_SCAN_RESULT)
                kind = WLH_HOST_EVENT_WIFI_SCAN_RESULT;
            else if (envelope.method_id == WLH_WIFI_EVENT_SCAN_COMPLETED)
                kind = WLH_HOST_EVENT_WIFI_SCAN_COMPLETED;
            else if (envelope.method_id == WLH_WIFI_EVENT_CONNECTED)
                kind = WLH_HOST_EVENT_WIFI_CONNECTED;
            else if (envelope.method_id == WLH_WIFI_EVENT_DISCONNECTED)
                kind = WLH_HOST_EVENT_WIFI_DISCONNECTED;
            else if (envelope.method_id == WLH_WIFI_EVENT_AP_CLIENT_JOINED)
                kind = WLH_HOST_EVENT_WIFI_AP_CLIENT_JOINED;
            else if (envelope.method_id == WLH_WIFI_EVENT_AP_CLIENT_LEFT)
                kind = WLH_HOST_EVENT_WIFI_AP_CLIENT_LEFT;
            dispatch_event(
                host,
                kind,
                envelope.service_id,
                envelope.method_id,
                payload,
                payload_size
            );
            return WLH_HOST_OK;
        }

        if (envelope.kind == WLH_RPC_KIND_EVENT &&
            envelope.service_id == WLH_SERVICE_BLUETOOTH &&
            envelope.method_id == WLH_BLUETOOTH_EVENT_STATE_CHANGED) {
            wlh_protocol_v1_BluetoothStateChangedEvent decoded =
                wlh_protocol_v1_BluetoothStateChangedEvent_init_zero;
            pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
            wlh_host_bluetooth_state_event_t event;
            if (!pb_decode(
                    &stream,
                    wlh_protocol_v1_BluetoothStateChangedEvent_fields,
                    &decoded
                ) ||
                (uint32_t)decoded.state > WLH_BLUETOOTH_STATE_ERROR)
                return WLH_HOST_PROTOCOL_ERROR;
            memset(&event, 0, sizeof(event));
            event.state = (wlh_bluetooth_state_t)decoded.state;
            event.reason = decoded.reason;
            host->bluetooth_state = event.state;
            dispatch_event(
                host,
                WLH_HOST_EVENT_BLUETOOTH_STATE_CHANGED,
                envelope.service_id,
                envelope.method_id,
                (const uint8_t *)&event,
                sizeof(event)
            );
            return WLH_HOST_OK;
        }

        if (envelope.kind == WLH_RPC_KIND_EVENT &&
            envelope.service_id == WLH_SERVICE_OTA &&
            envelope.method_id == WLH_OTA_EVENT_PROGRESS) {
            dispatch_event(
                host,
                WLH_HOST_EVENT_OTA_PROGRESS,
                envelope.service_id,
                envelope.method_id,
                payload,
                payload_size
            );
            return WLH_HOST_OK;
        }

        if (envelope.kind == WLH_RPC_KIND_EVENT &&
            envelope.service_id == WLH_SERVICE_USER_PASSTHROUGH &&
            envelope.method_id == WLH_USER_PASSTHROUGH_EVENT_RESULT) {
            dispatch_event(
                host,
                WLH_HOST_EVENT_USER_MESSAGE_RESULT,
                envelope.service_id,
                envelope.method_id,
                payload,
                payload_size
            );
            return WLH_HOST_OK;
        }
    }
    return WLH_HOST_PROTOCOL_ERROR;
}

wlh_host_result_t wlh_host_on_frame(
    wlh_host_t *host, const uint8_t *frame, size_t size
) {
    wlh_host_data_job_t *data;
    if (host == NULL || frame == NULL || size < WLH_FRAME_HEADER_SIZE ||
        size > host->config.max_frame_size || !host->worker_started)
        return WLH_HOST_INVALID_ARGUMENT;
    data = (wlh_host_data_job_t *)host->config.buffers.alloc(
        host->config.buffers.context, sizeof(*data) + size
    );
    if (data == NULL)
        return WLH_HOST_NO_MEMORY;
    data->size = size;
    memcpy(data->data, frame, size);
    if (enqueue_job(host, WLH_HOST_JOB_RX_FRAME, data, WLH_OSAL_NO_WAIT) != 0) {
        host->config.buffers.free(
            host->config.buffers.context, (uint8_t *)data
        );
        return WLH_HOST_PENDING_FULL;
    }
    return WLH_HOST_OK;
}
