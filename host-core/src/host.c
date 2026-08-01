#include "wlh/host.h"
#include "wlh/log.h"

#include "host_internal.h"

#include <limits.h>
#include <string.h>

#include "adc.pb.h"
#include "bluetooth.pb.h"
#include "common.pb.h"
#include "device_info.pb.h"
#include "io.pb.h"
#include "kv.pb.h"
#include "link.pb.h"
#include "user_passthrough.pb.h"
#include "wifi.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>

typedef struct wlh_event_task {
    wlh_host_t *host;
    wlh_host_event_t event;
    size_t allocation_size;
    uint8_t payload[];
} wlh_event_task_t;

typedef struct wlh_completion_task {
    wlh_host_t *host;
    wlh_rpc_completion_fn completion;
    void *context;
    wlh_host_result_t result;
    uint16_t status_domain;
    int16_t status_code;
    size_t payload_size;
    size_t allocation_size;
    uint8_t payload[];
} wlh_completion_task_t;

_Static_assert(
    sizeof(wlh_host_job_t) <= sizeof(uintptr_t) * 2u,
    "host queue slot too small"
);

uint64_t now_ms(const wlh_host_t *host) {
    return host->config.osal.monotonic_time_ms(host->config.osal.context);
}

static void event_task_run(void *context) {
    wlh_event_task_t *task = context;
    if (task->host->config.on_event != NULL) {
        task->host->config.on_event(
            task->host->config.event_context, &task->event
        );
    }
    task->host->config.buffers.free(
        task->host->config.buffers.context, (uint8_t *)task
    );
}

static void completion_task_run(void *context) {
    wlh_completion_task_t *task = context;
    task->completion(
        task->context,
        task->result,
        task->status_domain,
        task->status_code,
        task->payload_size == 0u ? NULL : task->payload,
        task->payload_size
    );
    task->host->config.buffers.free(
        task->host->config.buffers.context, (uint8_t *)task
    );
}

bool dispatch_event(
    wlh_host_t *host,
    wlh_host_event_kind_t kind,
    uint16_t service_id,
    uint16_t method_id,
    const uint8_t *payload,
    size_t payload_size
) {
    size_t allocation_size = sizeof(wlh_event_task_t) + payload_size;
    wlh_event_task_t *task = (wlh_event_task_t *)host->config.buffers.alloc(
        host->config.buffers.context, allocation_size
    );
    if (task == NULL) {
        host->diagnostics.buffer_allocation_failures++;
        WLH_LOGW("wlh_host", "event buffer allocation failed");
        return false;
    }
    memset(task, 0, sizeof(*task));
    task->host = host;
    task->event.kind = kind;
    task->event.state = host->state;
    task->event.service_id = service_id;
    task->event.method_id = method_id;
    task->event.payload_size = payload_size;
    task->event.payload = payload_size == 0u ? NULL : task->payload;
    task->allocation_size = allocation_size;
    if (payload_size != 0u) {
        memcpy(task->payload, payload, payload_size);
    }
    if (host->config.executor.post(
            host->config.executor.context, event_task_run, task
        ) != 0) {
        host->config.buffers.free(
            host->config.buffers.context, (uint8_t *)task
        );
        WLH_LOGW("wlh_host", "event executor queue full");
        return false;
    }
    return true;
}

