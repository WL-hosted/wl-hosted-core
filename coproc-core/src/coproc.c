#include "wlh/coproc.h"
#include "wlh/log.h"

#include "coproc_internal.h"

#include <limits.h>
#include <string.h>

#include "adc.pb.h"
#include "bluetooth.pb.h"
#include "device_info.pb.h"
#include "diagnostics.pb.h"
#include "io.pb.h"
#include "kv.pb.h"
#include "link.pb.h"
#include "ota.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "user_passthrough.pb.h"
#include "wifi.pb.h"

/* Bound for caller-supplied SSIDs, taken from the nanopb field so it tracks
   the schema. WifiNetwork and WifiLinkInfo must agree for MAX_SSID_SIZE to be
   a valid bound for both event shapes. */
_Static_assert(
    sizeof(((wlh_protocol_v1_WifiNetwork *)0)->ssid.bytes) ==
        WLH_COPROC_MAX_SSID_SIZE,
    "WifiNetwork and WifiLinkInfo ssid bounds diverged"
);

/* The adapter contract is NUL-terminated strings, so every KV bound is one
   byte smaller than the nanopb field that carries it. */
_Static_assert(
    sizeof(((wlh_protocol_v1_KvWriteRequest *)0)->key) ==
        WLH_COPROC_KV_MAX_KEY_SIZE + 1u,
    "KV key bound diverged from the schema"
);
_Static_assert(
    sizeof(((wlh_protocol_v1_KvReadResponse *)0)->value) ==
        WLH_COPROC_KV_MAX_VALUE_SIZE + 1u,
    "KV value bound diverged from the schema"
);

/* A BSS is only usable if its SSID fits the schema bound and is non-NULL when
   non-empty; every event path copies it into a fixed-size nanopb field. */

_Static_assert(
    sizeof(coproc_job_t) <= sizeof(uintptr_t) * 2u,
    "coprocessor queue slot too small"
);

static const char *coproc_state_name(wlh_coproc_state_t state) {
    switch (state) {
    case WLH_COPROC_STATE_STOPPED:
        return "STOPPED";
    case WLH_COPROC_STATE_WAITING_FOR_HELLO:
        return "WAITING_FOR_HELLO";
    case WLH_COPROC_STATE_READY:
        return "READY";
    case WLH_COPROC_STATE_CONGESTED:
        return "CONGESTED";
    case WLH_COPROC_STATE_FAILED:
        return "FAILED";
    default:
        return "UNKNOWN";
    }
}

static void log_state_transition(
    wlh_coproc_state_t previous, wlh_coproc_state_t state
) {
    const char *previous_name = coproc_state_name(previous);
    const char *state_name = coproc_state_name(state);
    (void)previous_name;
    (void)state_name;
    WLH_LOGI("wlh_coproc", "state %s -> %s", previous_name, state_name);
}

void set_state(wlh_coproc_t *coproc, wlh_coproc_state_t state) {
    wlh_coproc_state_t previous;
    if (coproc->state == state) {
        return;
    }
    previous = coproc->state;
    coproc->state = state;
    coproc->diagnostics.state = state;
    log_state_transition(previous, state);
}

uint64_t now_ms(const wlh_coproc_t *coproc) {
    return coproc->config.osal.monotonic_time_ms(coproc->config.osal.context);
}

int enqueue_job(
    wlh_coproc_t *coproc,
    coproc_job_kind_t kind,
    void *payload,
    uint32_t timeout_ms
) {
    coproc_job_t job = {(uint32_t)kind, payload};
    if (coproc == NULL || !coproc->worker_started)
        return -1;
    return coproc->config.osal.queue_send(
        coproc->config.osal.context, &coproc->core_queue, &job, timeout_ms
    );
}

/* Combine already-queued Ethernet records into one payload.  Credits and
 * sequences belong to wire frames, not raw records, so this deliberately
 * turns two or more queued jobs into one send_payload() call.  The queue is
 * FIFO: as soon as a non-Ethernet job, a different channel, or a record that
 * does not fit is observed, retain it as the worker's next job rather than
 * reordering it behind later traffic.
 *
 * This runs with state_mutex held.  ethernet_send() takes the same mutex
 * while admitting jobs, making the pending counter exact and ensuring the
 * source jobs cannot be modified while their record bytes are copied. */
