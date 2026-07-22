#include "wlh/host.h"

#include <limits.h>
#include <string.h>

#include "common.pb.h"
#include "device_info.pb.h"
#include "link.pb.h"
#include "user_passthrough.pb.h"
#include "wifi.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>

#define WLH_HOST_PROTOBUF_LIMIT 2048u
#define WLH_HOST_RPC_LIMIT (WLH_RPC_ENVELOPE_SIZE + WLH_HOST_PROTOBUF_LIMIT)

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

typedef enum wlh_host_job_kind {
    WLH_HOST_JOB_STOP = 1,
    WLH_HOST_JOB_RX_FRAME,
    WLH_HOST_JOB_RPC_REQUEST,
    WLH_HOST_JOB_ETHERNET_TX,
    WLH_HOST_JOB_TRANSPORT_LOST,
    WLH_HOST_JOB_TRANSPORT_STARTED,
    WLH_HOST_JOB_TRANSPORT_START_FAILED,
    WLH_HOST_JOB_TRANSPORT_STOPPED,
    WLH_HOST_JOB_TRANSPORT_STOP_FAILED
} wlh_host_job_kind_t;

typedef struct wlh_host_job {
    uint32_t kind;
    void *payload;
} wlh_host_job_t;

typedef struct wlh_host_data_job {
    size_t size;
    uint8_t data[];
} wlh_host_data_job_t;

typedef struct wlh_host_rpc_job {
    uint16_t service_id;
    uint16_t method_id;
    uint32_t request_id;
    uint32_t timeout_ms;
    wlh_rpc_completion_fn completion;
    void *completion_context;
    size_t payload_size;
    uint8_t payload[];
} wlh_host_rpc_job_t;

_Static_assert(
    sizeof(wlh_host_job_t) <= sizeof(uintptr_t) * 2u,
    "host queue slot too small"
);

static uint64_t now_ms(const wlh_host_t *host) {
    return host->config.osal.monotonic_time_ms(host->config.osal.context);
}

static void tx_complete(
    void *completion_context, uint8_t *frame, size_t size, int status
) {
    wlh_host_t *host = completion_context;
    (void)size;
    host->config.buffers.free(host->config.buffers.context, frame);
    if (status != 0)
        wlh_host_transport_lost(host);
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

static void dispatch_event(
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
        return;
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
    }
}