void dispatch_completion(
    wlh_host_t *host,
    wlh_rpc_completion_fn completion,
    void *context,
    wlh_host_result_t result,
    uint16_t status_domain,
    int16_t status_code,
    const uint8_t *payload,
    size_t payload_size
) {
    size_t allocation_size;
    wlh_completion_task_t *task;
    if (completion == NULL) {
        return;
    }
    allocation_size = sizeof(*task) + payload_size;
    task = (wlh_completion_task_t *)host->config.buffers.alloc(
        host->config.buffers.context, allocation_size
    );
    if (task == NULL) {
        host->diagnostics.buffer_allocation_failures++;
        WLH_LOGW("wlh_host", "completion buffer allocation failed");
        completion(
            context, WLH_HOST_NO_MEMORY, 0u, WLH_STATUS_NO_MEMORY, NULL, 0u
        );
        return;
    }
    memset(task, 0, sizeof(*task));
    task->host = host;
    task->completion = completion;
    task->context = context;
    task->result = result;
    task->status_domain = status_domain;
    task->status_code = status_code;
    task->payload_size = payload_size;
    task->allocation_size = allocation_size;
    if (payload_size != 0u) {
        memcpy(task->payload, payload, payload_size);
    }
    if (host->config.executor.post(
            host->config.executor.context, completion_task_run, task
        ) != 0) {
        host->config.buffers.free(
            host->config.buffers.context, (uint8_t *)task
        );
        completion(
            context, WLH_HOST_TRANSPORT_ERROR, 0u, WLH_STATUS_INTERNAL, NULL, 0u
        );
    }
}

static const char *host_state_name(wlh_host_state_t state) {
    switch (state) {
    case WLH_HOST_STATE_UNINITIALIZED:
        return "UNINITIALIZED";
    case WLH_HOST_STATE_TRANSPORT_STARTING:
        return "TRANSPORT_STARTING";
    case WLH_HOST_STATE_WAITING_FOR_PEER:
        return "WAITING_FOR_PEER";
    case WLH_HOST_STATE_NEGOTIATING:
        return "NEGOTIATING";
    case WLH_HOST_STATE_READY:
        return "READY";
    case WLH_HOST_STATE_CONGESTED:
        return "CONGESTED";
    case WLH_HOST_STATE_RECOVERING:
        return "RECOVERING";
    case WLH_HOST_STATE_FAILED:
        return "FAILED";
    case WLH_HOST_STATE_STOPPING:
        return "STOPPING";
    default:
        return "UNKNOWN";
    }
}

static void log_state_transition(
    wlh_host_state_t previous, wlh_host_state_t state
) {
    const char *previous_name = host_state_name(previous);
    const char *state_name = host_state_name(state);
    (void)previous_name;
    (void)state_name;
    WLH_LOGI("wlh_host", "state %s -> %s", previous_name, state_name);
}

void set_state(wlh_host_t *host, wlh_host_state_t state) {
    wlh_host_state_t previous;
    if (host->state == state) {
        return;
    }
    previous = host->state;
    host->state = state;
    host->diagnostics.state = state;
    log_state_transition(previous, state);
    dispatch_event(host, WLH_HOST_EVENT_STATE_CHANGED, 0u, 0u, NULL, 0u);
}

