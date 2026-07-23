#include "wlh/host.h"

#include <assert.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "common.pb.h"
#include "device_info.pb.h"
#include "link.pb.h"
#include "user_passthrough.pb.h"
#include "wifi.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>

typedef struct test_task_state {
    pthread_t thread;
    wlh_osal_task_fn entry;
    void *argument;
} test_task_state_t;

typedef struct test_queue_state {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    uint8_t *storage;
    size_t item_size;
    size_t capacity;
    size_t head;
    size_t count;
} test_queue_state_t;

_Static_assert(
    sizeof(test_task_state_t) <= sizeof(wlh_osal_task_t), "task storage"
);
_Static_assert(
    sizeof(test_queue_state_t) <= sizeof(wlh_osal_queue_t), "queue storage"
);

static void *test_task_main(void *argument) {
    test_task_state_t *state = argument;
    state->entry(state->argument);
    return NULL;
}
static int test_task_create(
    void *ctx,
    wlh_osal_task_t *task,
    const wlh_osal_task_attributes_t *attr,
    wlh_osal_task_fn entry,
    void *argument
) {
    test_task_state_t *state = (test_task_state_t *)task;
    (void)ctx;
    (void)attr;
    memset(task, 0, sizeof(*task));
    state->entry = entry;
    state->argument = argument;
    return pthread_create(&state->thread, NULL, test_task_main, state);
}
static int test_task_join(void *ctx, wlh_osal_task_t *task, uint32_t timeout) {
    (void)ctx;
    (void)timeout;
    return pthread_join(((test_task_state_t *)task)->thread, NULL);
}
static int test_mutex_create(void *ctx, wlh_osal_mutex_t *mutex) {
    (void)ctx;
    memset(mutex, 0, sizeof(*mutex));
    return pthread_mutex_init((pthread_mutex_t *)mutex, NULL);
}
static void test_mutex_destroy(void *ctx, wlh_osal_mutex_t *mutex) {
    (void)ctx;
    (void)pthread_mutex_destroy((pthread_mutex_t *)mutex);
}
static int test_mutex_lock(
    void *ctx, wlh_osal_mutex_t *mutex, uint32_t timeout
) {
    (void)ctx;
    (void)timeout;
    return pthread_mutex_lock((pthread_mutex_t *)mutex);
}
static void test_mutex_unlock(void *ctx, wlh_osal_mutex_t *mutex) {
    (void)ctx;
    (void)pthread_mutex_unlock((pthread_mutex_t *)mutex);
}
static int test_queue_create(
    void *ctx,
    wlh_osal_queue_t *queue,
    void *storage,
    size_t item_size,
    size_t capacity
) {
    test_queue_state_t *state = (test_queue_state_t *)queue;
    (void)ctx;
    memset(queue, 0, sizeof(*queue));
    assert(pthread_mutex_init(&state->mutex, NULL) == 0);
    assert(pthread_cond_init(&state->not_empty, NULL) == 0);
    assert(pthread_cond_init(&state->not_full, NULL) == 0);
    state->storage = storage;
    state->item_size = item_size;
    state->capacity = capacity;
    return 0;
}
static void test_queue_destroy(void *ctx, wlh_osal_queue_t *queue) {
    test_queue_state_t *state = (test_queue_state_t *)queue;
    (void)ctx;
    (void)pthread_cond_destroy(&state->not_empty);
    (void)pthread_cond_destroy(&state->not_full);
    (void)pthread_mutex_destroy(&state->mutex);
}
static int test_queue_send(
    void *ctx, wlh_osal_queue_t *queue, const void *item, uint32_t timeout
) {
    test_queue_state_t *state = (test_queue_state_t *)queue;
    size_t tail;
    (void)ctx;
    pthread_mutex_lock(&state->mutex);
    if (state->count == state->capacity && timeout == WLH_OSAL_NO_WAIT) {
        pthread_mutex_unlock(&state->mutex);
        return -1;
    }
    while (state->count == state->capacity)
        pthread_cond_wait(&state->not_full, &state->mutex);
    tail = (state->head + state->count) % state->capacity;
    memcpy(state->storage + tail * state->item_size, item, state->item_size);
    state->count++;
    pthread_cond_signal(&state->not_empty);
    pthread_mutex_unlock(&state->mutex);
    return 0;
}
static int test_queue_send_isr(
    void *ctx, wlh_osal_queue_t *queue, const void *item, bool *woken
) {
    if (woken != NULL)
        *woken = true;
    return test_queue_send(ctx, queue, item, WLH_OSAL_NO_WAIT);
}
static int test_queue_receive(
    void *ctx, wlh_osal_queue_t *queue, void *item, uint32_t timeout
) {
    test_queue_state_t *state = (test_queue_state_t *)queue;
    struct timespec deadline;
    int result = 0;
    (void)ctx;
    pthread_mutex_lock(&state->mutex);
    if (timeout != WLH_OSAL_NO_WAIT && timeout != WLH_OSAL_WAIT_FOREVER) {
        clock_gettime(CLOCK_REALTIME, &deadline);
        deadline.tv_nsec += (long)timeout * 1000000L;
        deadline.tv_sec += deadline.tv_nsec / 1000000000L;
        deadline.tv_nsec %= 1000000000L;
    }
    while (state->count == 0u) {
        int wait_result;
        if (timeout == WLH_OSAL_NO_WAIT) {
            result = -1;
            break;
        }
        wait_result = timeout == WLH_OSAL_WAIT_FOREVER
                          ? pthread_cond_wait(&state->not_empty, &state->mutex)
                          : pthread_cond_timedwait(
                                &state->not_empty, &state->mutex, &deadline
                            );
        if (wait_result != 0) {
            result = -1;
            break;
        }
    }
    if (result == 0) {
        memcpy(
            item,
            state->storage + state->head * state->item_size,
            state->item_size
        );
        state->head = (state->head + 1u) % state->capacity;
        state->count--;
        pthread_cond_signal(&state->not_full);
    }
    pthread_mutex_unlock(&state->mutex);
    return result;
}
static uint64_t test_now(void *ctx) {
    return *(volatile uint64_t *)ctx;
}
static void test_sleep(void *ctx, uint32_t ms) {
    struct timespec value = {
        (time_t)(ms / 1000u), (long)(ms % 1000u) * 1000000L
    };
    (void)ctx;
    nanosleep(&value, NULL);
}
static void test_yield(void *ctx) {
    (void)ctx;
}
static bool test_in_isr(void *ctx) {
    (void)ctx;
    return false;
}
static int stub_create2(void *ctx, void *object) {
    (void)ctx;
    (void)object;
    return 0;
}
static void stub_destroy(void *ctx, void *object) {
    (void)ctx;
    (void)object;
}
static int stub_sem_create(
    void *c, wlh_osal_semaphore_t *s, uint32_t i, uint32_t m
) {
    (void)c;
    (void)s;
    (void)i;
    (void)m;
    return 0;
}
static int stub_sem_take(void *c, wlh_osal_semaphore_t *s, uint32_t t) {
    (void)c;
    (void)s;
    (void)t;
    return -1;
}
static int stub_sem_give(void *c, wlh_osal_semaphore_t *s) {
    (void)c;
    (void)s;
    return 0;
}
static int stub_sem_give_isr(void *c, wlh_osal_semaphore_t *s, bool *w) {
    (void)c;
    (void)s;
    if (w)
        *w = false;
    return 0;
}
static int stub_event_wait(
    void *c,
    wlh_osal_event_t *e,
    uint32_t b,
    bool a,
    bool x,
    uint32_t t,
    uint32_t *o
) {
    (void)c;
    (void)e;
    (void)b;
    (void)a;
    (void)x;
    (void)t;
    if (o)
        *o = 0;
    return -1;
}
static int stub_event_set(void *c, wlh_osal_event_t *e, uint32_t b) {
    (void)c;
    (void)e;
    (void)b;
    return 0;
}
static int stub_event_set_isr(
    void *c, wlh_osal_event_t *e, uint32_t b, bool *w
) {
    (void)c;
    (void)e;
    (void)b;
    if (w)
        *w = false;
    return 0;
}
static int stub_timer_create(
    void *c, wlh_osal_timer_t *t, wlh_osal_timer_fn f, void *a
) {
    (void)c;
    (void)t;
    (void)f;
    (void)a;
    return 0;
}
static int stub_timer_start(void *c, wlh_osal_timer_t *t, uint32_t p, bool r) {
    (void)c;
    (void)t;
    (void)p;
    (void)r;
    return 0;
}
static int stub_timer_stop(void *c, wlh_osal_timer_t *t) {
    (void)c;
    (void)t;
    return 0;
}

