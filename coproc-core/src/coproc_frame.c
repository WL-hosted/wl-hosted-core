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

WLH_NOINLINE wlh_coproc_result_t
process_frame(wlh_coproc_t *coproc, const uint8_t *frame, size_t size) {
    wlh_frame_header_t header;
    const uint8_t *payload;
    size_t payload_size;
    wlh_wire_result_t wire;

    if (coproc == NULL || frame == NULL) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }

    wire = wlh_frame_decode(
        &header,
        &payload,
        &payload_size,
        frame,
        size,
        coproc->config.max_frame_size
    );
    if (wire != WLH_WIRE_OK) {
        if (wire == WLH_WIRE_CHECKSUM_MISMATCH) {
            ++coproc->diagnostics.checksum_errors;
        }
        WLH_LOGW("wlh_coproc", "frame decode error %d", (int)wire);
        return WLH_COPROC_PROTOCOL_ERROR;
    }

    if (coproc->rx_sequence_valid[header.channel] &&
        header.sequence != coproc->rx_sequence[header.channel]) {
        ++coproc->diagnostics.sequence_gaps;
        WLH_LOGW(
            "wlh_coproc",
            "sequence gap on channel %u: expected %lu, got %lu",
            (unsigned)header.channel,
            (unsigned long)coproc->rx_sequence[header.channel],
            (unsigned long)header.sequence
        );
    }
    coproc->rx_sequence[header.channel] = header.sequence + 1u;
    coproc->rx_sequence_valid[header.channel] = true;
    ++coproc->diagnostics.rx_frames;
    coproc->diagnostics.last_peer_activity_ms = now_ms(coproc);

    if (header.channel == WLH_CHANNEL_LINK_CONTROL ||
        header.channel == WLH_CHANNEL_CONTROL_RPC) {
        return handle_rpc(coproc, &header, payload, payload_size);
    }

    if (header.channel == WLH_CHANNEL_BLUETOOTH_HCI) {
        if ((coproc->state != WLH_COPROC_STATE_READY &&
             coproc->state != WLH_COPROC_STATE_CONGESTED) ||
            header.session_id != coproc->session_id) {
            return WLH_COPROC_INVALID_STATE;
        }
        return process_hci_frame(coproc, payload, payload_size);
    }

    if (header.channel == WLH_CHANNEL_ETHERNET_STA ||
        header.channel == WLH_CHANNEL_ETHERNET_AP) {
        bool payload_valid = payload_size != 0u;
        wlh_raw_record_iterator_t iterator;
        wlh_raw_record_view_t record;
        wlh_wire_result_t record_result = WLH_WIRE_INVALID_ARGUMENT;

        /* Validate every aggregated record before delivering any of them,
           so a malformed tail cannot partially deliver a frame. */
        if (payload_valid &&
            wlh_raw_record_iterator_init(&iterator, payload, payload_size) ==
                WLH_WIRE_OK) {
            while ((record_result =
                        wlh_raw_record_iterator_next(&iterator, &record)) ==
                   WLH_WIRE_OK) {
            }
        }
        payload_valid = payload_valid && record_result == WLH_WIRE_END;
        if (payload_valid) {
            wlh_coproc_ethernet_rx_fn receive =
                header.channel == WLH_CHANNEL_ETHERNET_STA
                    ? coproc->config.port.ethernet_rx
                    : coproc->config.port.ethernet_ap_rx;
            (void)wlh_raw_record_iterator_init(
                &iterator, payload, payload_size
            );
            while (wlh_raw_record_iterator_next(&iterator, &record) ==
                   WLH_WIRE_OK) {
                if (record.record_type != 1u || receive == NULL) {
                    continue;
                }
                receive(
                    coproc->config.port.context,
                    record.payload,
                    record.payload_size
                );
            }
        } else {
            WLH_LOGW(
                "wlh_coproc",
                "malformed raw record on channel %u (%zu bytes)",
                (unsigned)header.channel,
                payload_size
            );
        }
        /* Return the credit even when the payload is rejected, so a transient
           fault cannot permanently strand the peer in CONGESTED. */
        (void)send_credit_update(coproc, header.channel);
        return payload_valid ? WLH_COPROC_OK : WLH_COPROC_PROTOCOL_ERROR;
    }

    return WLH_COPROC_PROTOCOL_ERROR;
}

wlh_coproc_result_t wlh_coproc_on_frame(
    wlh_coproc_t *coproc, const uint8_t *frame, size_t size
) {
    coproc_data_job_t *job;
    if (coproc == NULL || frame == NULL || size < WLH_FRAME_HEADER_SIZE ||
        size > coproc->config.max_frame_size || !coproc->worker_started)
        return WLH_COPROC_INVALID_ARGUMENT;
    job = (coproc_data_job_t *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*job) + size
    );
    if (job == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    memset(job, 0, sizeof(*job));
    job->size = size;
    memcpy(job->data, frame, size);
    if (enqueue_job(coproc, COPROC_JOB_RX_FRAME, job, WLH_OSAL_NO_WAIT) != 0) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)job
        );
        return WLH_COPROC_BACKEND_ERROR;
    }
    return WLH_COPROC_OK;
}