static coproc_data_job_t *aggregate_ethernet_jobs(
    wlh_coproc_t *coproc,
    coproc_data_job_t *data,
    coproc_job_t *deferred,
    bool *has_deferred
) {
    const size_t payload_capacity =
        coproc->config.max_frame_size - WLH_FRAME_HEADER_SIZE;
    coproc_job_t next;

    while (*has_deferred == false && coproc->config.osal.queue_receive(
                                         coproc->config.osal.context,
                                         &coproc->core_queue,
                                         &next,
                                         WLH_OSAL_NO_WAIT
                                     ) == 0) {
        coproc_data_job_t *candidate;

        if (next.kind != COPROC_JOB_ETHERNET_TX) {
            *deferred = next;
            *has_deferred = true;
            break;
        }
        candidate = next.payload;
        if (candidate->channel != data->channel ||
            data->size > payload_capacity ||
            candidate->size > payload_capacity - data->size) {
            *deferred = next;
            *has_deferred = true;
            break;
        }
        if (data->capacity < payload_capacity) {
            coproc_data_job_t *expanded =
                (coproc_data_job_t *)coproc->config.buffers.alloc(
                    coproc->config.buffers.context,
                    sizeof(*expanded) + payload_capacity
                );
            if (expanded == NULL) {
                *deferred = next;
                *has_deferred = true;
                break;
            }
            memset(expanded, 0, sizeof(*expanded));
            expanded->channel = data->channel;
            expanded->size = data->size;
            expanded->capacity = payload_capacity;
            memcpy(expanded->data, data->data, data->size);
            coproc->config.buffers.free(
                coproc->config.buffers.context, (uint8_t *)data
            );
            data = expanded;
        }
        memcpy(data->data + data->size, candidate->data, candidate->size);
        data->size += candidate->size;
        if (coproc->ethernet_tx_jobs_pending > 0u)
            --coproc->ethernet_tx_jobs_pending;
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)candidate
        );
    }
    return data;
}