static wlh_osal_ops_t make_test_osal(volatile uint64_t *now) {
    wlh_osal_ops_t ops = {
        (void *)now,
        test_task_create,
        test_task_join,
        test_mutex_create,
        test_mutex_destroy,
        test_mutex_lock,
        test_mutex_unlock,
        stub_sem_create,
        (void (*)(void *, wlh_osal_semaphore_t *))stub_destroy,
        stub_sem_take,
        stub_sem_give,
        stub_sem_give_isr,
        (int (*)(void *, wlh_osal_event_t *))stub_create2,
        (void (*)(void *, wlh_osal_event_t *))stub_destroy,
        stub_event_wait,
        stub_event_set,
        stub_event_set_isr,
        test_queue_create,
        test_queue_destroy,
        test_queue_send,
        test_queue_send_isr,
        test_queue_receive,
        stub_timer_create,
        (void (*)(void *, wlh_osal_timer_t *))stub_destroy,
        stub_timer_start,
        stub_timer_stop,
        test_now,
        test_sleep,
        test_yield,
        test_in_isr
    };
    return ops;
}

typedef struct fixture {
    wlh_host_t host;
    volatile uint64_t now;
    uint8_t tx[4096];
    size_t tx_size;
    volatile unsigned tx_count;
    unsigned starts;
    unsigned stops;
    unsigned events;
    unsigned completions;
    wlh_host_result_t last_completion;
    wlh_host_event_kind_t last_event_kind;
    uint8_t last_event_payload[1024];
    size_t last_event_payload_size;
    unsigned device_info_callbacks;
    wlh_host_result_t device_info_result;
    wlh_host_device_info_t device_info;
    bool defer_start;
    wlh_transport_lifecycle_complete_fn start_completion;
    void *start_completion_context;
} fixture_t;

