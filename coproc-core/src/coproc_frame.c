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

#define WLH_ETHERNET_CREDIT_BATCH_UNITS 32u

static int ethernet_channel_index(uint8_t channel) {
    if (channel == WLH_CHANNEL_ETHERNET_STA)
        return 0;
    if (channel == WLH_CHANNEL_ETHERNET_AP)
        return 1;
    if (channel == WLH_CHANNEL_ETHERNET_ETH)
        return 2;
    return -1;
}

void reset_ethernet_rx_completions(wlh_coproc_t *coproc) {
    coproc->ethernet_rx_pending_credit[0] = 0u;
    coproc->ethernet_rx_pending_credit[1] = 0u;
    coproc->ethernet_rx_pending_credit[2] = 0u;
    coproc->ethernet_rx_pending_failures = 0u;
    coproc->ethernet_rx_wake_queued = false;
}

bool flush_ethernet_rx_completions(wlh_coproc_t *coproc) {
    static const uint8_t channels[3] = {
        WLH_CHANNEL_ETHERNET_STA,
        WLH_CHANNEL_ETHERNET_AP,
        WLH_CHANNEL_ETHERNET_ETH,
    };
    bool retry = false;
    size_t index;

    coproc->ethernet_rx_wake_queued = false;
    if (coproc->ethernet_rx_pending_failures != 0u) {
        coproc->diagnostics.ethernet_rx_failed +=
            coproc->ethernet_rx_pending_failures;
        coproc->ethernet_rx_pending_failures = 0u;
    }
    for (index = 0u; index < 3u; ++index) {
        uint32_t units = coproc->ethernet_rx_pending_credit[index];
        if (units == 0u)
            continue;
        coproc->ethernet_rx_pending_credit[index] = 0u;
        if (coproc->session_id == 0u ||
            send_credit_update(coproc, channels[index], units) !=
                WLH_COPROC_OK) {
            if (UINT32_MAX - coproc->ethernet_rx_pending_credit[index] < units)
                coproc->ethernet_rx_pending_credit[index] = UINT32_MAX;
            else
                coproc->ethernet_rx_pending_credit[index] += units;
            retry = true;
        }
    }
    return retry;
}