static void host_worker(void *argument) {
    wlh_host_t *host = argument;
    wlh_host_job_t job;
    uint32_t ethernet_transport_failures = 0u;
    (void)host->config.osal.mutex_lock(
        host->config.osal.context, &host->state_mutex, WLH_OSAL_WAIT_FOREVER
    );
    request_transport_start(host);
    host->config.osal.mutex_unlock(
        host->config.osal.context, &host->state_mutex
    );

    while (!host->worker_stopping) {
        uint32_t wait_ms;
        (void)host->config.osal.mutex_lock(
            host->config.osal.context, &host->state_mutex, WLH_OSAL_WAIT_FOREVER
        );
        (void)host_process_deadlines(host);
        (void)flush_ethernet_rx_credits(host);
        wait_ms = host_next_wait_ms(host);
        host->config.osal.mutex_unlock(
            host->config.osal.context, &host->state_mutex
        );

        if (host->config.osal.queue_receive(
                host->config.osal.context, &host->core_queue, &job, wait_ms
            ) == 0) {
            (void)host->config.osal.mutex_lock(
                host->config.osal.context,
                &host->state_mutex,
                WLH_OSAL_WAIT_FOREVER
            );
            if (job.kind == WLH_HOST_JOB_STOP) {
                set_state(host, WLH_HOST_STATE_STOPPING);
                cancel_pending(host, WLH_HOST_SESSION_CHANGED);
                if (host->config.transport.stop(
                        host->config.transport.context,
                        transport_stop_complete,
                        host
                    ) != 0)
                    finish_shutdown(host);
            } else if (job.kind == WLH_HOST_JOB_RX_FRAME) {
                wlh_host_data_job_t *data = job.payload;
                (void)process_frame(host, data->data, data->size);
                host->config.buffers.free(
                    host->config.buffers.context, (uint8_t *)data
                );
            } else if (job.kind == WLH_HOST_JOB_RPC_REQUEST) {
                wlh_host_rpc_job_t *rpc = job.payload;
                wlh_host_result_t result = process_rpc_request(host, rpc);
                if (result != WLH_HOST_OK)
                    dispatch_completion(
                        host,
                        rpc->completion,
                        rpc->completion_context,
                        result,
                        WLH_STATUS_DOMAIN_LINK,
                        WLH_STATUS_INTERNAL,
                        NULL,
                        0u
                    );
                host->config.buffers.free(
                    host->config.buffers.context, (uint8_t *)rpc
                );
            } else if (job.kind == WLH_HOST_JOB_ETHERNET_TX) {
                wlh_host_data_job_t *data = job.payload;
                wlh_host_result_t result;
                uint8_t ethernet_index =
                    data->channel == WLH_CHANNEL_ETHERNET_STA ? 0u : 1u;
                if (host->ethernet_tx_queued[ethernet_index] > 0u)
                    --host->ethernet_tx_queued[ethernet_index];
                result = send_payload_frame_units(
                    host, data->channel, data->data, data->size, false, 1u
                );
                if (result != WLH_HOST_OK) {
                    (void)host->config.osal.semaphore_give(
                        host->config.osal.context, &host->ethernet_tx_slots
                    );
                }
                if (result != WLH_HOST_OK) {
                    ++ethernet_transport_failures;
                }
                if (result != WLH_HOST_OK &&
                    (ethernet_transport_failures <= 5u ||
                     ethernet_transport_failures % 100u == 0u))
                    WLH_LOGW(
                        "wlh_host",
                        "ethernet TX failed channel=%u result=%d credit=%lu",
                        (unsigned)data->channel,
                        (int)result,
                        (unsigned long)host->tx_credit[data->channel]
                    );
                host->config.buffers.free(
                    host->config.buffers.context, (uint8_t *)data
                );
            } else if (job.kind == WLH_HOST_JOB_BLUETOOTH_TX) {
                wlh_host_data_job_t *data = job.payload;
                if (host->bluetooth_tx_inflight > 0u)
                    host->bluetooth_tx_inflight--;
                (void)send_payload_frame(
                    host, data->channel, data->data, data->size, false
                );
                host->config.buffers.free(
                    host->config.buffers.context, (uint8_t *)data
                );
            } else if (job.kind == WLH_HOST_JOB_OTA_TX) {
                wlh_host_data_job_t *data = job.payload;
                if (host->ota_tx_inflight > 0u)
                    host->ota_tx_inflight--;
                (void)send_payload_frame(
                    host, data->channel, data->data, data->size, false
                );
                host->config.buffers.free(
                    host->config.buffers.context, (uint8_t *)data
                );
            } else if (job.kind == WLH_HOST_JOB_TRANSPORT_LOST) {
                process_transport_lost(host);
            } else if (job.kind == WLH_HOST_JOB_TRANSPORT_STARTED) {
                if (host->state != WLH_HOST_STATE_STOPPING) {
                    host->started_ms = now_ms(host);
                    set_state(host, WLH_HOST_STATE_WAITING_FOR_PEER);
                    if (send_hello(host) != WLH_HOST_OK)
                        set_state(host, WLH_HOST_STATE_FAILED);
                }
            } else if (job.kind == WLH_HOST_JOB_TRANSPORT_START_FAILED) {
                if (host->state != WLH_HOST_STATE_STOPPING)
                    set_state(host, WLH_HOST_STATE_FAILED);
            } else if (job.kind == WLH_HOST_JOB_TRANSPORT_STOPPED) {
                if (host->state == WLH_HOST_STATE_STOPPING)
                    finish_shutdown(host);
                else if (host->state == WLH_HOST_STATE_RECOVERING)
                    request_transport_start(host);
            } else if (job.kind == WLH_HOST_JOB_TRANSPORT_STOP_FAILED) {
                if (host->state == WLH_HOST_STATE_STOPPING)
                    finish_shutdown(host);
                else
                    set_state(host, WLH_HOST_STATE_FAILED);
            }
            host->config.osal.mutex_unlock(
                host->config.osal.context, &host->state_mutex
            );
        }
    }

    (void)host->config.osal.mutex_lock(
        host->config.osal.context, &host->state_mutex, WLH_OSAL_WAIT_FOREVER
    );
    if (host->state != WLH_HOST_STATE_UNINITIALIZED)
        finish_shutdown(host);
    host->config.osal.mutex_unlock(
        host->config.osal.context, &host->state_mutex
    );
}

