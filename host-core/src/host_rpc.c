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

void cancel_pending(wlh_host_t *host, wlh_host_result_t result) {
    size_t index;
    for (index = 0; index < WLH_HOST_MAX_PENDING; ++index) {
        wlh_pending_rpc_t pending = host->pending[index];
        if (!pending.active) {
            continue;
        }
        memset(&host->pending[index], 0, sizeof(host->pending[index]));
        dispatch_completion(
            host,
            pending.completion,
            pending.completion_context,
            result,
            WLH_STATUS_DOMAIN_LINK,
            result == WLH_HOST_TIMEOUT ? WLH_STATUS_TIMEOUT
                                       : WLH_STATUS_SESSION_CHANGED,
            NULL,
            0u
        );
    }
}

static wlh_pending_rpc_t *allocate_pending(wlh_host_t *host) {
    size_t index;
    size_t limit = host->config.max_pending_rpc;
    if (limit > WLH_HOST_MAX_PENDING) {
        limit = WLH_HOST_MAX_PENDING;
    }
    for (index = 0; index < limit; ++index) {
        if (!host->pending[index].active) {
            return &host->pending[index];
        }
    }
    return NULL;
}

wlh_pending_rpc_t *find_pending(
    wlh_host_t *host, const wlh_rpc_envelope_t *envelope
) {
    size_t index;
    for (index = 0; index < WLH_HOST_MAX_PENDING; ++index) {
        wlh_pending_rpc_t *pending = &host->pending[index];
        if (pending->active && pending->session_id == host->session_id &&
            pending->request_id == envelope->request_id &&
            pending->service_id == envelope->service_id &&
            pending->method_id == envelope->method_id) {
            return pending;
        }
    }
    return NULL;
}

wlh_host_result_t host_process_deadlines(wlh_host_t *host) {
    uint64_t current;
    size_t index;
    if (host == NULL) {
        return WLH_HOST_INVALID_ARGUMENT;
    }
    current = now_ms(host);
    for (index = 0; index < WLH_HOST_MAX_PENDING; ++index) {
        wlh_pending_rpc_t pending = host->pending[index];
        if (pending.active && current >= pending.deadline_ms) {
            memset(&host->pending[index], 0, sizeof(host->pending[index]));
            host->diagnostics.rpc_timeouts++;
            WLH_LOGW(
                "wlh_host",
                "RPC timeout service=%u method=%u request=%lu",
                (unsigned)pending.service_id,
                (unsigned)pending.method_id,
                (unsigned long)pending.request_id
            );
            dispatch_completion(
                host,
                pending.completion,
                pending.completion_context,
                WLH_HOST_TIMEOUT,
                WLH_STATUS_DOMAIN_LINK,
                WLH_STATUS_TIMEOUT,
                NULL,
                0u
            );
        }
    }
    if (host->state == WLH_HOST_STATE_READY &&
        host->config.heartbeat_timeout_ms != 0u &&
        current - host->diagnostics.last_peer_activity_ms >=
            host->config.heartbeat_timeout_ms) {
        WLH_LOGW("wlh_host", "heartbeat timeout, recovering");
        cancel_pending(host, WLH_HOST_TIMEOUT);
        process_transport_lost(host);
        return WLH_HOST_TIMEOUT;
    }
    return WLH_HOST_OK;
}

static uint32_t deadline_wait_ms(uint64_t current, uint64_t deadline) {
    uint64_t remaining;
    if (deadline <= current)
        return WLH_OSAL_NO_WAIT;
    remaining = deadline - current;
    if (remaining >= WLH_OSAL_WAIT_FOREVER)
        return WLH_OSAL_WAIT_FOREVER - 1u;
    return (uint32_t)remaining;
}

uint32_t host_next_wait_ms(const wlh_host_t *host) {
    uint64_t current = now_ms(host);
    uint64_t nearest = UINT64_MAX;
    size_t index;

    for (index = 0; index < WLH_HOST_MAX_PENDING; ++index) {
        if (host->pending[index].active &&
            host->pending[index].deadline_ms < nearest)
            nearest = host->pending[index].deadline_ms;
    }
    if (host->state == WLH_HOST_STATE_READY &&
        host->config.heartbeat_timeout_ms != 0u) {
        uint64_t heartbeat_deadline = host->diagnostics.last_peer_activity_ms +
                                      host->config.heartbeat_timeout_ms;
        if (heartbeat_deadline < nearest)
            nearest = heartbeat_deadline;
    }
    return nearest == UINT64_MAX ? WLH_OSAL_WAIT_FOREVER
                                 : deadline_wait_ms(current, nearest);
}

int enqueue_job(
    wlh_host_t *host,
    wlh_host_job_kind_t kind,
    void *payload,
    uint32_t timeout_ms
) {
    wlh_host_job_t job = {(uint32_t)kind, payload};
    if (host == NULL || !host->worker_started)
        return -1;
    return host->config.osal.queue_send(
        host->config.osal.context, &host->core_queue, &job, timeout_ms
    );
}