static int transport_start(
    void *context,
    wlh_transport_lifecycle_complete_fn completion,
    void *completion_context
) {
    fixture_t *fixture = context;
    fixture->starts++;
    if (fixture->defer_start) {
        fixture->start_completion = completion;
        fixture->start_completion_context = completion_context;
    } else {
        completion(completion_context, 0);
    }
    return 0;
}
static int transport_stop(
    void *context,
    wlh_transport_lifecycle_complete_fn completion,
    void *completion_context
) {
    ((fixture_t *)context)->stops++;
    completion(completion_context, 0);
    return 0;
}
static int transport_submit(
    void *context,
    uint8_t *frame,
    size_t size,
    wlh_transport_tx_complete_fn completion,
    void *completion_context
) {
    fixture_t *fixture = context;
    assert(size <= sizeof(fixture->tx));
    memcpy(fixture->tx, frame, size);
    fixture->tx_size = size;
    fixture->tx_count++;
    completion(completion_context, frame, size, 0);
    return 0;
}
static uint8_t *buffer_alloc(void *context, size_t size) {
    (void)context;
    return malloc(size);
}
static void buffer_free(void *context, uint8_t *buffer) {
    (void)context;
    free(buffer);
}

typedef struct failing_allocator {
    size_t attempts;
    size_t fail_at;
    size_t outstanding;
} failing_allocator_t;

static uint8_t *failing_buffer_alloc(void *context, size_t size) {
    failing_allocator_t *allocator = context;
    uint8_t *buffer;

    ++allocator->attempts;
    if (allocator->attempts == allocator->fail_at)
        return NULL;
    buffer = malloc(size);
    if (buffer != NULL)
        ++allocator->outstanding;
    return buffer;
}

static void failing_buffer_free(void *context, uint8_t *buffer) {
    failing_allocator_t *allocator = context;

    assert(buffer != NULL);
    assert(allocator->outstanding != 0u);
    --allocator->outstanding;
    free(buffer);
}
static int post_now(void *context, wlh_task_fn task, void *task_context) {
    (void)context;
    task(task_context);
    return 0;
}
static void on_event(void *context, const wlh_host_event_t *event) {
    fixture_t *fixture = context;
    fixture->events++;
    fixture->last_event_kind = event->kind;
    if (event->payload_size <= sizeof(fixture->last_event_payload)) {
        memcpy(
            fixture->last_event_payload, event->payload, event->payload_size
        );
        fixture->last_event_payload_size = event->payload_size;
    }
}
static void on_completion(
    void *context,
    wlh_host_result_t result,
    uint16_t domain,
    int16_t status,
    const uint8_t *payload,
    size_t payload_size
) {
    fixture_t *fixture = context;
    (void)domain;
    (void)status;
    (void)payload;
    (void)payload_size;
    fixture->completions++;
    fixture->last_completion = result;
}
static void on_device_info(
    void *context,
    wlh_host_result_t result,
    uint16_t domain,
    int16_t status,
    const wlh_host_device_info_t *info
) {
    fixture_t *fixture = context;
    (void)domain;
    (void)status;
    fixture->device_info_callbacks++;
    fixture->device_info_result = result;
    if (info != NULL)
        fixture->device_info = *info;
    else
        memset(&fixture->device_info, 0, sizeof(fixture->device_info));
}

static void fixture_init(fixture_t *fixture) {
    wlh_host_config_t config;
    memset(fixture, 0, sizeof(*fixture));
    memset(&config, 0, sizeof(config));
    config.transport = (wlh_transport_ops_t){
        fixture, transport_start, transport_stop, transport_submit
    };
    config.buffers = (wlh_buffer_ops_t){fixture, buffer_alloc, buffer_free};
    config.osal = make_test_osal(&fixture->now);
    config.executor = (wlh_executor_ops_t){fixture, post_now};

    config.on_event = on_event;
    config.event_context = fixture;

    config.max_frame_size = 4096u;
    config.rpc_timeout_ms = 100u;
    config.heartbeat_timeout_ms = 5000u;
    config.max_pending_rpc = 4u;
    config.core_queue_depth = 8u;
    assert(wlh_host_init(&fixture->host, &config) == WLH_HOST_OK);
}

static void wait_milliseconds(uint32_t milliseconds) {
    struct timespec value = {
        (time_t)(milliseconds / 1000u), (long)(milliseconds % 1000u) * 1000000L
    };
    (void)nanosleep(&value, NULL);
}

static void wait_for_state(fixture_t *fixture, wlh_host_state_t state) {
    unsigned attempt;
    for (attempt = 0; attempt < 1000u; ++attempt) {
        wlh_host_diagnostics_t diagnostics;
        wlh_host_get_diagnostics(&fixture->host, &diagnostics);
        if (diagnostics.state == state)
            return;
        wait_milliseconds(1u);
    }
    fprintf(
        stderr,
        "timed out waiting for state %d (current %d)\n",
        state,
        fixture->host.state
    );
    assert(!"timed out waiting for state");
}

static void wait_for_tx(fixture_t *fixture, unsigned count) {
    unsigned attempt;
    for (attempt = 0; attempt < 1000u && fixture->tx_count < count; ++attempt)
        wait_milliseconds(1u);
    assert(fixture->tx_count >= count);
}

static void wait_for_completion(fixture_t *fixture, unsigned count) {
    unsigned attempt;
    for (attempt = 0; attempt < 1000u && fixture->completions < count;
         ++attempt)
        wait_milliseconds(1u);
    assert(fixture->completions >= count);
}