static void dispatch_completion(
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

static void set_state(wlh_host_t *host, wlh_host_state_t state) {
    if (host->state == state) {
        return;
    }
    host->state = state;
    host->diagnostics.state = state;
    dispatch_event(host, WLH_HOST_EVENT_STATE_CHANGED, 0u, 0u, NULL, 0u);
}

static void cancel_pending(wlh_host_t *host, wlh_host_result_t result) {
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

static wlh_host_result_t encode_pb(
    uint8_t *output,
    size_t capacity,
    size_t *size,
    const pb_msgdesc_t *fields,
    const void *message
) {
    pb_ostream_t stream = pb_ostream_from_buffer(output, capacity);
    if (!pb_encode(&stream, fields, message)) {
        return WLH_HOST_PROTOCOL_ERROR;
    }
    *size = stream.bytes_written;
    return WLH_HOST_OK;
}

static wlh_host_result_t send_payload_frame(
    wlh_host_t *host,
    uint8_t channel,
    const uint8_t *payload,
    size_t payload_size,
    bool reserved
) {
    wlh_frame_header_t header;
    uint8_t *frame;
    size_t frame_size = WLH_FRAME_HEADER_SIZE + payload_size;
    size_t encoded_size = 0u;
    if (frame_size > host->config.max_frame_size || payload_size > UINT16_MAX) {
        return WLH_HOST_INVALID_ARGUMENT;
    }
    if (!reserved && host->tx_credit[channel] == 0u) {
        set_state(host, WLH_HOST_STATE_CONGESTED);
        return WLH_HOST_NO_CREDIT;
    }
    frame =
        host->config.buffers.alloc(host->config.buffers.context, frame_size);
    if (frame == NULL) {
        host->diagnostics.buffer_allocation_failures++;
        return WLH_HOST_NO_MEMORY;
    }
    wlh_frame_header_init(&header, channel);
    header.session_id =
        channel == WLH_CHANNEL_LINK_CONTROL && host->session_id == 0u
            ? 0u
            : host->session_id;
    header.sequence = host->tx_sequence[channel]++;
    if (wlh_frame_encode(
            frame, frame_size, &encoded_size, &header, payload, payload_size
        ) != WLH_WIRE_OK) {
        host->config.buffers.free(host->config.buffers.context, frame);
        return WLH_HOST_PROTOCOL_ERROR;
    }
    if (host->config.transport.submit_tx(
            host->config.transport.context,
            frame,
            encoded_size,
            tx_complete,
            host
        ) != 0) {
        host->config.buffers.free(host->config.buffers.context, frame);
        return WLH_HOST_TRANSPORT_ERROR;
    }
    if (!reserved) {
        host->tx_credit[channel]--;
    }
    host->diagnostics.tx_frames++;
    return WLH_HOST_OK;
}

static wlh_host_result_t send_rpc(
    wlh_host_t *host,
    const wlh_rpc_envelope_t *envelope,
    const uint8_t *payload,
    size_t payload_size,
    bool reserved
) {
    uint8_t rpc[WLH_HOST_RPC_LIMIT];
    size_t rpc_size = 0u;
    uint8_t channel = envelope->service_id == WLH_SERVICE_LINK
                          ? WLH_CHANNEL_LINK_CONTROL
                          : WLH_CHANNEL_CONTROL_RPC;
    if (wlh_rpc_encode(
            rpc, sizeof(rpc), &rpc_size, envelope, payload, payload_size
        ) != WLH_WIRE_OK) {
        return WLH_HOST_PROTOCOL_ERROR;
    }
    return send_payload_frame(host, channel, rpc, rpc_size, reserved);
}

static wlh_host_result_t send_hello(wlh_host_t *host) {
    wlh_protocol_v1_HelloRequest hello = wlh_protocol_v1_HelloRequest_init_zero;
    wlh_rpc_envelope_t envelope;
    uint8_t payload[WLH_HOST_PROTOBUF_LIMIT];
    size_t payload_size = 0u;

    hello.protocol_versions_count = 1u;
    hello.protocol_versions[0].major = 1u;
    hello.protocol_versions[0].min_minor = 0u;
    hello.protocol_versions[0].max_minor = 0u;
    memcpy(
        hello.implementation,
        "wl-hosted-host-core",
        sizeof("wl-hosted-host-core")
    );
    memcpy(hello.implementation_version, "0.1.0", sizeof("0.1.0"));
    hello.max_frame_size = host->config.max_frame_size;
    hello.alignment = 1u;
    hello.checksum_modes_count = 2u;
    hello.checksum_modes[0] = wlh_protocol_v1_ChecksumMode_CHECKSUM_MODE_SUM32;
    hello.checksum_modes[1] = wlh_protocol_v1_ChecksumMode_CHECKSUM_MODE_CRC32C;
    hello.max_rpc_payload = WLH_HOST_PROTOBUF_LIMIT;
    hello.services_count = 3u;
    // clang-format off
    hello.services[0] = (wlh_protocol_v1_ServiceVersionRange){
        WLH_SERVICE_LINK, 1u, 0u, 0u,
    };

    hello.services[1] = (wlh_protocol_v1_ServiceVersionRange){
        WLH_SERVICE_WIFI, 1u, 0u, 0u,
    };

    hello.services[2] = (wlh_protocol_v1_ServiceVersionRange){
        WLH_SERVICE_DIAGNOSTICS, 1u, 0u, 0u,
    };

    hello.channels_count = 3u;
    hello.channels[0] = (wlh_protocol_v1_ChannelCapability){
        WLH_CHANNEL_LINK_CONTROL, WLH_HOST_PROTOBUF_LIMIT, 0u, 1u, 0u,
    };

    hello.channels[1] = (wlh_protocol_v1_ChannelCapability){
        WLH_CHANNEL_CONTROL_RPC, WLH_HOST_PROTOBUF_LIMIT, 0u, 1u, 0u,
    };

    hello.channels[2] = (wlh_protocol_v1_ChannelCapability){
        WLH_CHANNEL_ETHERNET_STA, 1600u, 0u, 1u, 0u,
    };
    // clang-format on
    if (encode_pb(
            payload,
            sizeof(payload),
            &payload_size,
            wlh_protocol_v1_HelloRequest_fields,
            &hello
        ) != WLH_HOST_OK) {
        return WLH_HOST_PROTOCOL_ERROR;
    }
    memset(&envelope, 0, sizeof(envelope));
    envelope.service_id = WLH_SERVICE_LINK;
    envelope.method_id = WLH_LINK_METHOD_HELLO;
    envelope.request_id = host->next_request_id++;
    if (envelope.request_id == 0u) {
        envelope.request_id = host->next_request_id++;
    }
    envelope.kind = WLH_RPC_KIND_REQUEST;
    set_state(host, WLH_HOST_STATE_NEGOTIATING);
    return send_rpc(host, &envelope, payload, payload_size, true);
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

static wlh_pending_rpc_t *find_pending(
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

static wlh_host_result_t handle_hello_response(
    wlh_host_t *host, const uint8_t *payload, size_t payload_size
) {
    wlh_protocol_v1_HelloResponse hello =
        wlh_protocol_v1_HelloResponse_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
    size_t index;
    if (!pb_decode(&stream, wlh_protocol_v1_HelloResponse_fields, &hello) ||
        !hello.has_selected_protocol || hello.selected_protocol.major != 1u ||
        hello.session_id == 0u ||
        hello.max_frame_size < WLH_FRAME_HEADER_SIZE) {
        set_state(host, WLH_HOST_STATE_FAILED);
        return WLH_HOST_PROTOCOL_ERROR;
    }
    host->session_id = hello.session_id;
    host->diagnostics.session_id = hello.session_id;
    memset(host->tx_credit, 0, sizeof(host->tx_credit));
    for (index = 0; index < hello.initial_credits_count; ++index) {
        if (hello.initial_credits[index].channel_id < WLH_HOST_CHANNEL_COUNT) {
            host->tx_credit[hello.initial_credits[index].channel_id] =
                hello.initial_credits[index].units;
        }
    }
    host->diagnostics.last_peer_activity_ms = now_ms(host);
    set_state(host, WLH_HOST_STATE_READY);
    return WLH_HOST_OK;
}

static void handle_link_event(
    wlh_host_t *host,
    const wlh_rpc_envelope_t *envelope,
    const uint8_t *payload,
    size_t payload_size
) {
    if (envelope->method_id == WLH_LINK_METHOD_CREDIT_UPDATE) {
        wlh_protocol_v1_CreditUpdate credit =
            wlh_protocol_v1_CreditUpdate_init_zero;
        pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
        if (pb_decode(&stream, wlh_protocol_v1_CreditUpdate_fields, &credit) &&
            credit.channel_id < WLH_HOST_CHANNEL_COUNT) {
            uint32_t old = host->tx_credit[credit.channel_id];
            host->tx_credit[credit.channel_id] = UINT32_MAX - old < credit.units
                                                     ? UINT32_MAX
                                                     : old + credit.units;
            if (host->state == WLH_HOST_STATE_CONGESTED) {
                set_state(host, WLH_HOST_STATE_READY);
            }
        }
    } else if (envelope->method_id == WLH_LINK_METHOD_HEARTBEAT) {
        wlh_protocol_v1_Heartbeat heartbeat =
            wlh_protocol_v1_Heartbeat_init_zero;
        pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
        if (pb_decode(&stream, wlh_protocol_v1_Heartbeat_fields, &heartbeat) &&
            heartbeat.session_id == host->session_id) {
            host->diagnostics.last_peer_activity_ms = now_ms(host);
        }
    } else if (envelope->method_id == WLH_LINK_EVENT_SESSION_CHANGED) {
        wlh_protocol_v1_SessionChangedEvent changed =
            wlh_protocol_v1_SessionChangedEvent_init_zero;
        pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
        if (pb_decode(
                &stream, wlh_protocol_v1_SessionChangedEvent_fields, &changed
            ) &&
            changed.new_session_id != 0u &&
            changed.new_session_id != host->session_id) {
            host->diagnostics.peer_resets++;
            cancel_pending(host, WLH_HOST_SESSION_CHANGED);
            host->session_id = 0u;
            memset(host->tx_credit, 0, sizeof(host->tx_credit));
            memset(host->rx_sequence_valid, 0, sizeof(host->rx_sequence_valid));
            (void)send_hello(host);
        }
    }
}

static wlh_host_result_t host_process_deadlines(wlh_host_t *host);
static uint32_t host_next_wait_ms(const wlh_host_t *host);
static wlh_host_result_t process_frame(
    wlh_host_t *host, const uint8_t *frame, size_t size
);
static wlh_host_result_t process_rpc_request(
    wlh_host_t *host, const wlh_host_rpc_job_t *job
);

static int enqueue_job(
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

static void transport_start_complete(void *context, int status) {
    wlh_host_t *host = context;
    (void)enqueue_job(
        host,
        status == 0 ? WLH_HOST_JOB_TRANSPORT_STARTED
                    : WLH_HOST_JOB_TRANSPORT_START_FAILED,
        NULL,
        WLH_OSAL_WAIT_FOREVER
    );
}

static void transport_stop_complete(void *context, int status) {
    wlh_host_t *host = context;
    (void)enqueue_job(
        host,
        status == 0 ? WLH_HOST_JOB_TRANSPORT_STOPPED
                    : WLH_HOST_JOB_TRANSPORT_STOP_FAILED,
        NULL,
        WLH_OSAL_WAIT_FOREVER
    );
}

static void request_transport_start(wlh_host_t *host) {
    set_state(host, WLH_HOST_STATE_TRANSPORT_STARTING);
    if (host->config.transport.start(
            host->config.transport.context, transport_start_complete, host
        ) != 0)
        set_state(host, WLH_HOST_STATE_FAILED);
}

static void finish_shutdown(wlh_host_t *host) {
    host->session_id = 0u;
    set_state(host, WLH_HOST_STATE_UNINITIALIZED);
    host->worker_stopping = true;
}

static void process_transport_lost(wlh_host_t *host) {
    if (host->state == WLH_HOST_STATE_STOPPING ||
        host->state == WLH_HOST_STATE_UNINITIALIZED)
        return;
    host->diagnostics.transport_resets++;
    cancel_pending(host, WLH_HOST_SESSION_CHANGED);
    host->session_id = 0u;
    memset(host->tx_credit, 0, sizeof(host->tx_credit));
    memset(host->tx_sequence, 0, sizeof(host->tx_sequence));
    memset(host->rx_sequence_valid, 0, sizeof(host->rx_sequence_valid));
    set_state(host, WLH_HOST_STATE_RECOVERING);
    if (host->config.transport.stop(
            host->config.transport.context, transport_stop_complete, host
        ) != 0)
        set_state(host, WLH_HOST_STATE_FAILED);
}

static void host_worker(void *argument) {
    wlh_host_t *host = argument;
    wlh_host_job_t job;
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
                (void)send_payload_frame(
                    host,
                    WLH_CHANNEL_ETHERNET_STA,
                    data->data,
                    data->size,
                    false
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
    host->config.osal.mutex_destroy(
        host->config.osal.context, &host->state_mutex
    );
    return WLH_HOST_OK;
}

static wlh_host_result_t host_process_deadlines(wlh_host_t *host) {
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

static uint32_t host_next_wait_ms(const wlh_host_t *host) {
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

static wlh_host_result_t process_frame(
    wlh_host_t *host, const uint8_t *frame, size_t size
) {
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
        dispatch_event(host, WLH_HOST_EVENT_PROTOCOL_FAULT, 0u, 0u, NULL, 0u);
        return WLH_HOST_PROTOCOL_ERROR;
    }

    if (host->session_id != 0u && header.session_id != host->session_id) {
        host->diagnostics.peer_resets++;
        cancel_pending(host, WLH_HOST_SESSION_CHANGED);
        host->session_id = 0u;
        set_state(host, WLH_HOST_STATE_RECOVERING);
        (void)send_hello(host);
        return WLH_HOST_SESSION_CHANGED;
    }

    if (host->rx_sequence_valid[header.channel] &&
        header.sequence != host->expected_rx_sequence[header.channel]) {
        host->diagnostics.sequence_gaps++;
    }
    host->expected_rx_sequence[header.channel] = header.sequence + 1u;
    host->rx_sequence_valid[header.channel] = true;
    host->diagnostics.rx_frames++;
    host->diagnostics.last_peer_activity_ms = now_ms(host);

    // Dispatch by channel.
    if (header.channel == WLH_CHANNEL_ETHERNET_STA) {
        if (frame_payload_size < 8u) {
            return WLH_HOST_PROTOCOL_ERROR;
        }
        dispatch_event(
            host,
            WLH_HOST_EVENT_ETHERNET_STA_RX,
            0u,
            0u,
            frame_payload + 8u,
            frame_payload_size - 8u
        );
        return WLH_HOST_OK;
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

static wlh_host_result_t process_rpc_request(
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

void wlh_host_transport_lost(wlh_host_t *host) {
    if (host != NULL && host->worker_started)
        (void)enqueue_job(
            host, WLH_HOST_JOB_TRANSPORT_LOST, NULL, WLH_OSAL_NO_WAIT
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

static wlh_host_result_t wifi_request(
    wlh_host_t *host,
    uint16_t method,
    const pb_msgdesc_t *fields,
    const void *message,
    wlh_rpc_completion_fn completion,
    void *context
) {
    uint8_t payload[WLH_HOST_PROTOBUF_LIMIT];
    size_t payload_size = 0u;
    wlh_host_result_t result =
        encode_pb(payload, sizeof(payload), &payload_size, fields, message);
    if (result != WLH_HOST_OK)
        return result;
    return wlh_host_rpc_request(
        host,
        WLH_SERVICE_WIFI,
        method,
        payload,
        payload_size,
        0u,
        completion,
        context,
        NULL
    );
}

wlh_host_result_t wlh_host_wifi_initialize(
    wlh_host_t *host, wlh_rpc_completion_fn completion, void *context
) {
    wlh_protocol_v1_WifiInitializeRequest request =
        wlh_protocol_v1_WifiInitializeRequest_init_zero;
    request.interface_flags = 1u;
    return wifi_request(
        host,
        WLH_WIFI_METHOD_INITIALIZE,
        wlh_protocol_v1_WifiInitializeRequest_fields,
        &request,
        completion,
        context
    );
}

wlh_host_result_t wlh_host_wifi_scan(
    wlh_host_t *host,
    const wlh_wifi_scan_params_t *params,
    wlh_rpc_completion_fn completion,
    void *context
) {
    wlh_protocol_v1_WifiScanRequest request =
        wlh_protocol_v1_WifiScanRequest_init_zero;
    if (params == NULL || params->scan_id == 0u ||
        params->ssid_size > WLH_HOST_MAX_SSID_SIZE ||
        (params->ssid_size != 0u && params->ssid == NULL))
        return WLH_HOST_INVALID_ARGUMENT;
    request.scan_id = params->scan_id;
    request.interface = wlh_protocol_v1_WifiInterface_WIFI_INTERFACE_STA;
    request.ssid.size = params->ssid_size;
    if (params->ssid_size != 0u)
        memcpy(request.ssid.bytes, params->ssid, params->ssid_size);
    request.include_hidden = params->include_hidden;
    request.max_results = params->max_results;
    return wifi_request(
        host,
        WLH_WIFI_METHOD_SCAN_START,
        wlh_protocol_v1_WifiScanRequest_fields,
        &request,
        completion,
        context
    );
}

wlh_host_result_t wlh_host_wifi_connect(
    wlh_host_t *host,
    const wlh_wifi_connect_params_t *params,
    wlh_rpc_completion_fn completion,
    void *context
) {
    wlh_protocol_v1_WifiConnectRequest request =
        wlh_protocol_v1_WifiConnectRequest_init_zero;
    if (params == NULL || params->ssid_size == 0u ||
        params->ssid_size > WLH_HOST_MAX_SSID_SIZE ||
        params->credential_size > WLH_HOST_MAX_CREDENTIAL_SIZE ||
        params->ssid == NULL ||
        (params->credential_size != 0u && params->credential == NULL))
        return WLH_HOST_INVALID_ARGUMENT;
    request.ssid.size = params->ssid_size;
    memcpy(request.ssid.bytes, params->ssid, params->ssid_size);
    request.credential.size = params->credential_size;
    if (params->credential_size != 0u)
        memcpy(
            request.credential.bytes,
            params->credential,
            params->credential_size
        );
    request.security = (wlh_protocol_v1_WifiSecurity)params->security;
    request.timeout_ms = params->timeout_ms;
    return wifi_request(
        host,
        WLH_WIFI_METHOD_CONNECT,
        wlh_protocol_v1_WifiConnectRequest_fields,
        &request,
        completion,
        context
    );
}

wlh_host_result_t wlh_host_wifi_disconnect(
    wlh_host_t *host, wlh_rpc_completion_fn completion, void *context
) {
    wlh_protocol_v1_WifiDisconnectRequest request =
        wlh_protocol_v1_WifiDisconnectRequest_init_zero;
    request.interface = wlh_protocol_v1_WifiInterface_WIFI_INTERFACE_STA;
    return wifi_request(
        host,
        WLH_WIFI_METHOD_DISCONNECT,
        wlh_protocol_v1_WifiDisconnectRequest_fields,
        &request,
        completion,
        context
    );
}

wlh_host_result_t wlh_host_wifi_start_ap(
    wlh_host_t *host,
    const wlh_wifi_start_ap_params_t *params,
    wlh_rpc_completion_fn completion,
    void *context
) {
    wlh_protocol_v1_WifiStartApRequest request =
        wlh_protocol_v1_WifiStartApRequest_init_zero;
    if (params == NULL || params->ssid_size == 0u ||
        params->ssid_size > WLH_HOST_MAX_SSID_SIZE ||
        params->credential_size > WLH_HOST_MAX_CREDENTIAL_SIZE ||
        params->ssid == NULL ||
        (params->credential_size != 0u && params->credential == NULL))
        return WLH_HOST_INVALID_ARGUMENT;
    request.ssid.size = params->ssid_size;
    memcpy(request.ssid.bytes, params->ssid, params->ssid_size);
    request.credential.size = params->credential_size;
    if (params->credential_size != 0u)
        memcpy(
            request.credential.bytes,
            params->credential,
            params->credential_size
        );
    request.security = (wlh_protocol_v1_WifiSecurity)params->security;
    request.channel = params->channel;
    request.max_clients = params->max_clients;
    return wifi_request(
        host,
        WLH_WIFI_METHOD_START_AP,
        wlh_protocol_v1_WifiStartApRequest_fields,
        &request,
        completion,
        context
    );
}

wlh_host_result_t wlh_host_wifi_stop_ap(
    wlh_host_t *host, wlh_rpc_completion_fn completion, void *context
) {
    wlh_protocol_v1_Empty request = wlh_protocol_v1_Empty_init_zero;
    return wifi_request(
        host,
        WLH_WIFI_METHOD_STOP_AP,
        wlh_protocol_v1_Empty_fields,
        &request,
        completion,
        context
    );
}

typedef struct wlh_device_info_request {
    wlh_host_t *host;
    wlh_host_device_info_fn completion;
    void *context;
} wlh_device_info_request_t;

static void device_info_completion(
    void *context,
    wlh_host_result_t result,
    uint16_t status_domain,
    int16_t status_code,
    const uint8_t *payload,
    size_t payload_size
) {
    wlh_device_info_request_t *request = context;
    wlh_host_t *host = request->host;
    wlh_host_device_info_t info;
    const wlh_host_device_info_t *decoded = NULL;

    if (result == WLH_HOST_OK) {
        wlh_protocol_v1_DeviceInfoResponse message =
            wlh_protocol_v1_DeviceInfoResponse_init_zero;
        pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
        if (pb_decode(
                &stream, wlh_protocol_v1_DeviceInfoResponse_fields, &message
            )) {
            memset(&info, 0, sizeof(info));
            memcpy(info.vendor, message.vendor, sizeof(info.vendor) - 1u);
            memcpy(
                info.mcu_model, message.mcu_model, sizeof(info.mcu_model) - 1u
            );
            memcpy(
                info.board_profile,
                message.board_profile,
                sizeof(info.board_profile) - 1u
            );
            info.uid_size = message.uid.size;
            memcpy(info.uid, message.uid.bytes, message.uid.size);
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

wlh_host_result_t wlh_host_get_device_info(
    wlh_host_t *host, wlh_host_device_info_fn completion, void *context
) {
    wlh_device_info_request_t *request;
    wlh_host_result_t result;
    wlh_protocol_v1_Empty message = wlh_protocol_v1_Empty_init_zero;
    uint8_t payload[WLH_HOST_PROTOBUF_LIMIT];
    size_t payload_size = 0u;

    if (host == NULL || completion == NULL || !host->worker_started)
        return WLH_HOST_INVALID_ARGUMENT;
    result = encode_pb(
        payload,
        sizeof(payload),
        &payload_size,
        wlh_protocol_v1_Empty_fields,
        &message
    );
    if (result != WLH_HOST_OK)
        return result;

    request = (wlh_device_info_request_t *)host->config.buffers.alloc(
        host->config.buffers.context, sizeof(*request)
    );
    if (request == NULL)
        return WLH_HOST_NO_MEMORY;
    request->host = host;
    request->completion = completion;
    request->context = context;

    result = wlh_host_rpc_request(
        host,
        WLH_SERVICE_DEVICE_INFO,
        WLH_DEVICE_INFO_METHOD_GET_INFO,
        payload,
        payload_size,
        0u,
        device_info_completion,
        request,
        NULL
    );
    if (result != WLH_HOST_OK) {
        host->config.buffers.free(
            host->config.buffers.context, (uint8_t *)request
        );
    }
    return result;
}

wlh_host_result_t wlh_host_user_message_send(
    wlh_host_t *host,
    uint32_t endpoint_id,
    uint32_t message_type,
    uint32_t flags,
    const uint8_t *payload,
    size_t payload_size,
    wlh_rpc_completion_fn completion,
    void *context
) {
    uint8_t encoded[WLH_HOST_PROTOBUF_LIMIT];
    size_t encoded_size = 0u;
    wlh_host_result_t result;
    wlh_protocol_v1_UserMessageSendRequest request =
        wlh_protocol_v1_UserMessageSendRequest_init_zero;

    if (host == NULL || endpoint_id == 0u ||
        payload_size > WLH_HOST_MAX_USER_PAYLOAD_SIZE ||
        (payload_size != 0u && payload == NULL))
        return WLH_HOST_INVALID_ARGUMENT;

    request.endpoint_id = endpoint_id;
    request.message_type = message_type;
    request.flags = flags;
    request.payload.size = payload_size;
    if (payload_size != 0u)
        memcpy(request.payload.bytes, payload, payload_size);

    result = encode_pb(
        encoded,
        sizeof(encoded),
        &encoded_size,
        wlh_protocol_v1_UserMessageSendRequest_fields,
        &request
    );
    if (result != WLH_HOST_OK)
        return result;
    return wlh_host_rpc_request(
        host,
        WLH_SERVICE_USER_PASSTHROUGH,
        WLH_USER_PASSTHROUGH_METHOD_SEND,
        encoded,
        encoded_size,
        0u,
        completion,
        context,
        NULL
    );
}

wlh_host_result_t wlh_host_ethernet_sta_send(
    wlh_host_t *host, const uint8_t *ethernet_frame, size_t size
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
        job->size = 8u + size;
        job->data[0] = 1u;
        job->data[1] = 0u;
        wlh_write_u16_le(job->data + 2u, 4u, 8u);
        wlh_write_u32_le(job->data + 4u, 4u, (uint32_t)size);
        memcpy(job->data + 8u, ethernet_frame, size);
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
