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

static wlh_host_result_t ethernet_send(
    wlh_host_t *host,
    uint8_t channel,
    const uint8_t *ethernet_frame,
    size_t size
) {
    uint8_t *record;
    uint8_t ethernet_index;
    static uint32_t admission_denials;
    if (host == NULL || ethernet_frame == NULL || size == 0u || size > 1600u ||
        !host->worker_started)
        return WLH_HOST_INVALID_ARGUMENT;
    if (host->config.osal.semaphore_take(
            host->config.osal.context,
            &host->ethernet_tx_slots,
            WLH_OSAL_NO_WAIT
        ) != 0)
        return WLH_HOST_PENDING_FULL;
    ethernet_index = channel == WLH_CHANNEL_ETHERNET_STA ? 0u : 1u;
    record = host->config.buffers.alloc(
        host->config.buffers.context, sizeof(wlh_host_data_job_t) + 8u + size
    );
    if (record == NULL) {
        (void)host->config.osal.semaphore_give(
            host->config.osal.context, &host->ethernet_tx_slots
        );
        return WLH_HOST_NO_MEMORY;
    }
    {
        wlh_host_data_job_t *job = (wlh_host_data_job_t *)record;
        size_t record_size = 0;
        job->channel = channel;
        if (wlh_raw_record_encode(
                job->data, 8u + size, &record_size, 1u, 0u, ethernet_frame, size
            ) != WLH_WIRE_OK) {
            host->config.buffers.free(
                host->config.buffers.context, (uint8_t *)job
            );
            (void)host->config.osal.semaphore_give(
                host->config.osal.context, &host->ethernet_tx_slots
            );
            return WLH_HOST_INVALID_ARGUMENT;
        }
        job->size = record_size;
        if (host->config.osal.mutex_lock(
                host->config.osal.context,
                &host->state_mutex,
                /* The worker holds this mutex while processing each RX
                 * frame. A zero-wait attempt starves linkoutput during a
                 * continuous Wi-Fi RX burst, dropping every ARP/TCP frame.
                 * Keep this bounded while allowing one worker critical
                 * section to complete. */
                20u
            ) != 0) {
            host->config.buffers.free(
                host->config.buffers.context, (uint8_t *)job
            );
            (void)host->config.osal.semaphore_give(
                host->config.osal.context, &host->ethernet_tx_slots
            );
            return WLH_HOST_PENDING_FULL;
        }
        bool no_credit = host->tx_credit[channel] <=
                         host->ethernet_tx_queued[ethernet_index];
        /* Keep this nonblocking: the worker that drains the queue contends for
         * the same state_mutex held here, so waiting would deadlock progress
         * rather than create it. Drops are counted so they stay visible. */
        if (no_credit ||
            enqueue_job(
                host, WLH_HOST_JOB_ETHERNET_TX, job, WLH_OSAL_NO_WAIT
            ) != 0) {
            if (no_credit)
                ++host->diagnostics.ethernet_no_credit;
            else
                ++host->diagnostics.ethernet_queue_full;
            ++admission_denials;
            if (admission_denials <= 5u || admission_denials % 100u == 0u) {
                WLH_LOGW(
                    "wlh_host",
                    "ethernet admission denied channel=%u credit=%lu "
                    "queued=%lu cause=%s",
                    (unsigned)channel,
                    (unsigned long)host->tx_credit[channel],
                    (unsigned long)host->ethernet_tx_queued[ethernet_index],
                    no_credit ? "no_credit" : "queue_full"
                );
            }
            host->config.osal.mutex_unlock(
                host->config.osal.context, &host->state_mutex
            );
            host->config.buffers.free(
                host->config.buffers.context, (uint8_t *)job
            );
            (void)host->config.osal.semaphore_give(
                host->config.osal.context, &host->ethernet_tx_slots
            );
            return WLH_HOST_NO_CREDIT;
        }
        ++host->ethernet_tx_queued[ethernet_index];
        host->config.osal.mutex_unlock(
            host->config.osal.context, &host->state_mutex
        );
    }
    return WLH_HOST_OK;
}

wlh_host_result_t wlh_host_ethernet_sta_send(
    wlh_host_t *host, const uint8_t *ethernet_frame, size_t size
) {
    return ethernet_send(host, WLH_CHANNEL_ETHERNET_STA, ethernet_frame, size);
}

wlh_host_result_t wlh_host_ethernet_ap_send(
    wlh_host_t *host, const uint8_t *ethernet_frame, size_t size
) {
    return ethernet_send(host, WLH_CHANNEL_ETHERNET_AP, ethernet_frame, size);
}
