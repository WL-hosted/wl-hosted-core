#include "host_test_support.h"

#include <assert.h>
#include <pthread.h>
#include <stdatomic.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

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
    _Atomic uint32_t *values = (_Atomic uint32_t *)s;
    (void)c;
    atomic_store(&values[0], i);
    atomic_store(&values[1], m);
    return 0;
}
static int stub_sem_take(void *c, wlh_osal_semaphore_t *s, uint32_t t) {
    _Atomic uint32_t *values = (_Atomic uint32_t *)s;
    uint32_t count = atomic_load(&values[0]);
    (void)c;
    (void)t;
    while (count != 0u) {
        if (atomic_compare_exchange_weak(&values[0], &count, count - 1u))
            return 0;
    }
    return -1;
}
static int stub_sem_give(void *c, wlh_osal_semaphore_t *s) {
    _Atomic uint32_t *values = (_Atomic uint32_t *)s;
    uint32_t count = atomic_load(&values[0]);
    uint32_t maximum = atomic_load(&values[1]);
    (void)c;
    while (count < maximum) {
        if (atomic_compare_exchange_weak(&values[0], &count, count + 1u))
            return 0;
    }
    return -1;
}
static int stub_sem_give_isr(void *c, wlh_osal_semaphore_t *s, bool *w) {
    if (w)
        *w = false;
    return stub_sem_give(c, s);
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

uint8_t *failing_buffer_alloc(void *context, size_t size) {
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

void failing_buffer_free(void *context, uint8_t *buffer) {
    failing_allocator_t *allocator = context;

    assert(buffer != NULL);
    assert(allocator->outstanding != 0u);
    --allocator->outstanding;
    free(buffer);
}
static int post_now(void *context, wlh_task_fn task, void *task_context) {
    fixture_t *fixture = context;
    if (fixture->reject_executor)
        return -1;
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
void on_completion(
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
void on_device_info(
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

void on_io_read(
    void *context,
    wlh_host_result_t result,
    uint16_t domain,
    int16_t status,
    const wlh_host_io_state_t *state
) {
    fixture_t *fixture = context;
    (void)domain;
    (void)status;
    fixture->io_read_callbacks++;
    fixture->io_read_result = result;
    if (state != NULL)
        fixture->io_state = *state;
    else
        memset(&fixture->io_state, 0, sizeof(fixture->io_state));
}

void on_adc_read(
    void *context,
    wlh_host_result_t result,
    uint16_t domain,
    int16_t status,
    const wlh_host_adc_sample_t *sample
) {
    fixture_t *fixture = context;
    (void)domain;
    (void)status;
    fixture->adc_read_callbacks++;
    fixture->adc_read_result = result;
    if (sample != NULL)
        fixture->adc_sample = *sample;
    else
        memset(&fixture->adc_sample, 0, sizeof(fixture->adc_sample));
}

void on_kv_read(
    void *context,
    wlh_host_result_t result,
    uint16_t domain,
    int16_t status,
    const char *value,
    size_t value_size
) {
    fixture_t *fixture = context;
    (void)domain;
    fixture->kv_read_callbacks++;
    fixture->kv_read_result = result;
    fixture->kv_read_status = status;
    fixture->kv_value_size = value_size;
    if (value != NULL) {
        assert(value_size < sizeof(fixture->kv_value));
        memcpy(fixture->kv_value, value, value_size);
    }
    fixture->kv_value[value_size] = '\0';
}

static wlh_host_result_t on_hci_rx(
    void *context, uint8_t h4_type, const uint8_t *payload, size_t payload_size
) {
    fixture_t *fixture = context;
    if (fixture->hci_rx_return != WLH_HOST_OK)
        return fixture->hci_rx_return;
    fixture->hci_rx_count++;
    fixture->hci_rx_type = h4_type;
    assert(payload_size <= sizeof(fixture->hci_rx_payload));
    memcpy(fixture->hci_rx_payload, payload, payload_size);
    fixture->hci_rx_size = payload_size;
    return WLH_HOST_OK;
}

static void on_hci_tx_ready(void *context) {
    ((fixture_t *)context)->hci_tx_ready_count++;
}

void on_bluetooth_info(
    void *context,
    wlh_host_result_t result,
    uint16_t domain,
    int16_t status,
    const wlh_bluetooth_controller_info_t *info
) {
    fixture_t *fixture = context;
    (void)domain;
    (void)status;
    fixture->bt_info_callbacks++;
    fixture->bt_info_result = result;
    if (info != NULL)
        fixture->bt_info = *info;
    else
        memset(&fixture->bt_info, 0, sizeof(fixture->bt_info));
}

void fixture_init(fixture_t *fixture) {
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

    config.bluetooth_hci_rx = on_hci_rx;
    config.bluetooth_hci_tx_ready = on_hci_tx_ready;
    config.bluetooth_context = fixture;

    config.max_frame_size = 4096u;
    config.rpc_timeout_ms = 100u;
    config.heartbeat_timeout_ms = 5000u;
    config.max_pending_rpc = 4u;
    config.core_queue_depth = 8u;
    assert(wlh_host_init(&fixture->host, &config) == WLH_HOST_OK);
}

void wait_milliseconds(uint32_t milliseconds) {
    struct timespec value = {
        (time_t)(milliseconds / 1000u), (long)(milliseconds % 1000u) * 1000000L
    };
    (void)nanosleep(&value, NULL);
}

void wait_for_state(fixture_t *fixture, wlh_host_state_t state) {
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

void wait_for_tx(fixture_t *fixture, unsigned count) {
    unsigned attempt;
    for (attempt = 0; attempt < 1000u && fixture->tx_count < count; ++attempt)
        wait_milliseconds(1u);
    assert(fixture->tx_count >= count);
}

void wait_for_completion(fixture_t *fixture, unsigned count) {
    unsigned attempt;
    for (attempt = 0; attempt < 1000u && fixture->completions < count;
         ++attempt)
        wait_milliseconds(1u);
    assert(fixture->completions >= count);
}

size_t make_rpc_frame(
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

void establish_ready(fixture_t *fixture) {
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

    hello.initial_credits_count = 3u;
    hello.initial_credits[0] =
        (wlh_protocol_v1_InitialCredit){WLH_CHANNEL_CONTROL_RPC, 8u, 1u};
    hello.initial_credits[1] =
        (wlh_protocol_v1_InitialCredit){WLH_CHANNEL_ETHERNET_STA, 2u, 1u};
    hello.initial_credits[2] =
        (wlh_protocol_v1_InitialCredit){WLH_CHANNEL_ETHERNET_AP, 2u, 1u};
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

/* Hello response advertising the Bluetooth service and HCI channel, with two
   initial HCI credits. Sent while the host is negotiating (session id 0). */
void send_bluetooth_hello(fixture_t *fixture, uint32_t session_id) {
    wlh_protocol_v1_HelloResponse hello =
        wlh_protocol_v1_HelloResponse_init_zero;
    uint8_t payload[1024];
    uint8_t frame[4096];
    pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
    size_t frame_size;
    hello.has_selected_protocol = true;
    hello.selected_protocol.major = 1u;
    hello.session_id = session_id;
    hello.boot_id = 99u;
    hello.max_frame_size = 4096u;
    hello.alignment = 1u;
    hello.checksum_mode = wlh_protocol_v1_ChecksumMode_CHECKSUM_MODE_SUM32;

    hello.services_count = 1u;
    hello.services[0].service_id = WLH_SERVICE_BLUETOOTH;
    hello.services[0].major = 1u;
    hello.channels_count = 1u;
    hello.channels[0].channel_id = WLH_CHANNEL_BLUETOOTH_HCI;
    hello.channels[0].max_frame_payload = 1024u;

    hello.initial_credits_count = 4u;
    hello.initial_credits[0] =
        (wlh_protocol_v1_InitialCredit){WLH_CHANNEL_CONTROL_RPC, 8u, 1u};
    hello.initial_credits[1] =
        (wlh_protocol_v1_InitialCredit){WLH_CHANNEL_ETHERNET_STA, 2u, 1u};
    hello.initial_credits[2] =
        (wlh_protocol_v1_InitialCredit){WLH_CHANNEL_ETHERNET_AP, 2u, 1u};
    hello.initial_credits[3] =
        (wlh_protocol_v1_InitialCredit){WLH_CHANNEL_BLUETOOTH_HCI, 2u, 1u};
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

void establish_ready_bluetooth(fixture_t *fixture) {
    assert(wlh_host_start(&fixture->host) == WLH_HOST_OK);
    wait_for_state(fixture, WLH_HOST_STATE_NEGOTIATING);
    wait_for_tx(fixture, 1u);
    send_bluetooth_hello(fixture, 42u);
}

size_t make_hci_channel_frame(
    uint8_t *output,
    uint8_t channel,
    uint32_t session_id,
    uint32_t sequence,
    uint8_t record_type,
    const uint8_t *payload,
    size_t payload_size
) {
    uint8_t record[2048];
    size_t record_size = 0u;
    size_t frame_size = 0u;
    wlh_frame_header_t header;
    assert(
        wlh_raw_record_encode(
            record,
            sizeof(record),
            &record_size,
            record_type,
            0u,
            payload,
            payload_size
        ) == WLH_WIRE_OK
    );
    wlh_frame_header_init(&header, channel);
    header.session_id = session_id;
    header.sequence = sequence;
    assert(
        wlh_frame_encode(
            output, 4096u, &frame_size, &header, record, record_size
        ) == WLH_WIRE_OK
    );
    return frame_size;
}

size_t make_hci_frame(
    uint8_t *output,
    uint32_t session_id,
    uint32_t sequence,
    uint8_t record_type,
    const uint8_t *payload,
    size_t payload_size
) {
    return make_hci_channel_frame(
        output,
        WLH_CHANNEL_BLUETOOTH_HCI,
        session_id,
        sequence,
        record_type,
        payload,
        payload_size
    );
}

/* Decode the single raw record inside the HCI frame the host last sent. */
void decode_tx_hci(
    const fixture_t *fixture,
    uint8_t *record_type,
    uint8_t *payload_out,
    size_t *payload_size
) {
    wlh_frame_header_t header;
    const uint8_t *frame_payload;
    size_t frame_payload_size;
    wlh_raw_record_iterator_t iterator;
    wlh_raw_record_view_t record;
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
    assert(header.channel == WLH_CHANNEL_BLUETOOTH_HCI);
    assert(
        wlh_raw_record_iterator_init(
            &iterator, frame_payload, frame_payload_size
        ) == WLH_WIRE_OK
    );
    assert(wlh_raw_record_iterator_next(&iterator, &record) == WLH_WIRE_OK);
    *record_type = record.record_type;
    memcpy(payload_out, record.payload, record.payload_size);
    *payload_size = record.payload_size;
    assert(wlh_raw_record_iterator_next(&iterator, &record) == WLH_WIRE_END);
}

uint32_t captured_request_id(
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

/* Decode the protobuf body of the frame the host last transmitted. */
void decode_tx_message(
    const fixture_t *fixture, const pb_msgdesc_t *fields, void *message
) {
    wlh_frame_header_t header;
    const uint8_t *frame_payload;
    size_t frame_payload_size;
    wlh_rpc_envelope_t envelope;
    const uint8_t *payload;
    size_t payload_size;
    pb_istream_t stream;
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
    stream = pb_istream_from_buffer(payload, payload_size);
    assert(pb_decode(&stream, fields, message));
}