static void coproc_worker(void *argument) {
    wlh_coproc_t *coproc = argument;
    coproc_job_t job;
    coproc_job_t deferred_job;
    bool has_deferred_job = false;
    (void)coproc->config.osal.mutex_lock(
        coproc->config.osal.context, &coproc->state_mutex, WLH_OSAL_WAIT_FOREVER
    );
    set_state(coproc, WLH_COPROC_STATE_WAITING_FOR_HELLO);
    coproc->started_ms = now_ms(coproc);
    coproc->last_heartbeat_ms = coproc->started_ms;
    coproc->config.osal.mutex_unlock(
        coproc->config.osal.context, &coproc->state_mutex
    );

    while (!coproc->worker_stopping) {
        uint32_t wait_ms;
        (void)coproc->config.osal.mutex_lock(
            coproc->config.osal.context,
            &coproc->state_mutex,
            WLH_OSAL_WAIT_FOREVER
        );
        (void)coproc_emit_due_heartbeat(coproc);
        wait_ms = coproc_next_wait_ms(coproc);
        coproc->config.osal.mutex_unlock(
            coproc->config.osal.context, &coproc->state_mutex
        );

        if ((has_deferred_job ||
             coproc->config.osal.queue_receive(
                 coproc->config.osal.context, &coproc->core_queue, &job, wait_ms
             ) == 0)) {
            if (has_deferred_job) {
                job = deferred_job;
                has_deferred_job = false;
            }
            (void)coproc->config.osal.mutex_lock(
                coproc->config.osal.context,
                &coproc->state_mutex,
                WLH_OSAL_WAIT_FOREVER
            );
            if (job.kind == COPROC_JOB_STOP) {
                coproc->worker_stopping = true;
            } else if (job.kind == COPROC_JOB_RX_FRAME) {
                coproc_data_job_t *data = job.payload;
                wlh_coproc_result_t result =
                    process_frame(coproc, data->data, data->size);
                if (result != WLH_COPROC_OK) {
                    WLH_LOGW(
                        "wlh_coproc",
                        "RX frame processing failed: %d",
                        (int)result
                    );
                }
                coproc->config.buffers.free(
                    coproc->config.buffers.context, (uint8_t *)data
                );
            } else if (job.kind == COPROC_JOB_WIFI_INITIALIZED) {
                coproc_wifi_initialized_job_t *completed = job.payload;
                if (coproc->wifi_initialize_pending.active &&
                    completed->operation_id ==
                        coproc->wifi_initialize_pending.operation_id &&
                    coproc->wifi_initialize_pending.session_id ==
                        coproc->session_id) {
                    wlh_rpc_envelope_t request;
                    memset(&request, 0, sizeof(request));
                    request.service_id = WLH_SERVICE_WIFI;
                    request.method_id = WLH_WIFI_METHOD_INITIALIZE;
                    request.request_id =
                        coproc->wifi_initialize_pending.request_id;
                    request.kind = WLH_RPC_KIND_REQUEST;
                    memset(
                        &coproc->wifi_initialize_pending,
                        0,
                        sizeof(coproc->wifi_initialize_pending)
                    );
                    (void)send_status(
                        coproc, &request, completed->backend_status
                    );
                }
                coproc->config.buffers.free(
                    coproc->config.buffers.context, (uint8_t *)completed
                );
            } else if (job.kind == COPROC_JOB_RPC_EVENT) {
                coproc_data_job_t *data = job.payload;
                (void)send_rpc(
                    coproc,
                    data->service_id,
                    data->method_id,
                    0u,
                    WLH_RPC_KIND_EVENT,
                    0u,
                    0,
                    data->data,
                    data->size
                );
                coproc->config.buffers.free(
                    coproc->config.buffers.context, (uint8_t *)data
                );
            } else if (job.kind == COPROC_JOB_ETHERNET_TX) {
                coproc_data_job_t *data = job.payload;
                data = aggregate_ethernet_jobs(
                    coproc, data, &deferred_job, &has_deferred_job
                );
                if (coproc->ethernet_tx_jobs_pending > 0u)
                    --coproc->ethernet_tx_jobs_pending;
                (void)send_payload(
                    coproc, data->channel, data->data, data->size
                );
                coproc->config.buffers.free(
                    coproc->config.buffers.context, (uint8_t *)data
                );
            } else if (job.kind == COPROC_JOB_BLUETOOTH_COMPLETE) {
                coproc_bluetooth_complete_job_t *completed = job.payload;
                bluetooth_operation_completed(coproc, completed);
                coproc->config.buffers.free(
                    coproc->config.buffers.context, (uint8_t *)completed
                );
            } else if (job.kind == COPROC_JOB_BLUETOOTH_INFO) {
                coproc_bluetooth_info_job_t *completed = job.payload;
                bluetooth_info_completed(coproc, completed);
                coproc->config.buffers.free(
                    coproc->config.buffers.context, (uint8_t *)completed
                );
            } else if (job.kind == COPROC_JOB_BLUETOOTH_TX) {
                coproc_data_job_t *data = job.payload;
                if (data->channel == WLH_CHANNEL_BLUETOOTH_HCI_ADV) {
                    if (coproc->bluetooth_adv_tx_inflight > 0u)
                        --coproc->bluetooth_adv_tx_inflight;
                } else if (coproc->bluetooth_tx_inflight > 0u) {
                    --coproc->bluetooth_tx_inflight;
                }
                if (coproc->bluetooth_hci_stopped) {
                    ++coproc->diagnostics.hci_drops;
                } else {
                    (void)send_payload(
                        coproc, data->channel, data->data, data->size
                    );
                }
                coproc->config.buffers.free(
                    coproc->config.buffers.context, (uint8_t *)data
                );
            } else if (job.kind == COPROC_JOB_BLUETOOTH_FATAL) {
                coproc_bluetooth_fatal_job_t *fatal = job.payload;
                bluetooth_enter_error(coproc, fatal->reason);
                coproc->config.buffers.free(
                    coproc->config.buffers.context, (uint8_t *)fatal
                );
            } else if (job.kind == COPROC_JOB_OTA_COMPLETE) {
                coproc_ota_complete_job_t *completed = job.payload;
                ota_operation_completed(coproc, completed);
                coproc->config.buffers.free(
                    coproc->config.buffers.context, (uint8_t *)completed
                );
            } else if (job.kind == COPROC_JOB_OTA_WRITE_COMPLETE) {
                coproc_ota_write_job_t *completed = job.payload;
                ota_write_completed(coproc, completed);
                coproc->config.buffers.free(
                    coproc->config.buffers.context, (uint8_t *)completed
                );
            } else if (job.kind == COPROC_JOB_TRANSPORT_FAILED) {
                WLH_LOGW("wlh_coproc", "transport failed");
                set_state(coproc, WLH_COPROC_STATE_FAILED);
            }
            coproc->config.osal.mutex_unlock(
                coproc->config.osal.context, &coproc->state_mutex
            );
        }
    }

    (void)coproc->config.osal.mutex_lock(
        coproc->config.osal.context, &coproc->state_mutex, WLH_OSAL_WAIT_FOREVER
    );
    set_state(coproc, WLH_COPROC_STATE_STOPPED);
    coproc->session_id = 0u;
    coproc->config.osal.mutex_unlock(
        coproc->config.osal.context, &coproc->state_mutex
    );
}