wlh_host_result_t wlh_host_rpc_request(
    wlh_host_t *host,
    uint16_t service_id,
    uint16_t method_id,
    const uint8_t *payload,
    size_t payload_size,
    uint32_t timeout_ms,
    wlh_rpc_completion_fn completion,
    void *completion_context,
    uint32_t *request_id
) {
    wlh_host_rpc_job_t *job;
    uint32_t assigned_request_id;
    if (host == NULL || service_id == 0u || method_id == 0u ||
        (payload_size != 0u && payload == NULL) ||
        payload_size > WLH_HOST_PROTOBUF_LIMIT || !host->worker_started)
        return WLH_HOST_INVALID_ARGUMENT;

    if (host->config.osal.mutex_lock(
            host->config.osal.context,
            &host->state_mutex,
            host->config.rpc_timeout_ms
        ) != 0)
        return WLH_HOST_TIMEOUT;
    assigned_request_id = host->next_request_id++;
    if (assigned_request_id == 0u)
        assigned_request_id = host->next_request_id++;
    host->config.osal.mutex_unlock(
        host->config.osal.context, &host->state_mutex
    );

    job = (wlh_host_rpc_job_t *)host->config.buffers.alloc(
        host->config.buffers.context, sizeof(*job) + payload_size
    );
    if (job == NULL)
        return WLH_HOST_NO_MEMORY;
    memset(job, 0, sizeof(*job));
    job->service_id = service_id;
    job->method_id = method_id;
    job->request_id = assigned_request_id;
    job->timeout_ms = timeout_ms;
    job->completion = completion;
    job->completion_context = completion_context;
    job->payload_size = payload_size;
    if (payload_size != 0u)
        memcpy(job->payload, payload, payload_size);
    if (enqueue_job(host, WLH_HOST_JOB_RPC_REQUEST, job, WLH_OSAL_NO_WAIT) !=
        0) {
        host->config.buffers.free(host->config.buffers.context, (uint8_t *)job);
        return WLH_HOST_PENDING_FULL;
    }
    if (request_id != NULL)
        *request_id = assigned_request_id;
    return WLH_HOST_OK;
}

wlh_host_result_t rpc_message_request(
    wlh_host_t *host,
    uint16_t service_id,
    uint16_t method_id,
    const pb_msgdesc_t *fields,
    const void *message,
    wlh_rpc_completion_fn completion,
    void *context
) {
    uint8_t *payload;
    size_t payload_size = 0u;
    size_t encoded_size = 0u;
    wlh_host_result_t result;

    if (!pb_get_encoded_size(&payload_size, fields, message) ||
        payload_size > WLH_HOST_PROTOBUF_LIMIT) {
        return WLH_HOST_PROTOCOL_ERROR;
    }
    if (payload_size == 0u) {
        return wlh_host_rpc_request(
            host, service_id, method_id, NULL, 0u, 0u, completion, context, NULL
        );
    }
    payload =
        host->config.buffers.alloc(host->config.buffers.context, payload_size);
    if (payload == NULL)
        return WLH_HOST_NO_MEMORY;
    result = encode_pb(payload, payload_size, &encoded_size, fields, message);
    if (result == WLH_HOST_OK && encoded_size == payload_size) {
        result = wlh_host_rpc_request(
            host,
            service_id,
            method_id,
            payload,
            payload_size,
            0u,
            completion,
            context,
            NULL
        );
    } else {
        result = WLH_HOST_PROTOCOL_ERROR;
    }
    host->config.buffers.free(host->config.buffers.context, payload);
    return result;
}

wlh_host_result_t process_rpc_request(
    wlh_host_t *host, const wlh_host_rpc_job_t *job
) {
    wlh_pending_rpc_t *pending;
    wlh_rpc_envelope_t envelope;
    wlh_host_result_t result;
    if (host->state != WLH_HOST_STATE_READY)
        return WLH_HOST_INVALID_STATE;

    pending = allocate_pending(host);
    if (pending == NULL)
        return WLH_HOST_PENDING_FULL;

    memset(&envelope, 0, sizeof(envelope));
    envelope.service_id = job->service_id;
    envelope.method_id = job->method_id;
    envelope.request_id = job->request_id;
    envelope.kind = WLH_RPC_KIND_REQUEST;

    memset(pending, 0, sizeof(*pending));
    pending->active = true;
    pending->session_id = host->session_id;
    pending->request_id = envelope.request_id;
    pending->service_id = job->service_id;
    pending->method_id = job->method_id;
    pending->deadline_ms =
        now_ms(host) +
        (job->timeout_ms == 0u ? host->config.rpc_timeout_ms : job->timeout_ms);
    pending->completion = job->completion;
    pending->completion_context = job->completion_context;

    result = send_rpc(host, &envelope, job->payload, job->payload_size, false);
    if (result != WLH_HOST_OK) {
        memset(pending, 0, sizeof(*pending));
        return result;
    }
    return WLH_HOST_OK;
}