wlh_host_result_t wlh_host_init(
    wlh_host_t *host, const wlh_host_config_t *config
) {
    // clang-format off
    if (host == NULL || config == NULL ||
        config->transport.start == NULL ||
        config->transport.stop == NULL ||
        config->transport.submit_tx == NULL ||
        config->buffers.alloc == NULL ||
        config->buffers.free == NULL ||
        !wlh_osal_ops_valid(&config->osal) ||
        config->executor.post == NULL ||
        config->max_frame_size < WLH_FRAME_HEADER_SIZE ||
        config->max_frame_size > WLH_FRAME_MAX_SIZE ||
        config->max_pending_rpc == 0u ||
        config->max_pending_rpc > WLH_HOST_MAX_PENDING) {
        // clang-format on
        return WLH_HOST_INVALID_ARGUMENT;
    }
    memset(host, 0, sizeof(*host));
    host->config = *config;
    if (host->config.core_queue_depth == 0u)
        host->config.core_queue_depth = 16u;
    if (host->config.core_queue_depth > WLH_HOST_MAX_QUEUE_DEPTH)
        return WLH_HOST_INVALID_ARGUMENT;
    if (host->config.ethernet_tx_depth == 0u)
        host->config.ethernet_tx_depth = host->config.core_queue_depth;
    if (host->config.stop_timeout_ms == 0u)
        host->config.stop_timeout_ms = 3000u;
    host->state = WLH_HOST_STATE_UNINITIALIZED;
    host->diagnostics.state = host->state;
    host->next_request_id = 1u;
    return WLH_HOST_OK;
}

wlh_host_result_t wlh_host_start(wlh_host_t *host) {
    wlh_osal_task_attributes_t attributes;
    if (host == NULL || host->state != WLH_HOST_STATE_UNINITIALIZED) {
        return WLH_HOST_INVALID_STATE;
    }
    if (host->config.osal.mutex_create(
            host->config.osal.context, &host->state_mutex
        ) != 0)
        return WLH_HOST_INVALID_STATE;
    if (host->config.osal.queue_create(
            host->config.osal.context,
            &host->core_queue,
            host->core_queue_storage,
            sizeof(wlh_host_job_t),
            host->config.core_queue_depth
        ) != 0) {
        host->config.osal.mutex_destroy(
            host->config.osal.context, &host->state_mutex
        );
        return WLH_HOST_INVALID_STATE;
    }
    if (host->config.osal.semaphore_create(
            host->config.osal.context,
            &host->ethernet_tx_slots,
            host->config.ethernet_tx_depth,
            host->config.ethernet_tx_depth
        ) != 0) {
        host->config.osal.queue_destroy(
            host->config.osal.context, &host->core_queue
        );
        host->config.osal.mutex_destroy(
            host->config.osal.context, &host->state_mutex
        );
        return WLH_HOST_INVALID_STATE;
    }
    host->worker_stopping = false;
    host->worker_started = true;
    attributes = host->config.core_task;
    if (attributes.name == NULL)
        attributes.name = "wlh-host-core";
    if (host->config.osal.task_create(
            host->config.osal.context,
            &host->core_task,
            &attributes,
            host_worker,
            host
        ) != 0) {
        host->worker_started = false;
        host->config.osal.queue_destroy(
            host->config.osal.context, &host->core_queue
        );
        host->config.osal.semaphore_destroy(
            host->config.osal.context, &host->ethernet_tx_slots
        );
        host->config.osal.mutex_destroy(
            host->config.osal.context, &host->state_mutex
        );
        return WLH_HOST_INVALID_STATE;
    }
    return WLH_HOST_OK;
}