wlh_coproc_result_t wlh_coproc_init(
    wlh_coproc_t *coproc, const wlh_coproc_config_t *config
) {
    if (coproc == NULL || config == NULL || config->port.submit_tx == NULL ||
        config->buffers.alloc == NULL || config->buffers.free == NULL ||
        !wlh_osal_ops_valid(&config->osal) ||
        config->heartbeat_interval_ms == 0u ||
        config->max_frame_size < WLH_FRAME_HEADER_SIZE ||
        config->max_frame_size > WLH_COPROC_MAX_FRAME_SIZE) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }

    /* The Bluetooth backend is all-or-none: advertising the service with a
       partial ops table would strand lifecycle requests. hci_tx_ready stays
       optional. */
    if ((config->bluetooth.initialize != NULL ||
         config->bluetooth.enable != NULL ||
         config->bluetooth.disable != NULL ||
         config->bluetooth.deinitialize != NULL ||
         config->bluetooth.get_info != NULL ||
         config->bluetooth.hci_send != NULL) &&
        (config->bluetooth.initialize == NULL ||
         config->bluetooth.enable == NULL ||
         config->bluetooth.disable == NULL ||
         config->bluetooth.deinitialize == NULL ||
         config->bluetooth.get_info == NULL ||
         config->bluetooth.hci_send == NULL)) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }

    /* The OTA backend is all-or-none as well: begin/write/finalize/activate
       are required together. abort is optional; ota_reset and the ABORT
       handler fall back to a synchronous local reset when it is absent. */
    if ((config->ota.begin != NULL || config->ota.write != NULL ||
         config->ota.finalize != NULL || config->ota.activate != NULL ||
         config->ota.abort != NULL) &&
        (config->ota.begin == NULL || config->ota.write == NULL ||
         config->ota.finalize == NULL || config->ota.activate == NULL)) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }

    memset(coproc, 0, sizeof(*coproc));
    coproc->config = *config;
    if (coproc->config.core_queue_depth == 0u)
        coproc->config.core_queue_depth = 16u;
    if (coproc->config.core_queue_depth > WLH_COPROC_MAX_QUEUE_DEPTH)
        return WLH_COPROC_INVALID_ARGUMENT;
    if (coproc->config.stop_timeout_ms == 0u)
        coproc->config.stop_timeout_ms = 3000u;
    coproc->next_session_id =
        config->initial_session_id != 0u ? config->initial_session_id : 1u;
    coproc->next_backend_operation_id = 1u;
    coproc->next_ota_transfer_id = 1u;
    coproc->ota_state = (uint32_t)wlh_protocol_v1_OtaState_OTA_STATE_IDLE;
    return WLH_COPROC_OK;
}

wlh_coproc_result_t wlh_coproc_start(wlh_coproc_t *coproc) {
    wlh_osal_task_attributes_t attributes;
    if (coproc == NULL || coproc->state != WLH_COPROC_STATE_STOPPED) {
        return WLH_COPROC_INVALID_STATE;
    }

    if (coproc->config.osal.mutex_create(
            coproc->config.osal.context, &coproc->state_mutex
        ) != 0)
        return WLH_COPROC_INVALID_STATE;
    if (coproc->config.osal.queue_create(
            coproc->config.osal.context,
            &coproc->core_queue,
            coproc->core_queue_storage,
            sizeof(coproc_job_t),
            coproc->config.core_queue_depth
        ) != 0) {
        coproc->config.osal.mutex_destroy(
            coproc->config.osal.context, &coproc->state_mutex
        );
        return WLH_COPROC_INVALID_STATE;
    }
    coproc->worker_stopping = false;
    coproc->worker_started = true;
    attributes = coproc->config.core_task;
    if (attributes.name == NULL)
        attributes.name = "wlh-coproc-core";
    if (coproc->config.osal.task_create(
            coproc->config.osal.context,
            &coproc->core_task,
            &attributes,
            coproc_worker,
            coproc
        ) != 0) {
        coproc->worker_started = false;
        coproc->config.osal.queue_destroy(
            coproc->config.osal.context, &coproc->core_queue
        );
        coproc->config.osal.mutex_destroy(
            coproc->config.osal.context, &coproc->state_mutex
        );
        return WLH_COPROC_INVALID_STATE;
    }
    return WLH_COPROC_OK;
}