static size_t make_rpc_frame(
    uint8_t *output,
    uint32_t session_id,
    uint32_t sequence,
    uint16_t service,
    uint16_t method,
    uint32_t request_id,
    uint8_t kind,
    int16_t status,
    const uint8_t *payload,
    size_t payload_size
) {
    uint8_t rpc[2048];
    size_t rpc_size = 0u;
    size_t frame_size = 0u;
    wlh_rpc_envelope_t envelope;
    wlh_frame_header_t header;
    memset(&envelope, 0, sizeof(envelope));
    envelope.service_id = service;
    envelope.method_id = method;
    envelope.request_id = request_id;
    envelope.kind = kind;
    envelope.status_domain = status == 0 ? 0u : WLH_STATUS_DOMAIN_PROTOCOL;
    envelope.status_code = status;
    assert(
        wlh_rpc_encode(
            rpc, sizeof(rpc), &rpc_size, &envelope, payload, payload_size
        ) == WLH_WIRE_OK
    );
    wlh_frame_header_init(
        &header,
        service == WLH_SERVICE_LINK ? WLH_CHANNEL_LINK_CONTROL
                                    : WLH_CHANNEL_CONTROL_RPC
    );
    header.session_id = session_id;
    header.sequence = sequence;
    assert(
        wlh_frame_encode(output, 4096u, &frame_size, &header, rpc, rpc_size) ==
        WLH_WIRE_OK
    );
    return frame_size;
}

static void establish_ready(fixture_t *fixture) {
    wlh_protocol_v1_HelloResponse hello =
        wlh_protocol_v1_HelloResponse_init_zero;
    uint8_t payload[1024];
    uint8_t frame[4096];
    pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
    size_t frame_size;
    assert(wlh_host_start(&fixture->host) == WLH_HOST_OK);
    wait_for_state(fixture, WLH_HOST_STATE_NEGOTIATING);
    wait_for_tx(fixture, 1u);
    hello.has_selected_protocol = true;
    hello.selected_protocol.major = 1u;
    hello.session_id = 42u;
    hello.boot_id = 99u;
    hello.max_frame_size = 4096u;
    hello.alignment = 1u;
    hello.checksum_mode = wlh_protocol_v1_ChecksumMode_CHECKSUM_MODE_SUM32;

    hello.initial_credits_count = 2u;
    hello.initial_credits[0] =
        (wlh_protocol_v1_InitialCredit){WLH_CHANNEL_CONTROL_RPC, 8u, 1u};
    hello.initial_credits[1] =
        (wlh_protocol_v1_InitialCredit){WLH_CHANNEL_ETHERNET_STA, 2u, 1u};
    assert(pb_encode(&stream, wlh_protocol_v1_HelloResponse_fields, &hello));
    frame_size = make_rpc_frame(
        frame,
        0u,
        0u,
        WLH_SERVICE_LINK,
        WLH_LINK_METHOD_HELLO,
        1u,
        WLH_RPC_KIND_RESPONSE,
        0,
        payload,
        stream.bytes_written
    );
    assert(wlh_host_on_frame(&fixture->host, frame, frame_size) == WLH_HOST_OK);
    wait_for_state(fixture, WLH_HOST_STATE_READY);
}

static uint32_t captured_request_id(
    const fixture_t *fixture, uint16_t *service, uint16_t *method
) {
    wlh_frame_header_t header;
    const uint8_t *frame_payload;
    size_t frame_payload_size;
    wlh_rpc_envelope_t envelope;
    const uint8_t *payload;
    size_t payload_size;
    assert(
        wlh_frame_decode(
            &header,
            &frame_payload,
            &frame_payload_size,
            fixture->tx,
            fixture->tx_size,
            sizeof(fixture->tx)
        ) == WLH_WIRE_OK
    );
    assert(
        wlh_rpc_decode(
            &envelope,
            &payload,
            &payload_size,
            frame_payload,
            frame_payload_size,
            2048u
        ) == WLH_WIRE_OK
    );
    *service = envelope.service_id;
    *method = envelope.method_id;
    return envelope.request_id;
}

