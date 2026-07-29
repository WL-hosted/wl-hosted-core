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

static wlh_coproc_result_t ethernet_send(
    wlh_coproc_t *coproc, uint8_t channel, const uint8_t *frame, size_t size
) {
    coproc_data_job_t *job;
    uint8_t ethernet_limit;

    if (coproc == NULL || frame == NULL ||
        size + RAW_HEADER_SIZE >
            WLH_COPROC_MAX_FRAME_SIZE - WLH_FRAME_HEADER_SIZE ||
        !coproc->worker_started) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }
    job = (coproc_data_job_t *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*job) + RAW_HEADER_SIZE + size
    );
    if (job == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    memset(job, 0, sizeof(*job));
    job->channel = channel;
    {
        size_t record_size = 0;
        if (wlh_raw_record_encode(
                job->data,
                RAW_HEADER_SIZE + size,
                &record_size,
                1u,
                0u,
                frame,
                size
            ) != WLH_WIRE_OK) {
            coproc->config.buffers.free(
                coproc->config.buffers.context, (uint8_t *)job
            );
            return WLH_COPROC_INVALID_ARGUMENT;
        }
        job->size = record_size;
        job->capacity = record_size;
    }
    /* Wi-Fi RX can arrive in bursts while a Host is negotiating or issuing a
     * control RPC. Ethernet is explicitly best-effort, whereas a dropped
     * control request loses the only response and strands the Host. Reserve
     * bounded core-queue capacity for control/completion jobs. Holding the
     * same mutex as the worker makes the count and enqueue atomic with
     * respect to dequeuing an Ethernet job. */
    ethernet_limit =
        coproc->config.core_queue_depth > WLH_COPROC_CONTROL_QUEUE_RESERVE
            ? (uint8_t)(coproc->config.core_queue_depth -
                        WLH_COPROC_CONTROL_QUEUE_RESERVE)
            : 1u;
    if (coproc->config.osal.mutex_lock(
            coproc->config.osal.context, &coproc->state_mutex, WLH_OSAL_NO_WAIT
        ) != 0) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)job
        );
        return WLH_COPROC_BACKEND_ERROR;
    }
    if (coproc->ethernet_tx_jobs_pending >= ethernet_limit ||
        enqueue_job(coproc, COPROC_JOB_ETHERNET_TX, job, WLH_OSAL_NO_WAIT) !=
            0) {
        coproc->config.osal.mutex_unlock(
            coproc->config.osal.context, &coproc->state_mutex
        );
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)job
        );
        return WLH_COPROC_BACKEND_ERROR;
    }
    ++coproc->ethernet_tx_jobs_pending;
    coproc->config.osal.mutex_unlock(
        coproc->config.osal.context, &coproc->state_mutex
    );
    return WLH_COPROC_OK;
}

wlh_coproc_result_t wlh_coproc_ethernet_sta_send(
    wlh_coproc_t *coproc, const uint8_t *frame, size_t size
) {
    return ethernet_send(coproc, WLH_CHANNEL_ETHERNET_STA, frame, size);
}

wlh_coproc_result_t wlh_coproc_ethernet_ap_send(
    wlh_coproc_t *coproc, const uint8_t *frame, size_t size
) {
    return ethernet_send(coproc, WLH_CHANNEL_ETHERNET_AP, frame, size);
}
