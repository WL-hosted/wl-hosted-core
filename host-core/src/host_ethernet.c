#include "host_internal.h"

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
    if (host == NULL || ethernet_frame == NULL || size == 0u || size > 1600u ||
        !host->worker_started)
        return WLH_HOST_INVALID_ARGUMENT;
    record = host->config.buffers.alloc(
        host->config.buffers.context, sizeof(wlh_host_data_job_t) + 8u + size
    );
    if (record == NULL)
        return WLH_HOST_NO_MEMORY;
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
            return WLH_HOST_INVALID_ARGUMENT;
        }
        job->size = record_size;
        if (enqueue_job(
                host, WLH_HOST_JOB_ETHERNET_TX, job, WLH_OSAL_NO_WAIT
            ) != 0) {
            host->config.buffers.free(
                host->config.buffers.context, (uint8_t *)job
            );
            return WLH_HOST_PENDING_FULL;
        }
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