wlh_coproc_result_t wlh_coproc_stop(wlh_coproc_t *coproc) {
    if (coproc == NULL) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }
    if (!coproc->worker_started ||
        enqueue_job(coproc, COPROC_JOB_STOP, NULL, 100u) != 0)
        return WLH_COPROC_INVALID_STATE;
    if (coproc->config.osal.task_join(
            coproc->config.osal.context,
            &coproc->core_task,
            coproc->config.stop_timeout_ms
        ) != 0)
        return WLH_COPROC_BACKEND_ERROR;
    coproc->worker_started = false;
    coproc->config.osal.queue_destroy(
        coproc->config.osal.context, &coproc->core_queue
    );
    coproc->config.osal.mutex_destroy(
        coproc->config.osal.context, &coproc->state_mutex
    );
    return WLH_COPROC_OK;
}

/* Host->Controller H4 shape checks. `payload` excludes the H4 type octet. */

void wlh_coproc_get_diagnostics(
    const wlh_coproc_t *coproc, wlh_coproc_diagnostics_t *diagnostics
) {
    if (coproc != NULL && diagnostics != NULL) {
        if (coproc->worker_started)
            (void)coproc->config.osal.mutex_lock(
                coproc->config.osal.context,
                (wlh_osal_mutex_t *)&coproc->state_mutex,
                WLH_OSAL_WAIT_FOREVER
            );
        *diagnostics = coproc->diagnostics;
        diagnostics->state = coproc->state;
        diagnostics->session_id = coproc->session_id;
        if (coproc->worker_started)
            coproc->config.osal.mutex_unlock(
                coproc->config.osal.context,
                (wlh_osal_mutex_t *)&coproc->state_mutex
            );
    }
}

#ifdef WLH_ENABLE_TEST_HOOKS
void wlh_coproc_test_set_credit(
    wlh_coproc_t *coproc, uint8_t channel, uint32_t credit
) {
    if (coproc != NULL && coproc->worker_started) {
        (void)coproc->config.osal.mutex_lock(
            coproc->config.osal.context,
            &coproc->state_mutex,
            WLH_OSAL_WAIT_FOREVER
        );
        coproc->tx_credit[channel] = credit;
        coproc->config.osal.mutex_unlock(
            coproc->config.osal.context, &coproc->state_mutex
        );
    }
}

void wlh_coproc_test_reset_channel(wlh_coproc_t *coproc, uint8_t channel) {
    if (coproc != NULL && coproc->worker_started) {
        (void)coproc->config.osal.mutex_lock(
            coproc->config.osal.context,
            &coproc->state_mutex,
            WLH_OSAL_WAIT_FOREVER
        );
        coproc->tx_sequence[channel] = 0u;
        coproc->rx_sequence[channel] = 0u;
        coproc->rx_sequence_valid[channel] = false;
        coproc->config.osal.mutex_unlock(
            coproc->config.osal.context, &coproc->state_mutex
        );
    }
}

void wlh_coproc_test_reset_session(wlh_coproc_t *coproc, uint32_t reason) {
    if (coproc != NULL && coproc->worker_started) {
        uint8_t payload[64];
        size_t payload_size = 0;
        wlh_protocol_v1_SessionChangedEvent event =
            wlh_protocol_v1_SessionChangedEvent_init_zero;

        (void)coproc->config.osal.mutex_lock(
            coproc->config.osal.context,
            &coproc->state_mutex,
            WLH_OSAL_WAIT_FOREVER
        );

        event.old_session_id = coproc->session_id;
        event.new_session_id = coproc->next_session_id;
        event.reset_reason = reason;
        event.boot_id = coproc->next_session_id;

        if (coproc->state == WLH_COPROC_STATE_READY &&
            encode_message(
                payload,
                sizeof(payload),
                &payload_size,
                wlh_protocol_v1_SessionChangedEvent_fields,
                &event
            )) {
            (void)send_rpc(
                coproc,
                WLH_SERVICE_LINK,
                WLH_LINK_EVENT_SESSION_CHANGED,
                0u,
                WLH_RPC_KIND_EVENT,
                0u,
                0,
                payload,
                payload_size
            );
        }

        ++coproc->diagnostics.peer_resets;
        coproc->session_id = 0u;
        memset(
            &coproc->bluetooth_pending, 0, sizeof(coproc->bluetooth_pending)
        );
        coproc->bluetooth_state = BT_STATE_UNSPECIFIED;
        coproc->bluetooth_tx_inflight = 0u;
        coproc->bluetooth_hci_stopped = false;
        set_state(coproc, WLH_COPROC_STATE_WAITING_FOR_HELLO);
        coproc->config.osal.mutex_unlock(
            coproc->config.osal.context, &coproc->state_mutex
        );
    }
}
#endif