wlh_coproc_result_t wlh_coproc_ethernet_rx_complete(
    wlh_coproc_t *coproc,
    uint32_t session_id,
    uint8_t channel,
    uint32_t units,
    int backend_status
) {
    int index = ethernet_channel_index(channel);
    bool enqueue_wake = false;
    uint32_t batch_units;
    if (coproc == NULL || units == 0u || index < 0)
        return WLH_COPROC_INVALID_ARGUMENT;
    if (!coproc->worker_started || coproc->config.osal.mutex_lock(
                                       coproc->config.osal.context,
                                       &coproc->state_mutex,
                                       WLH_OSAL_WAIT_FOREVER
                                   ) != 0)
        return WLH_COPROC_INVALID_STATE;
    if (coproc->session_id == 0u || session_id != coproc->session_id) {
        coproc->config.osal.mutex_unlock(
            coproc->config.osal.context, &coproc->state_mutex
        );
        return WLH_COPROC_INVALID_STATE;
    }
    if (UINT32_MAX - coproc->ethernet_rx_pending_credit[index] < units)
        coproc->ethernet_rx_pending_credit[index] = UINT32_MAX;
    else
        coproc->ethernet_rx_pending_credit[index] += units;
    if (backend_status != 0 &&
        coproc->ethernet_rx_pending_failures < UINT32_MAX)
        ++coproc->ethernet_rx_pending_failures;
    /* Do not wake the Core for every Wi-Fi TX completion. At line rate that
       creates one 52-byte CreditUpdate (and one SDIO transaction) per packet,
       which can consume more bus time than the Ethernet data itself. The
       normal worker/heartbeat wake flushes a short tail; a full batch wakes
       it immediately so at most one negotiated channel window is withheld.
       The batch must never exceed the peer's in-flight window: with a window
       smaller than the nominal batch the peer drains its credits and then
       stalls until the 1 s heartbeat flush, which caps the host->device data
       rate at initial_credit frames per second. */
    batch_units = coproc->config.initial_credit;
    if (batch_units == 0u || batch_units > WLH_ETHERNET_CREDIT_BATCH_UNITS)
        batch_units = WLH_ETHERNET_CREDIT_BATCH_UNITS;
    if (!coproc->ethernet_rx_wake_queued &&
        coproc->ethernet_rx_pending_credit[index] >= batch_units) {
        coproc->ethernet_rx_wake_queued = true;
        enqueue_wake = true;
    }
    coproc->config.osal.mutex_unlock(
        coproc->config.osal.context, &coproc->state_mutex
    );
    if (enqueue_wake &&
        enqueue_job(
            coproc, COPROC_JOB_ETHERNET_RX_COMPLETE, NULL, WLH_OSAL_NO_WAIT
        ) != 0) {
        /* A full queue already guarantees that the worker is runnable. It
         * flushes the accumulated credits before its next receive. */
    }
    return WLH_COPROC_OK;
}

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
        /* Frames the peer sent but that never reached this point still spent
         * the peer's credits (it charges one per raw record at USB-submit
         * time). Refund the skipped Ethernet data frames, otherwise every
         * lost frame permanently leaks one window unit until the channel
         * strangles. The wrap-safe forward check keeps reordered/duplicate
         * frames from inflating the window. */
        uint32_t skipped =
            header.sequence - coproc->rx_sequence[header.channel];
        ++coproc->diagnostics.sequence_gaps;
        WLH_LOGW(
            "wlh_coproc",
            "sequence gap on channel %u: expected %lu, got %lu",
            (unsigned)header.channel,
            (unsigned long)coproc->rx_sequence[header.channel],
            (unsigned long)header.sequence
        );
        if (skipped < 0x80000000u &&
            (header.channel == WLH_CHANNEL_ETHERNET_STA ||
             header.channel == WLH_CHANNEL_ETHERNET_AP ||
             header.channel == WLH_CHANNEL_ETHERNET_ETH)) {
            (void)send_credit_update(coproc, header.channel, skipped);
        }
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

    if (header.channel == WLH_CHANNEL_OTA_STREAM) {
        if ((coproc->state != WLH_COPROC_STATE_READY &&
             coproc->state != WLH_COPROC_STATE_CONGESTED) ||
            header.session_id != coproc->session_id) {
            return WLH_COPROC_INVALID_STATE;
        }
        /* Credit for this frame is returned inside process_ota_frame: either
           immediately when the chunk is rejected, or from write_complete once
           the accepted chunk is durably written. */
        return process_ota_frame(coproc, payload, payload_size);
    }

    if (header.channel == WLH_CHANNEL_ETHERNET_STA ||
        header.channel == WLH_CHANNEL_ETHERNET_AP ||
        header.channel == WLH_CHANNEL_ETHERNET_ETH) {
        bool payload_valid = payload_size != 0u;
        wlh_raw_record_iterator_t iterator;
        wlh_raw_record_view_t record;
        wlh_wire_result_t record_result = WLH_WIRE_INVALID_ARGUMENT;
        uint32_t immediate_credit_units = 0u;

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
            wlh_coproc_ethernet_rx_fn receive;
            switch (header.channel) {
            case WLH_CHANNEL_ETHERNET_STA:
                receive = coproc->config.port.ethernet_sta_rx;
                break;
            case WLH_CHANNEL_ETHERNET_AP:
                receive = coproc->config.port.ethernet_ap_rx;
                break;
            default:
                receive = coproc->config.port.ethernet_eth_rx;
                break;
            }
            (void)wlh_raw_record_iterator_init(
                &iterator, payload, payload_size
            );
            while (wlh_raw_record_iterator_next(&iterator, &record) ==
                   WLH_WIRE_OK) {
                if (record.record_type != 1u || receive == NULL) {
                    ++immediate_credit_units;
                    continue;
                }
                {
                    wlh_coproc_ethernet_rx_result_t receive_result = receive(
                        coproc->config.port.context,
                        coproc->session_id,
                        header.channel,
                        record.payload,
                        record.payload_size
                    );
                    if (receive_result == WLH_COPROC_ETHERNET_RX_PENDING)
                        continue;
                    ++immediate_credit_units;
                    if (receive_result == WLH_COPROC_ETHERNET_RX_REJECTED) {
                        ++coproc->diagnostics.ethernet_rx_rejected;
                    }
                }
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
           fault cannot permanently strand the peer in CONGESTED. The peer
           charges one unit per raw record. Synchronous deliveries return here;
           asynchronous deliveries return from ethernet_rx_complete. A
           malformed payload yields no trustworthy record count, so fall back
           to one unit rather than a value derived from a bad parse. */
        if (!payload_valid)
            immediate_credit_units = 1u;
        (void)send_credit_update(
            coproc, header.channel, immediate_credit_units
        );
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