wlh_host_result_t wlh_host_stop(wlh_host_t *host) {
    if (host == NULL) {
        return WLH_HOST_INVALID_ARGUMENT;
    }
    if (!host->worker_started)
        return WLH_HOST_INVALID_STATE;
    if (enqueue_job(host, WLH_HOST_JOB_STOP, NULL, 100u) != 0)
        return WLH_HOST_INVALID_STATE;
    if (host->config.osal.task_join(
            host->config.osal.context,
            &host->core_task,
            host->config.stop_timeout_ms
        ) != 0)
        return WLH_HOST_TIMEOUT;
    host->worker_started = false;
    host->config.osal.queue_destroy(
        host->config.osal.context, &host->core_queue
    );
    host->config.osal.semaphore_destroy(
        host->config.osal.context, &host->ethernet_tx_slots
    );
    host->config.osal.mutex_destroy(
        host->config.osal.context, &host->state_mutex
    );
    return WLH_HOST_OK;
}

void wlh_host_get_diagnostics(
    const wlh_host_t *host, wlh_host_diagnostics_t *diagnostics
) {
    size_t index;
    if (host == NULL || diagnostics == NULL)
        return;
    if (host->worker_started)
        (void)host->config.osal.mutex_lock(
            host->config.osal.context,
            (wlh_osal_mutex_t *)&host->state_mutex,
            WLH_OSAL_WAIT_FOREVER
        );
    *diagnostics = host->diagnostics;
    diagnostics->pending_rpc = 0u;
    for (index = 0; index < WLH_HOST_MAX_PENDING; ++index)
        diagnostics->pending_rpc += host->pending[index].active ? 1u : 0u;
    if (host->worker_started)
        host->config.osal.mutex_unlock(
            host->config.osal.context, (wlh_osal_mutex_t *)&host->state_mutex
        );
}

#ifdef WLH_ENABLE_TEST_HOOKS
void wlh_host_test_set_credit(
    wlh_host_t *host, uint8_t channel, uint32_t credit
) {
    if (host != NULL && host->worker_started) {
        (void)host->config.osal.mutex_lock(
            host->config.osal.context, &host->state_mutex, WLH_OSAL_WAIT_FOREVER
        );
        host->tx_credit[channel] = credit;
        host->config.osal.mutex_unlock(
            host->config.osal.context, &host->state_mutex
        );
    }
}
void wlh_host_test_force_session_change(wlh_host_t *host, uint32_t session_id) {
    if (host == NULL || !host->worker_started || session_id == 0u)
        return;
    (void)host->config.osal.mutex_lock(
        host->config.osal.context, &host->state_mutex, WLH_OSAL_WAIT_FOREVER
    );
    if (session_id == host->session_id) {
        host->config.osal.mutex_unlock(
            host->config.osal.context, &host->state_mutex
        );
        return;
    }
    cancel_pending(host, WLH_HOST_SESSION_CHANGED);
    host->session_id = session_id;
    host->diagnostics.session_id = session_id;
    host->diagnostics.peer_resets++;
    host->config.osal.mutex_unlock(
        host->config.osal.context, &host->state_mutex
    );
}
void wlh_host_test_expire_all(wlh_host_t *host) {
    size_t index;
    if (host == NULL || !host->worker_started)
        return;
    (void)host->config.osal.mutex_lock(
        host->config.osal.context, &host->state_mutex, WLH_OSAL_WAIT_FOREVER
    );
    for (index = 0; index < WLH_HOST_MAX_PENDING; ++index)
        if (host->pending[index].active)
            host->pending[index].deadline_ms = 0u;
    host->config.osal.mutex_unlock(
        host->config.osal.context, &host->state_mutex
    );
}
#endif