static void test_handshake_and_rpc(void) {
    fixture_t fixture;
    uint8_t frame[4096];
    uint16_t service, method;
    uint32_t request_id;
    size_t frame_size;
    fixture_init(&fixture);
    establish_ready(&fixture);
    {
        unsigned tx_before = fixture.tx_count;
        assert(
            wlh_host_wifi_initialize(&fixture.host, on_completion, &fixture) ==
            WLH_HOST_OK
        );
        wait_for_tx(&fixture, tx_before + 1u);
    }
    request_id = captured_request_id(&fixture, &service, &method);
    assert(service == WLH_SERVICE_WIFI && method == WLH_WIFI_METHOD_INITIALIZE);
    frame_size = make_rpc_frame(
        frame,
        42u,
        0u,
        service,
        method,
        request_id,
        WLH_RPC_KIND_RESPONSE,
        0,
        NULL,
        0u
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    wait_for_completion(&fixture, 1u);
    assert(fixture.last_completion == WLH_HOST_OK);

    frame_size = make_rpc_frame(
        frame,
        42u,
        1u,
        service,
        method,
        request_id + 100u,
        WLH_RPC_KIND_RESPONSE,
        0,
        NULL,
        0u
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    wait_milliseconds(20u);
    assert(wlh_host_stop(&fixture.host) == WLH_HOST_OK);
    assert(fixture.starts == 1u && fixture.stops == 1u);
}

static void test_timeout_credit_and_session(void) {
    fixture_t fixture;
    uint8_t ethernet[60] = {0};
    uint8_t frame[4096];
    uint16_t service, method;
    uint32_t request_id;
    size_t frame_size;
    fixture_init(&fixture);
    establish_ready(&fixture);
    {
        unsigned tx_before = fixture.tx_count;
        assert(
            wlh_host_wifi_disconnect(&fixture.host, on_completion, &fixture) ==
            WLH_HOST_OK
        );
        wait_for_tx(&fixture, tx_before + 1u);
    }
    (void)captured_request_id(&fixture, &service, &method);
    fixture.now = 101u;
    wait_for_completion(&fixture, 1u);
    assert(fixture.last_completion == WLH_HOST_TIMEOUT);
    assert(
        wlh_host_ethernet_sta_send(&fixture.host, ethernet, sizeof(ethernet)) ==
        WLH_HOST_OK
    );
    assert(
        wlh_host_ethernet_sta_send(&fixture.host, ethernet, sizeof(ethernet)) ==
        WLH_HOST_OK
    );
    wait_milliseconds(20u);
    frame_size = make_rpc_frame(
        frame,
        77u,
        0u,
        WLH_SERVICE_DIAGNOSTICS,
        WLH_DIAGNOSTICS_METHOD_PING,
        9u,
        WLH_RPC_KIND_EVENT,
        0,
        NULL,
        0u
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    wait_for_state(&fixture, WLH_HOST_STATE_NEGOTIATING);
    wait_for_tx(&fixture, 5u);
    request_id = captured_request_id(&fixture, &service, &method);
    assert(
        request_id != 0u && service == WLH_SERVICE_LINK &&
        method == WLH_LINK_METHOD_HELLO
    );
    assert(wlh_host_stop(&fixture.host) == WLH_HOST_OK);
}

static void test_asynchronous_transport_start(void) {
    fixture_t fixture;
    fixture_init(&fixture);
    fixture.defer_start = true;
    assert(wlh_host_start(&fixture.host) == WLH_HOST_OK);
    wait_for_state(&fixture, WLH_HOST_STATE_TRANSPORT_STARTING);
    assert(fixture.tx_count == 0u);
    assert(fixture.start_completion != NULL);
    fixture.start_completion(fixture.start_completion_context, 0);
    wait_for_state(&fixture, WLH_HOST_STATE_NEGOTIATING);
    wait_for_tx(&fixture, 1u);
    assert(wlh_host_stop(&fixture.host) == WLH_HOST_OK);
}

static void test_device_info_and_user_passthrough(void) {
    fixture_t fixture;
    uint8_t frame[4096];
    uint16_t service, method;
    uint32_t request_id;
    size_t frame_size;
    unsigned tx_before;
    unsigned attempt;
    fixture_init(&fixture);
    establish_ready(&fixture);

    /* GET_INFO request reaches the wire and the response is decoded. */
    tx_before = fixture.tx_count;
    assert(
        wlh_host_get_device_info(&fixture.host, on_device_info, &fixture) ==
        WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    request_id = captured_request_id(&fixture, &service, &method);
    assert(
        service == WLH_SERVICE_DEVICE_INFO &&
        method == WLH_DEVICE_INFO_METHOD_GET_INFO
    );
    {
        wlh_protocol_v1_DeviceInfoResponse info =
            wlh_protocol_v1_DeviceInfoResponse_init_zero;
        uint8_t payload[256];
        pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
        memcpy(info.vendor, "espressif", sizeof("espressif"));
        memcpy(info.mcu_model, "ESP32-S3", sizeof("ESP32-S3"));
        info.uid.size = 6u;
        memcpy(info.uid.bytes, "\x01\x02\x03\x04\x05\x06", 6u);
        memcpy(
            info.board_profile,
            "espressif.esp32s3.coreboard.usb-wifi",
            sizeof("espressif.esp32s3.coreboard.usb-wifi")
        );
        assert(
            pb_encode(&stream, wlh_protocol_v1_DeviceInfoResponse_fields, &info)
        );
        frame_size = make_rpc_frame(
            frame,
            42u,
            0u,
            service,
            method,
            request_id,
            WLH_RPC_KIND_RESPONSE,
            0,
            payload,
            stream.bytes_written
        );
    }
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    for (attempt = 0; attempt < 1000u && fixture.device_info_callbacks == 0u;
         ++attempt)
        wait_milliseconds(1u);
    assert(fixture.device_info_callbacks == 1u);
    assert(fixture.device_info_result == WLH_HOST_OK);
    assert(strcmp(fixture.device_info.vendor, "espressif") == 0);
    assert(strcmp(fixture.device_info.mcu_model, "ESP32-S3") == 0);
    assert(
        fixture.device_info.uid_size == 6u && fixture.device_info.uid[0] == 1u
    );
    assert(
        strcmp(
            fixture.device_info.board_profile,
            "espressif.esp32s3.coreboard.usb-wifi"
        ) == 0
    );

    /* SEND encodes the request fields and completes on the ack. */
    tx_before = fixture.tx_count;
    {
        static const uint8_t user_payload[] = "hello";
        assert(
            wlh_host_user_message_send(
                &fixture.host,
                7u,
                3u,
                1u,
                user_payload,
                sizeof(user_payload) - 1u,
                on_completion,
                &fixture
            ) == WLH_HOST_OK
        );
    }
    wait_for_tx(&fixture, tx_before + 1u);
    request_id = captured_request_id(&fixture, &service, &method);
    assert(
        service == WLH_SERVICE_USER_PASSTHROUGH &&
        method == WLH_USER_PASSTHROUGH_METHOD_SEND
    );
    {
        wlh_frame_header_t header;
        const uint8_t *frame_payload;
        size_t frame_payload_size;
        wlh_rpc_envelope_t envelope;
        const uint8_t *payload;
        size_t payload_size;
        wlh_protocol_v1_UserMessageSendRequest decoded =
            wlh_protocol_v1_UserMessageSendRequest_init_zero;
        pb_istream_t stream;
        assert(
            wlh_frame_decode(
                &header,
                &frame_payload,
                &frame_payload_size,
                fixture.tx,
                fixture.tx_size,
                sizeof(fixture.tx)
            ) == WLH_WIRE_OK
        );
        assert(
            wlh_rpc_decode(
                &envelope,
                &payload,
                &payload_size,
                frame_payload,
                frame_payload_size,
                2048u
            ) == WLH_WIRE_OK
        );
        stream = pb_istream_from_buffer(payload, payload_size);
        assert(pb_decode(
            &stream, wlh_protocol_v1_UserMessageSendRequest_fields, &decoded
        ));
        assert(
            decoded.endpoint_id == 7u && decoded.message_type == 3u &&
            decoded.flags == 1u
        );
        assert(
            decoded.payload.size == 5u &&
            memcmp(decoded.payload.bytes, "hello", 5u) == 0
        );
    }
    frame_size = make_rpc_frame(
        frame,
        42u,
        1u,
        service,
        method,
        request_id,
        WLH_RPC_KIND_RESPONSE,
        0,
        NULL,
        0u
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    wait_for_completion(&fixture, 1u);
    assert(fixture.last_completion == WLH_HOST_OK);

    /* A RESULT event is dispatched as WLH_HOST_EVENT_USER_MESSAGE_RESULT. */
    {
        unsigned events_before = fixture.events;
        wlh_protocol_v1_UserMessageResultEvent event =
            wlh_protocol_v1_UserMessageResultEvent_init_zero;
        wlh_protocol_v1_UserMessageResultEvent decoded =
            wlh_protocol_v1_UserMessageResultEvent_init_zero;
        uint8_t payload[256];
        pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
        pb_istream_t istream;
        event.endpoint_id = 7u;
        event.message_type = 3u;
        event.correlation_id = request_id;
        event.result = -5;
        event.payload.size = 4u;
        memcpy(event.payload.bytes, "done", 4u);
        assert(pb_encode(
            &stream, wlh_protocol_v1_UserMessageResultEvent_fields, &event
        ));
        frame_size = make_rpc_frame(
            frame,
            42u,
            2u,
            WLH_SERVICE_USER_PASSTHROUGH,
            WLH_USER_PASSTHROUGH_EVENT_RESULT,
            0u,
            WLH_RPC_KIND_EVENT,
            0,
            payload,
            stream.bytes_written
        );
        assert(
            wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK
        );
        for (attempt = 0; attempt < 1000u && fixture.events == events_before;
             ++attempt)
            wait_milliseconds(1u);
        assert(fixture.events == events_before + 1u);
        assert(fixture.last_event_kind == WLH_HOST_EVENT_USER_MESSAGE_RESULT);
        istream = pb_istream_from_buffer(
            fixture.last_event_payload, fixture.last_event_payload_size
        );
        assert(pb_decode(
            &istream, wlh_protocol_v1_UserMessageResultEvent_fields, &decoded
        ));
        assert(
            decoded.endpoint_id == 7u && decoded.correlation_id == request_id &&
            decoded.result == -5 && decoded.payload.size == 4u &&
            memcmp(decoded.payload.bytes, "done", 4u) == 0
        );
    }
    assert(wlh_host_stop(&fixture.host) == WLH_HOST_OK);
}

static void test_wifi_softap(void) {
    fixture_t fixture;
    uint8_t frame[4096];
    uint16_t service, method;
    uint32_t request_id;
    size_t frame_size;
    unsigned tx_before;
    unsigned events_before;
    unsigned attempt;
    fixture_init(&fixture);
    establish_ready(&fixture);

    /* Invalid parameters are rejected before anything reaches the wire. */
    {
        static const uint8_t ssid[] = "office-ap";
        wlh_wifi_start_ap_params_t params;
        memset(&params, 0, sizeof(params));
        params.ssid = ssid;
        params.ssid_size = sizeof(ssid) - 1u;
        tx_before = fixture.tx_count;
        assert(
            wlh_host_wifi_start_ap(
                &fixture.host, NULL, on_completion, &fixture
            ) == WLH_HOST_INVALID_ARGUMENT
        );
        params.ssid_size = 0u;
        assert(
            wlh_host_wifi_start_ap(
                &fixture.host, &params, on_completion, &fixture
            ) == WLH_HOST_INVALID_ARGUMENT
        );
        params.ssid_size = WLH_HOST_MAX_SSID_SIZE + 1u;
        assert(
            wlh_host_wifi_start_ap(
                &fixture.host, &params, on_completion, &fixture
            ) == WLH_HOST_INVALID_ARGUMENT
        );
        params.ssid_size = sizeof(ssid) - 1u;
        params.ssid = NULL;
        assert(
            wlh_host_wifi_start_ap(
                &fixture.host, &params, on_completion, &fixture
            ) == WLH_HOST_INVALID_ARGUMENT
        );
        params.ssid = ssid;
        params.credential_size = WLH_HOST_MAX_CREDENTIAL_SIZE + 1u;
        assert(
            wlh_host_wifi_start_ap(
                &fixture.host, &params, on_completion, &fixture
            ) == WLH_HOST_INVALID_ARGUMENT
        );
        params.credential_size = 8u;
        params.credential = NULL;
        assert(
            wlh_host_wifi_start_ap(
                &fixture.host, &params, on_completion, &fixture
            ) == WLH_HOST_INVALID_ARGUMENT
        );
        wait_milliseconds(20u);
        assert(fixture.tx_count == tx_before);
    }

    /* START_AP encodes the request fields and completes on the response. */
    tx_before = fixture.tx_count;
    {
        static const uint8_t ssid[] = "office-ap";
        static const uint8_t credential[] = "s3cret-pass";
        wlh_wifi_start_ap_params_t params;
        memset(&params, 0, sizeof(params));
        params.ssid = ssid;
        params.ssid_size = sizeof(ssid) - 1u;
        params.credential = credential;
        params.credential_size = sizeof(credential) - 1u;
        params.security = wlh_protocol_v1_WifiSecurity_WIFI_SECURITY_WPA2_PSK;
        params.channel = 6u;
        params.max_clients = 4u;
        assert(
            wlh_host_wifi_start_ap(
                &fixture.host, &params, on_completion, &fixture
            ) == WLH_HOST_OK
        );
    }
    wait_for_tx(&fixture, tx_before + 1u);
    request_id = captured_request_id(&fixture, &service, &method);
    assert(service == WLH_SERVICE_WIFI && method == WLH_WIFI_METHOD_START_AP);
    {
        wlh_frame_header_t header;
        const uint8_t *frame_payload;
        size_t frame_payload_size;
        wlh_rpc_envelope_t envelope;
        const uint8_t *payload;
        size_t payload_size;
        wlh_protocol_v1_WifiStartApRequest decoded =
            wlh_protocol_v1_WifiStartApRequest_init_zero;
        pb_istream_t stream;
        assert(
            wlh_frame_decode(
                &header,
                &frame_payload,
                &frame_payload_size,
                fixture.tx,
                fixture.tx_size,
                sizeof(fixture.tx)
            ) == WLH_WIRE_OK
        );
        assert(
            wlh_rpc_decode(
                &envelope,
                &payload,
                &payload_size,
                frame_payload,
                frame_payload_size,
                2048u
            ) == WLH_WIRE_OK
        );
        stream = pb_istream_from_buffer(payload, payload_size);
        assert(pb_decode(
            &stream, wlh_protocol_v1_WifiStartApRequest_fields, &decoded
        ));
        assert(
            decoded.ssid.size == 9u &&
            memcmp(decoded.ssid.bytes, "office-ap", 9u) == 0
        );
        assert(
            decoded.credential.size == 11u &&
            memcmp(decoded.credential.bytes, "s3cret-pass", 11u) == 0
        );
        assert(
            decoded.security ==
            wlh_protocol_v1_WifiSecurity_WIFI_SECURITY_WPA2_PSK
        );
        assert(decoded.channel == 6u && decoded.max_clients == 4u);
        assert(
            decoded.band == 0 && !decoded.hidden &&
            decoded.beacon_interval_tu == 0u && !decoded.pmf_required
        );
    }
    frame_size = make_rpc_frame(
        frame,
        42u,
        0u,
        service,
        method,
        request_id,
        WLH_RPC_KIND_RESPONSE,
        0,
        NULL,
        0u
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    wait_for_completion(&fixture, 1u);
    assert(fixture.last_completion == WLH_HOST_OK);

    /* STOP_AP encodes an Empty payload. */
    tx_before = fixture.tx_count;
    assert(
        wlh_host_wifi_stop_ap(&fixture.host, on_completion, &fixture) ==
        WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    request_id = captured_request_id(&fixture, &service, &method);
    assert(service == WLH_SERVICE_WIFI && method == WLH_WIFI_METHOD_STOP_AP);
    {
        wlh_frame_header_t header;
        const uint8_t *frame_payload;
        size_t frame_payload_size;
        wlh_rpc_envelope_t envelope;
        const uint8_t *payload;
        size_t payload_size;
        wlh_protocol_v1_Empty decoded = wlh_protocol_v1_Empty_init_zero;
        pb_istream_t stream;
        assert(
            wlh_frame_decode(
                &header,
                &frame_payload,
                &frame_payload_size,
                fixture.tx,
                fixture.tx_size,
                sizeof(fixture.tx)
            ) == WLH_WIRE_OK
        );
        assert(
            wlh_rpc_decode(
                &envelope,
                &payload,
                &payload_size,
                frame_payload,
                frame_payload_size,
                2048u
            ) == WLH_WIRE_OK
        );
        stream = pb_istream_from_buffer(payload, payload_size);
        assert(pb_decode(&stream, wlh_protocol_v1_Empty_fields, &decoded));
    }
    frame_size = make_rpc_frame(
        frame,
        42u,
        1u,
        service,
        method,
        request_id,
        WLH_RPC_KIND_RESPONSE,
        0,
        NULL,
        0u
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    wait_for_completion(&fixture, 2u);
    assert(fixture.last_completion == WLH_HOST_OK);

    /* AP_CLIENT_JOINED is dispatched with its payload intact. */
    events_before = fixture.events;
    {
        wlh_protocol_v1_WifiApClientJoinedEvent event =
            wlh_protocol_v1_WifiApClientJoinedEvent_init_zero;
        wlh_protocol_v1_WifiApClientJoinedEvent decoded =
            wlh_protocol_v1_WifiApClientJoinedEvent_init_zero;
        uint8_t payload[256];
        pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
        pb_istream_t istream;
        event.has_client = true;
        event.client.mac.size = 6u;
        memcpy(event.client.mac.bytes, "\x24\x6f\x28\xaa\xbb\xcc", 6u);
        event.client.rssi_dbm = -47;
        event.client.association_id = 7u;
        assert(pb_encode(
            &stream, wlh_protocol_v1_WifiApClientJoinedEvent_fields, &event
        ));
        frame_size = make_rpc_frame(
            frame,
            42u,
            2u,
            WLH_SERVICE_WIFI,
            WLH_WIFI_EVENT_AP_CLIENT_JOINED,
            0u,
            WLH_RPC_KIND_EVENT,
            0,
            payload,
            stream.bytes_written
        );
        assert(
            wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK
        );
        for (attempt = 0; attempt < 1000u && fixture.events == events_before;
             ++attempt)
            wait_milliseconds(1u);
        assert(fixture.events == events_before + 1u);
        assert(fixture.last_event_kind == WLH_HOST_EVENT_WIFI_AP_CLIENT_JOINED);
        istream = pb_istream_from_buffer(
            fixture.last_event_payload, fixture.last_event_payload_size
        );
        assert(pb_decode(
            &istream, wlh_protocol_v1_WifiApClientJoinedEvent_fields, &decoded
        ));
        assert(
            decoded.client.mac.size == 6u &&
            memcmp(decoded.client.mac.bytes, "\x24\x6f\x28\xaa\xbb\xcc", 6u) ==
                0 &&
            decoded.client.rssi_dbm == -47 &&
            decoded.client.association_id == 7u
        );
    }

    /* AP_CLIENT_LEFT is dispatched with its payload intact. */
    events_before = fixture.events;
    {
        wlh_protocol_v1_WifiApClientLeftEvent event =
            wlh_protocol_v1_WifiApClientLeftEvent_init_zero;
        wlh_protocol_v1_WifiApClientLeftEvent decoded =
            wlh_protocol_v1_WifiApClientLeftEvent_init_zero;
        uint8_t payload[256];
        pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
        pb_istream_t istream;
        event.mac.size = 6u;
        memcpy(event.mac.bytes, "\x24\x6f\x28\xaa\xbb\xcc", 6u);
        event.association_id = 7u;
        event.ieee80211_reason = 8u;
        assert(pb_encode(
            &stream, wlh_protocol_v1_WifiApClientLeftEvent_fields, &event
        ));
        frame_size = make_rpc_frame(
            frame,
            42u,
            3u,
            WLH_SERVICE_WIFI,
            WLH_WIFI_EVENT_AP_CLIENT_LEFT,
            0u,
            WLH_RPC_KIND_EVENT,
            0,
            payload,
            stream.bytes_written
        );
        assert(
            wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK
        );
        for (attempt = 0; attempt < 1000u && fixture.events == events_before;
             ++attempt)
            wait_milliseconds(1u);
        assert(fixture.events == events_before + 1u);
        assert(fixture.last_event_kind == WLH_HOST_EVENT_WIFI_AP_CLIENT_LEFT);
        istream = pb_istream_from_buffer(
            fixture.last_event_payload, fixture.last_event_payload_size
        );
        assert(pb_decode(
            &istream, wlh_protocol_v1_WifiApClientLeftEvent_fields, &decoded
        ));
        assert(
            decoded.mac.size == 6u &&
            memcmp(decoded.mac.bytes, "\x24\x6f\x28\xaa\xbb\xcc", 6u) == 0 &&
            decoded.association_id == 7u && decoded.ieee80211_reason == 8u
        );
    }
    assert(wlh_host_stop(&fixture.host) == WLH_HOST_OK);
}

static void test_large_message_allocation_failures(void) {
    static const uint8_t payload[] = "allocation-test";
    wlh_host_t host;
    failing_allocator_t allocator;

    memset(&host, 0, sizeof(host));
    memset(&allocator, 0, sizeof(allocator));
    host.config.buffers = (wlh_buffer_ops_t){&allocator,
                                             failing_buffer_alloc,
                                             failing_buffer_free};

    allocator.fail_at = 1u;
    assert(
        wlh_host_user_message_send(
            &host, 1u, 1u, 0u, payload, sizeof(payload) - 1u, NULL, NULL
        ) == WLH_HOST_NO_MEMORY
    );
    assert(allocator.outstanding == 0u);

    allocator.attempts = 0u;
    allocator.fail_at = 2u;
    assert(
        wlh_host_user_message_send(
            &host, 1u, 1u, 0u, payload, sizeof(payload) - 1u, NULL, NULL
        ) == WLH_HOST_NO_MEMORY
    );
    assert(allocator.outstanding == 0u);
}

int main(void) {
    test_handshake_and_rpc();
    test_timeout_credit_and_session();
    test_asynchronous_transport_start();
    test_device_info_and_user_passthrough();
    test_wifi_softap();
    test_large_message_allocation_failures();
    puts("host core tests passed");
    return 0;
}
