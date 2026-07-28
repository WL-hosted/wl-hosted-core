#include "wlh/host.h"

#include <assert.h>
#include <pthread.h>
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
    unsigned io_read_callbacks;
    wlh_host_result_t io_read_result;
    wlh_host_io_state_t io_state;
    unsigned adc_read_callbacks;
    wlh_host_result_t adc_read_result;
    wlh_host_adc_sample_t adc_sample;
    unsigned kv_read_callbacks;
    wlh_host_result_t kv_read_result;
    int16_t kv_read_status;
    char kv_value[WLH_HOST_MAX_KV_VALUE_SIZE + 1u];
    size_t kv_value_size;
    unsigned hci_rx_count;
    uint8_t hci_rx_type;
    uint8_t hci_rx_payload[1100];
    size_t hci_rx_size;
    wlh_host_result_t hci_rx_return;
    unsigned hci_tx_ready_count;
    unsigned bt_info_callbacks;
    wlh_host_result_t bt_info_result;
    wlh_bluetooth_controller_info_t bt_info;
    bool defer_start;
    bool reject_executor;
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

static void on_io_read(
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

static void on_adc_read(
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

static void on_kv_read(
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

static void on_bluetooth_info(
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
static void send_bluetooth_hello(fixture_t *fixture, uint32_t session_id) {
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

static void establish_ready_bluetooth(fixture_t *fixture) {
    assert(wlh_host_start(&fixture->host) == WLH_HOST_OK);
    wait_for_state(fixture, WLH_HOST_STATE_NEGOTIATING);
    wait_for_tx(fixture, 1u);
    send_bluetooth_hello(fixture, 42u);
}

static size_t make_hci_channel_frame(
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

static size_t make_hci_frame(
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
static void decode_tx_hci(
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

/* Decode the protobuf body of the frame the host last transmitted. */
static void decode_tx_message(
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

static void test_ap_ethernet(void) {
    fixture_t fixture;
    uint8_t frame[4096];
    uint8_t raw[11] = {1u, 0u, 8u, 0u, 3u, 0u, 0u, 0u, 1u, 2u, 3u};
    uint8_t ethernet[60] = {0};
    wlh_frame_header_t header;
    wlh_rpc_envelope_t rpc;
    wlh_protocol_v1_CreditUpdate update =
        wlh_protocol_v1_CreditUpdate_init_zero;
    const uint8_t *payload;
    const uint8_t *rpc_payload;
    size_t frame_size = 0u;
    size_t payload_size = 0u;
    size_t rpc_payload_size = 0u;
    pb_istream_t stream;
    unsigned events_before;
    unsigned tx_before;

    fixture_init(&fixture);
    establish_ready(&fixture);
    events_before = fixture.events;
    wlh_frame_header_init(&header, WLH_CHANNEL_ETHERNET_AP);
    header.session_id = 42u;
    assert(
        wlh_frame_encode(
            frame, sizeof(frame), &frame_size, &header, raw, sizeof(raw)
        ) == WLH_WIRE_OK
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    while (fixture.events == events_before)
        wait_milliseconds(1u);
    assert(fixture.last_event_kind == WLH_HOST_EVENT_ETHERNET_AP_RX);
    assert(
        fixture.last_event_payload_size == 3u &&
        memcmp(fixture.last_event_payload, raw + 8u, 3u) == 0
    );
    assert(
        wlh_frame_decode(
            &header,
            &payload,
            &payload_size,
            fixture.tx,
            fixture.tx_size,
            sizeof(fixture.tx)
        ) == WLH_WIRE_OK
    );
    assert(header.channel == WLH_CHANNEL_LINK_CONTROL);
    assert(
        wlh_rpc_decode(
            &rpc,
            &rpc_payload,
            &rpc_payload_size,
            payload,
            payload_size,
            sizeof(fixture.tx)
        ) == WLH_WIRE_OK
    );
    assert(
        rpc.service_id == WLH_SERVICE_LINK &&
        rpc.method_id == WLH_LINK_METHOD_CREDIT_UPDATE &&
        rpc.kind == WLH_RPC_KIND_EVENT
    );
    stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
    assert(pb_decode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update));
    assert(update.channel_id == WLH_CHANNEL_ETHERNET_AP && update.units == 1u);

    /*
     * Dropping an event because the application executor is full must not
     * permanently consume the peer's transport credit.
     */
    tx_before = fixture.tx_count;
    events_before = fixture.events;
    fixture.reject_executor = true;
    wlh_frame_header_init(&header, WLH_CHANNEL_ETHERNET_AP);
    header.session_id = 42u;
    header.sequence = 1u;
    assert(
        wlh_frame_encode(
            frame, sizeof(frame), &frame_size, &header, raw, sizeof(raw)
        ) == WLH_WIRE_OK
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    wait_for_tx(&fixture, tx_before + 1u);
    assert(fixture.events == events_before);
    fixture.reject_executor = false;
    assert(
        wlh_frame_decode(
            &header,
            &payload,
            &payload_size,
            fixture.tx,
            fixture.tx_size,
            sizeof(fixture.tx)
        ) == WLH_WIRE_OK
    );
    assert(header.channel == WLH_CHANNEL_LINK_CONTROL);
    assert(
        wlh_rpc_decode(
            &rpc,
            &rpc_payload,
            &rpc_payload_size,
            payload,
            payload_size,
            sizeof(fixture.tx)
        ) == WLH_WIRE_OK
    );
    assert(
        rpc.service_id == WLH_SERVICE_LINK &&
        rpc.method_id == WLH_LINK_METHOD_CREDIT_UPDATE
    );

    tx_before = fixture.tx_count;
    assert(
        wlh_host_ethernet_ap_send(&fixture.host, ethernet, sizeof(ethernet)) ==
        WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    assert(
        wlh_frame_decode(
            &header,
            &payload,
            &payload_size,
            fixture.tx,
            fixture.tx_size,
            sizeof(fixture.tx)
        ) == WLH_WIRE_OK
    );
    assert(header.channel == WLH_CHANNEL_ETHERNET_AP);
    assert(payload_size == sizeof(ethernet) + 8u);
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

static void test_io_adc_kv_clients(void) {
    fixture_t fixture;
    uint8_t frame[4096];
    uint8_t payload[1024];
    uint16_t service, method;
    uint32_t request_id;
    size_t frame_size;
    unsigned tx_before;
    unsigned attempt;
    uint32_t sequence = 0u;
    fixture_init(&fixture);
    establish_ready(&fixture);

    /* This test issues more RPCs than the Hello credit covers, so top the RPC
       channel up the way a real coprocessor would. */
    {
        wlh_protocol_v1_CreditUpdate update =
            wlh_protocol_v1_CreditUpdate_init_zero;
        pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
        update.channel_id = WLH_CHANNEL_CONTROL_RPC;
        update.units = 64u;
        assert(
            pb_encode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update)
        );
        frame_size = make_rpc_frame(
            frame,
            42u,
            sequence++,
            WLH_SERVICE_LINK,
            WLH_LINK_METHOD_CREDIT_UPDATE,
            0u,
            WLH_RPC_KIND_EVENT,
            0,
            payload,
            stream.bytes_written
        );
        assert(
            wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK
        );
    }

    /* CONFIGURE encodes every field and completes on the ack. */
    tx_before = fixture.tx_count;
    {
        wlh_host_io_config_t config;
        memset(&config, 0, sizeof(config));
        config.pin_id = 3u;
        config.mode = WLH_HOST_IO_MODE_OPEN_DRAIN;
        config.pull = WLH_HOST_IO_PULL_UP;
        config.initial_level = true;
        assert(
            wlh_host_io_configure(
                &fixture.host, &config, on_completion, &fixture
            ) == WLH_HOST_OK
        );
    }
    wait_for_tx(&fixture, tx_before + 1u);
    request_id = captured_request_id(&fixture, &service, &method);
    assert(service == WLH_SERVICE_IO && method == WLH_IO_METHOD_CONFIGURE);
    {
        wlh_protocol_v1_IoConfigureRequest decoded =
            wlh_protocol_v1_IoConfigureRequest_init_zero;
        decode_tx_message(
            &fixture, wlh_protocol_v1_IoConfigureRequest_fields, &decoded
        );
        assert(
            decoded.pin_id == 3u &&
            decoded.mode == wlh_protocol_v1_IoMode_IO_MODE_OPEN_DRAIN &&
            decoded.pull == wlh_protocol_v1_IoPull_IO_PULL_UP &&
            decoded.initial_level
        );
    }
    frame_size = make_rpc_frame(
        frame,
        42u,
        sequence++,
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

    /* An out-of-range mode is rejected locally, before any transmission. */
    {
        wlh_host_io_config_t config;
        memset(&config, 0, sizeof(config));
        config.pin_id = 3u;
        config.pull = WLH_HOST_IO_PULL_NONE;
        assert(
            wlh_host_io_configure(
                &fixture.host, &config, on_completion, &fixture
            ) == WLH_HOST_INVALID_ARGUMENT
        );
    }

    /* READ decodes level plus the effective mode and pull. */
    tx_before = fixture.tx_count;
    assert(
        wlh_host_io_read(&fixture.host, 5u, on_io_read, &fixture) == WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    request_id = captured_request_id(&fixture, &service, &method);
    assert(service == WLH_SERVICE_IO && method == WLH_IO_METHOD_READ);
    {
        wlh_protocol_v1_IoReadResponse response =
            wlh_protocol_v1_IoReadResponse_init_zero;
        pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
        response.pin_id = 5u;
        response.level = true;
        response.mode = wlh_protocol_v1_IoMode_IO_MODE_INPUT;
        response.pull = wlh_protocol_v1_IoPull_IO_PULL_DOWN;
        assert(
            pb_encode(&stream, wlh_protocol_v1_IoReadResponse_fields, &response)
        );
        frame_size = make_rpc_frame(
            frame,
            42u,
            sequence++,
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
    for (attempt = 0; attempt < 1000u && fixture.io_read_callbacks == 0u;
         ++attempt)
        wait_milliseconds(1u);
    assert(fixture.io_read_callbacks == 1u);
    assert(fixture.io_read_result == WLH_HOST_OK);
    assert(
        fixture.io_state.pin_id == 5u && fixture.io_state.level &&
        fixture.io_state.mode == WLH_HOST_IO_MODE_INPUT &&
        fixture.io_state.pull == WLH_HOST_IO_PULL_DOWN
    );

    /* A response naming an unknown mode is a protocol error, not a guess. */
    tx_before = fixture.tx_count;
    assert(
        wlh_host_io_read(&fixture.host, 6u, on_io_read, &fixture) == WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    request_id = captured_request_id(&fixture, &service, &method);
    {
        wlh_protocol_v1_IoReadResponse response =
            wlh_protocol_v1_IoReadResponse_init_zero;
        pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
        response.pin_id = 6u;
        assert(
            pb_encode(&stream, wlh_protocol_v1_IoReadResponse_fields, &response)
        );
        frame_size = make_rpc_frame(
            frame,
            42u,
            sequence++,
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
    for (attempt = 0; attempt < 1000u && fixture.io_read_callbacks == 1u;
         ++attempt)
        wait_milliseconds(1u);
    assert(fixture.io_read_callbacks == 2u);
    assert(fixture.io_read_result == WLH_HOST_PROTOCOL_ERROR);

    /* WRITE carries the pin and level. */
    tx_before = fixture.tx_count;
    assert(
        wlh_host_io_write(&fixture.host, 5u, true, on_completion, &fixture) ==
        WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    request_id = captured_request_id(&fixture, &service, &method);
    assert(service == WLH_SERVICE_IO && method == WLH_IO_METHOD_WRITE);
    {
        wlh_protocol_v1_IoWriteRequest decoded =
            wlh_protocol_v1_IoWriteRequest_init_zero;
        decode_tx_message(
            &fixture, wlh_protocol_v1_IoWriteRequest_fields, &decoded
        );
        assert(decoded.pin_id == 5u && decoded.level);
    }

    /* ADC READ decodes the calibrated sample. */
    tx_before = fixture.tx_count;
    assert(
        wlh_host_adc_read(&fixture.host, 2u, on_adc_read, &fixture) ==
        WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    request_id = captured_request_id(&fixture, &service, &method);
    assert(service == WLH_SERVICE_ADC && method == WLH_ADC_METHOD_READ);
    {
        wlh_protocol_v1_AdcReadResponse response =
            wlh_protocol_v1_AdcReadResponse_init_zero;
        pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
        response.pin_id = 2u;
        response.millivolts = 1234u;
        assert(pb_encode(
            &stream, wlh_protocol_v1_AdcReadResponse_fields, &response
        ));
        frame_size = make_rpc_frame(
            frame,
            42u,
            sequence++,
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
    for (attempt = 0; attempt < 1000u && fixture.adc_read_callbacks == 0u;
         ++attempt)
        wait_milliseconds(1u);
    assert(fixture.adc_read_callbacks == 1u);
    assert(fixture.adc_read_result == WLH_HOST_OK);
    assert(
        fixture.adc_sample.pin_id == 2u &&
        fixture.adc_sample.millivolts == 1234u
    );

    /* KV WRITE encodes both strings. */
    tx_before = fixture.tx_count;
    assert(
        wlh_host_kv_write(
            &fixture.host, "boot_count", "7", on_completion, &fixture
        ) == WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    request_id = captured_request_id(&fixture, &service, &method);
    assert(service == WLH_SERVICE_KV && method == WLH_KV_METHOD_WRITE);
    {
        wlh_protocol_v1_KvWriteRequest decoded =
            wlh_protocol_v1_KvWriteRequest_init_zero;
        decode_tx_message(
            &fixture, wlh_protocol_v1_KvWriteRequest_fields, &decoded
        );
        assert(
            strcmp(decoded.key, "boot_count") == 0 &&
            strcmp(decoded.value, "7") == 0
        );
    }

    /* KV READ hands the value to the typed completion. */
    tx_before = fixture.tx_count;
    assert(
        wlh_host_kv_read(&fixture.host, "boot_count", on_kv_read, &fixture) ==
        WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    request_id = captured_request_id(&fixture, &service, &method);
    assert(service == WLH_SERVICE_KV && method == WLH_KV_METHOD_READ);
    {
        wlh_protocol_v1_KvReadRequest decoded =
            wlh_protocol_v1_KvReadRequest_init_zero;
        wlh_protocol_v1_KvReadResponse response =
            wlh_protocol_v1_KvReadResponse_init_zero;
        pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
        decode_tx_message(
            &fixture, wlh_protocol_v1_KvReadRequest_fields, &decoded
        );
        assert(strcmp(decoded.key, "boot_count") == 0);
        snprintf(response.value, sizeof(response.value), "7");
        assert(
            pb_encode(&stream, wlh_protocol_v1_KvReadResponse_fields, &response)
        );
        frame_size = make_rpc_frame(
            frame,
            42u,
            sequence++,
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
    for (attempt = 0; attempt < 1000u && fixture.kv_read_callbacks == 0u;
         ++attempt)
        wait_milliseconds(1u);
    assert(fixture.kv_read_callbacks == 1u);
    assert(fixture.kv_read_result == WLH_HOST_OK);
    assert(fixture.kv_value_size == 1u && strcmp(fixture.kv_value, "7") == 0);

    /* A NOT_FOUND response reaches the caller with the wire status intact and
       no value. */
    tx_before = fixture.tx_count;
    assert(
        wlh_host_kv_read(&fixture.host, "absent", on_kv_read, &fixture) ==
        WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    request_id = captured_request_id(&fixture, &service, &method);
    frame_size = make_rpc_frame(
        frame,
        42u,
        sequence++,
        service,
        method,
        request_id,
        WLH_RPC_KIND_RESPONSE,
        WLH_STATUS_NOT_FOUND,
        NULL,
        0u
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    for (attempt = 0; attempt < 1000u && fixture.kv_read_callbacks == 1u;
         ++attempt)
        wait_milliseconds(1u);
    assert(fixture.kv_read_callbacks == 2u);
    assert(fixture.kv_read_result == WLH_HOST_PROTOCOL_ERROR);
    assert(fixture.kv_read_status == WLH_STATUS_NOT_FOUND);
    assert(fixture.kv_value_size == 0u);

    /* KV ERASE carries the key. */
    tx_before = fixture.tx_count;
    assert(
        wlh_host_kv_erase(
            &fixture.host, "boot_count", on_completion, &fixture
        ) == WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    request_id = captured_request_id(&fixture, &service, &method);
    assert(service == WLH_SERVICE_KV && method == WLH_KV_METHOD_ERASE);
    {
        wlh_protocol_v1_KvEraseRequest decoded =
            wlh_protocol_v1_KvEraseRequest_init_zero;
        decode_tx_message(
            &fixture, wlh_protocol_v1_KvEraseRequest_fields, &decoded
        );
        assert(strcmp(decoded.key, "boot_count") == 0);
    }

    /* Keys and values outside the negotiated bounds never reach the wire. */
    {
        char long_key[WLH_HOST_MAX_KV_KEY_SIZE + 2u];
        char long_value[WLH_HOST_MAX_KV_VALUE_SIZE + 2u];
        memset(long_key, 'k', sizeof(long_key) - 1u);
        long_key[sizeof(long_key) - 1u] = '\0';
        memset(long_value, 'v', sizeof(long_value) - 1u);
        long_value[sizeof(long_value) - 1u] = '\0';
        assert(
            wlh_host_kv_read(&fixture.host, "", on_kv_read, &fixture) ==
            WLH_HOST_INVALID_ARGUMENT
        );
        assert(
            wlh_host_kv_read(&fixture.host, long_key, on_kv_read, &fixture) ==
            WLH_HOST_INVALID_ARGUMENT
        );
        assert(
            wlh_host_kv_write(
                &fixture.host, "k", long_value, on_completion, &fixture
            ) == WLH_HOST_INVALID_ARGUMENT
        );
        assert(
            wlh_host_kv_erase(&fixture.host, "", on_completion, &fixture) ==
            WLH_HOST_INVALID_ARGUMENT
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

static void test_bluetooth_not_negotiated(void) {
    fixture_t fixture;
    static const uint8_t reset_command[] = {0x03u, 0x0cu, 0x00u};
    fixture_init(&fixture);
    establish_ready(&fixture);
    /* The Hello response omitted the Bluetooth service and channel, so every
       Bluetooth entry point reports NOT_SUPPORTED before touching the wire. */
    assert(
        wlh_host_bluetooth_initialize(
            &fixture.host, 0u, on_completion, &fixture
        ) == WLH_HOST_NOT_SUPPORTED
    );
    assert(
        wlh_host_bluetooth_enable(&fixture.host, 0u, on_completion, &fixture) ==
        WLH_HOST_NOT_SUPPORTED
    );
    assert(
        wlh_host_bluetooth_disable(&fixture.host, on_completion, &fixture) ==
        WLH_HOST_NOT_SUPPORTED
    );
    assert(
        wlh_host_bluetooth_deinitialize(
            &fixture.host, false, on_completion, &fixture
        ) == WLH_HOST_NOT_SUPPORTED
    );
    assert(
        wlh_host_bluetooth_get_info(
            &fixture.host, on_bluetooth_info, &fixture
        ) == WLH_HOST_NOT_SUPPORTED
    );
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host,
            WLH_H4_TYPE_COMMAND,
            reset_command,
            sizeof(reset_command)
        ) == WLH_HOST_NOT_SUPPORTED
    );
    assert(fixture.completions == 0u && fixture.bt_info_callbacks == 0u);
    assert(wlh_host_stop(&fixture.host) == WLH_HOST_OK);
}

static void test_bluetooth_lifecycle_and_info(void) {
    fixture_t fixture;
    uint8_t frame[4096];
    uint8_t payload[512];
    uint16_t service, method;
    uint32_t request_id;
    size_t frame_size;
    unsigned tx_before;
    unsigned attempt;
    uint32_t sequence = 0u;
    fixture_init(&fixture);
    establish_ready_bluetooth(&fixture);

    /* INITIALIZE encodes the HCI transport and completes on the ack. */
    tx_before = fixture.tx_count;
    assert(
        wlh_host_bluetooth_initialize(
            &fixture.host, 5u, on_completion, &fixture
        ) == WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    request_id = captured_request_id(&fixture, &service, &method);
    assert(
        service == WLH_SERVICE_BLUETOOTH &&
        method == WLH_BLUETOOTH_METHOD_INITIALIZE
    );
    {
        wlh_protocol_v1_BluetoothInitializeRequest decoded =
            wlh_protocol_v1_BluetoothInitializeRequest_init_zero;
        decode_tx_message(
            &fixture,
            wlh_protocol_v1_BluetoothInitializeRequest_fields,
            &decoded
        );
        assert(
            decoded.transport ==
                wlh_protocol_v1_BluetoothTransport_BLUETOOTH_TRANSPORT_HCI &&
            decoded.feature_flags == 5u
        );
    }
    frame_size = make_rpc_frame(
        frame,
        42u,
        sequence++,
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

    /* GET_INFO decodes and validates the controller descriptor. */
    tx_before = fixture.tx_count;
    assert(
        wlh_host_bluetooth_get_info(
            &fixture.host, on_bluetooth_info, &fixture
        ) == WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    request_id = captured_request_id(&fixture, &service, &method);
    assert(
        service == WLH_SERVICE_BLUETOOTH &&
        method == WLH_BLUETOOTH_METHOD_GET_INFO
    );
    {
        wlh_protocol_v1_BluetoothControllerInfo info =
            wlh_protocol_v1_BluetoothControllerInfo_init_zero;
        pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
        info.state =
            wlh_protocol_v1_BluetoothControllerState_BLUETOOTH_CONTROLLER_STATE_ENABLED;
        info.public_address.size = 6u;
        memcpy(info.public_address.bytes, "\x11\x22\x33\x44\x55\x66", 6u);
        info.hci_version = 12u;
        info.manufacturer_id = 0x02e5u;
        info.feature_bits = 0x123456789abcdef0ull;
        info.max_hci_packet = 1021u;
        assert(pb_encode(
            &stream, wlh_protocol_v1_BluetoothControllerInfo_fields, &info
        ));
        frame_size = make_rpc_frame(
            frame,
            42u,
            sequence++,
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
    for (attempt = 0; attempt < 1000u && fixture.bt_info_callbacks == 0u;
         ++attempt)
        wait_milliseconds(1u);
    assert(fixture.bt_info_callbacks == 1u);
    assert(fixture.bt_info_result == WLH_HOST_OK);
    assert(fixture.bt_info.state == WLH_BLUETOOTH_STATE_ENABLED);
    assert(fixture.bt_info.has_public_address);
    assert(
        memcmp(
            fixture.bt_info.public_address, "\x11\x22\x33\x44\x55\x66", 6u
        ) == 0
    );
    assert(fixture.bt_info.hci_version == 12u);
    assert(fixture.bt_info.manufacturer_id == 0x02e5u);
    assert(fixture.bt_info.feature_bits == 0x123456789abcdef0ull);
    assert(fixture.bt_info.max_hci_packet == 1021u);

    /* A truncated public address is a protocol error, not a guess. */
    tx_before = fixture.tx_count;
    assert(
        wlh_host_bluetooth_get_info(
            &fixture.host, on_bluetooth_info, &fixture
        ) == WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    request_id = captured_request_id(&fixture, &service, &method);
    {
        wlh_protocol_v1_BluetoothControllerInfo info =
            wlh_protocol_v1_BluetoothControllerInfo_init_zero;
        pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
        info.public_address.size = 3u;
        assert(pb_encode(
            &stream, wlh_protocol_v1_BluetoothControllerInfo_fields, &info
        ));
        frame_size = make_rpc_frame(
            frame,
            42u,
            sequence++,
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
    for (attempt = 0; attempt < 1000u && fixture.bt_info_callbacks == 1u;
         ++attempt)
        wait_milliseconds(1u);
    assert(fixture.bt_info_callbacks == 2u);
    assert(fixture.bt_info_result == WLH_HOST_PROTOCOL_ERROR);

    /* STATE_CHANGED is normalized into the host event struct. */
    {
        unsigned events_before = fixture.events;
        wlh_protocol_v1_BluetoothStateChangedEvent event =
            wlh_protocol_v1_BluetoothStateChangedEvent_init_zero;
        pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
        const wlh_host_bluetooth_state_event_t *decoded;
        event.state =
            wlh_protocol_v1_BluetoothControllerState_BLUETOOTH_CONTROLLER_STATE_ENABLED;
        event.reason = 7u;
        assert(pb_encode(
            &stream, wlh_protocol_v1_BluetoothStateChangedEvent_fields, &event
        ));
        frame_size = make_rpc_frame(
            frame,
            42u,
            sequence++,
            WLH_SERVICE_BLUETOOTH,
            WLH_BLUETOOTH_EVENT_STATE_CHANGED,
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
        assert(
            fixture.last_event_kind == WLH_HOST_EVENT_BLUETOOTH_STATE_CHANGED
        );
        assert(
            fixture.last_event_payload_size ==
            sizeof(wlh_host_bluetooth_state_event_t)
        );
        decoded = (const wlh_host_bluetooth_state_event_t *)
                      fixture.last_event_payload;
        assert(
            decoded->state == WLH_BLUETOOTH_STATE_ENABLED &&
            decoded->reason == 7u
        );
    }
    assert(wlh_host_stop(&fixture.host) == WLH_HOST_OK);
}

static void test_bluetooth_hci_channel(void) {
    fixture_t fixture;
    uint8_t frame[4096];
    uint8_t payload[512];
    uint8_t record_payload[1100];
    uint8_t record_type;
    size_t record_size;
    size_t frame_size;
    unsigned tx_before;
    unsigned attempt;
    static const uint8_t reset_command[] = {0x03u, 0x0cu, 0x00u};
    static const uint8_t acl_packet[] = {
        0x01u, 0x00u, 0x02u, 0x00u, 0xaau, 0xbbu
    };
    static const uint8_t event_packet[] = {
        0x0eu, 0x04u, 0x01u, 0x03u, 0x0cu, 0x00u
    };
    fixture_init(&fixture);
    establish_ready_bluetooth(&fixture);

    /* Malformed packets and unsupported H4 types never reach the wire. */
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host, WLH_H4_TYPE_SCO, acl_packet, sizeof(acl_packet)
        ) == WLH_HOST_NOT_SUPPORTED
    );
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host, WLH_H4_TYPE_ISO, acl_packet, sizeof(acl_packet)
        ) == WLH_HOST_NOT_SUPPORTED
    );
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host, WLH_H4_TYPE_COMMAND, reset_command, 2u
        ) == WLH_HOST_INVALID_ARGUMENT
    );
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host, WLH_H4_TYPE_ACL, acl_packet, sizeof(acl_packet) - 1u
        ) == WLH_HOST_INVALID_ARGUMENT
    );
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
        ) == WLH_HOST_INVALID_ARGUMENT
    );

    /* A command goes out as one H4 raw record on the HCI channel. */
    tx_before = fixture.tx_count;
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host,
            WLH_H4_TYPE_COMMAND,
            reset_command,
            sizeof(reset_command)
        ) == WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    decode_tx_hci(&fixture, &record_type, record_payload, &record_size);
    assert(record_type == WLH_H4_TYPE_COMMAND);
    assert(
        record_size == sizeof(reset_command) &&
        memcmp(record_payload, reset_command, record_size) == 0
    );

    /* ACL data consumes the second credit. */
    tx_before = fixture.tx_count;
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host, WLH_H4_TYPE_ACL, acl_packet, sizeof(acl_packet)
        ) == WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    decode_tx_hci(&fixture, &record_type, record_payload, &record_size);
    assert(record_type == WLH_H4_TYPE_ACL);
    assert(
        record_size == sizeof(acl_packet) &&
        memcmp(record_payload, acl_packet, record_size) == 0
    );

    /* Out of credit: the send is refused without dropping anything, and the
       tx-ready edge fires exactly when usable credit returns. */
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host,
            WLH_H4_TYPE_COMMAND,
            reset_command,
            sizeof(reset_command)
        ) == WLH_HOST_NO_CREDIT
    );
    assert(fixture.hci_tx_ready_count == 0u);
    {
        wlh_protocol_v1_CreditUpdate update =
            wlh_protocol_v1_CreditUpdate_init_zero;
        pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
        update.channel_id = WLH_CHANNEL_BLUETOOTH_HCI;
        update.units = 1u;
        assert(
            pb_encode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update)
        );
        frame_size = make_rpc_frame(
            frame,
            42u,
            0u,
            WLH_SERVICE_LINK,
            WLH_LINK_METHOD_CREDIT_UPDATE,
            0u,
            WLH_RPC_KIND_EVENT,
            0,
            payload,
            stream.bytes_written
        );
        assert(
            wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK
        );
    }
    for (attempt = 0; attempt < 1000u && fixture.hci_tx_ready_count == 0u;
         ++attempt)
        wait_milliseconds(1u);
    assert(fixture.hci_tx_ready_count == 1u);
    tx_before = fixture.tx_count;
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host,
            WLH_H4_TYPE_COMMAND,
            reset_command,
            sizeof(reset_command)
        ) == WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);

    /* A valid inbound event reaches the adapter and returns the credit. */
    tx_before = fixture.tx_count;
    frame_size = make_hci_frame(
        frame, 42u, 0u, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    wait_for_tx(&fixture, tx_before + 1u);
    assert(fixture.hci_rx_count == 1u);
    assert(fixture.hci_rx_type == WLH_H4_TYPE_EVENT);
    assert(
        fixture.hci_rx_size == sizeof(event_packet) &&
        memcmp(fixture.hci_rx_payload, event_packet, sizeof(event_packet)) == 0
    );
    {
        wlh_frame_header_t header;
        wlh_rpc_envelope_t rpc;
        wlh_protocol_v1_CreditUpdate update =
            wlh_protocol_v1_CreditUpdate_init_zero;
        const uint8_t *tx_payload;
        const uint8_t *rpc_payload;
        size_t tx_payload_size = 0u;
        size_t rpc_payload_size = 0u;
        pb_istream_t stream;
        assert(
            wlh_frame_decode(
                &header,
                &tx_payload,
                &tx_payload_size,
                fixture.tx,
                fixture.tx_size,
                sizeof(fixture.tx)
            ) == WLH_WIRE_OK
        );
        assert(header.channel == WLH_CHANNEL_LINK_CONTROL);
        assert(
            wlh_rpc_decode(
                &rpc,
                &rpc_payload,
                &rpc_payload_size,
                tx_payload,
                tx_payload_size,
                sizeof(fixture.tx)
            ) == WLH_WIRE_OK
        );
        assert(
            rpc.service_id == WLH_SERVICE_LINK &&
            rpc.method_id == WLH_LINK_METHOD_CREDIT_UPDATE &&
            rpc.kind == WLH_RPC_KIND_EVENT
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        assert(
            pb_decode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update)
        );
        assert(
            update.channel_id == WLH_CHANNEL_BLUETOOTH_HCI && update.units == 1u
        );
    }

    /* When the adapter rejects a packet the drop is counted but the credit is
       still returned: withholding it would permanently shrink the channel
       window because nothing re-runs Hello after a local drop. */
    tx_before = fixture.tx_count;
    fixture.hci_rx_return = WLH_HOST_PENDING_FULL;
    frame_size = make_hci_frame(
        frame, 42u, 1u, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    wait_for_tx(&fixture, tx_before + 1u);
    assert(fixture.hci_rx_count == 1u);
    fixture.hci_rx_return = WLH_HOST_OK;
    {
        wlh_frame_header_t header;
        wlh_rpc_envelope_t rpc;
        wlh_protocol_v1_CreditUpdate update =
            wlh_protocol_v1_CreditUpdate_init_zero;
        const uint8_t *tx_payload;
        const uint8_t *rpc_payload;
        size_t tx_payload_size = 0u;
        size_t rpc_payload_size = 0u;
        pb_istream_t stream;
        assert(
            wlh_frame_decode(
                &header,
                &tx_payload,
                &tx_payload_size,
                fixture.tx,
                fixture.tx_size,
                sizeof(fixture.tx)
            ) == WLH_WIRE_OK
        );
        assert(header.channel == WLH_CHANNEL_LINK_CONTROL);
        assert(
            wlh_rpc_decode(
                &rpc,
                &rpc_payload,
                &rpc_payload_size,
                tx_payload,
                tx_payload_size,
                sizeof(fixture.tx)
            ) == WLH_WIRE_OK
        );
        assert(
            rpc.service_id == WLH_SERVICE_LINK &&
            rpc.method_id == WLH_LINK_METHOD_CREDIT_UPDATE
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        assert(
            pb_decode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update)
        );
        assert(
            update.channel_id == WLH_CHANNEL_BLUETOOTH_HCI && update.units == 1u
        );
    }
    {
        wlh_host_diagnostics_t diagnostics;
        wlh_host_get_diagnostics(&fixture.host, &diagnostics);
        assert(diagnostics.hci_drops == 1u && diagnostics.hci_malformed == 0u);
    }

    /* A malformed packet stops session HCI: STATE_CHANGED(ERROR) fires, later
       traffic is refused, and nothing was truncated or partially delivered. */
    {
        unsigned events_before = fixture.events;
        static const uint8_t bad_event[] = {
            0x0eu, 0x05u, 0x01u, 0x03u, 0x0cu, 0x00u
        };
        const wlh_host_bluetooth_state_event_t *decoded;
        frame_size = make_hci_frame(
            frame, 42u, 2u, WLH_H4_TYPE_EVENT, bad_event, sizeof(bad_event)
        );
        assert(
            wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK
        );
        for (attempt = 0; attempt < 1000u && fixture.events == events_before;
             ++attempt)
            wait_milliseconds(1u);
        assert(fixture.events == events_before + 1u);
        assert(
            fixture.last_event_kind == WLH_HOST_EVENT_BLUETOOTH_STATE_CHANGED
        );
        decoded = (const wlh_host_bluetooth_state_event_t *)
                      fixture.last_event_payload;
        assert(
            decoded->state == WLH_BLUETOOTH_STATE_ERROR &&
            decoded->reason == WLH_HOST_BLUETOOTH_REASON_MALFORMED_HCI
        );
        assert(fixture.hci_rx_count == 1u);
    }
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host,
            WLH_H4_TYPE_COMMAND,
            reset_command,
            sizeof(reset_command)
        ) == WLH_HOST_INVALID_STATE
    );
    frame_size = make_hci_frame(
        frame, 42u, 3u, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    wait_milliseconds(20u);
    assert(fixture.hci_rx_count == 1u);
    {
        wlh_host_diagnostics_t diagnostics;
        wlh_host_get_diagnostics(&fixture.host, &diagnostics);
        assert(diagnostics.hci_malformed == 1u && diagnostics.hci_drops == 2u);
    }

    /* Link recovery re-negotiates the session and reopens HCI. */
    tx_before = fixture.tx_count;
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
    wait_for_tx(&fixture, tx_before + 1u);
    send_bluetooth_hello(&fixture, 43u);
    tx_before = fixture.tx_count;
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host,
            WLH_H4_TYPE_COMMAND,
            reset_command,
            sizeof(reset_command)
        ) == WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    decode_tx_hci(&fixture, &record_type, record_payload, &record_size);
    assert(record_type == WLH_H4_TYPE_COMMAND);
    assert(wlh_host_stop(&fixture.host) == WLH_HOST_OK);
}

static void assert_last_tx_credit_update(
    const fixture_t *fixture, uint8_t channel
) {
    wlh_frame_header_t header;
    wlh_rpc_envelope_t rpc;
    wlh_protocol_v1_CreditUpdate update =
        wlh_protocol_v1_CreditUpdate_init_zero;
    const uint8_t *tx_payload;
    const uint8_t *rpc_payload;
    size_t tx_payload_size = 0u;
    size_t rpc_payload_size = 0u;
    pb_istream_t stream;
    assert(
        wlh_frame_decode(
            &header,
            &tx_payload,
            &tx_payload_size,
            fixture->tx,
            fixture->tx_size,
            sizeof(fixture->tx)
        ) == WLH_WIRE_OK
    );
    assert(header.channel == WLH_CHANNEL_LINK_CONTROL);
    assert(
        wlh_rpc_decode(
            &rpc,
            &rpc_payload,
            &rpc_payload_size,
            tx_payload,
            tx_payload_size,
            sizeof(fixture->tx)
        ) == WLH_WIRE_OK
    );
    assert(
        rpc.service_id == WLH_SERVICE_LINK &&
        rpc.method_id == WLH_LINK_METHOD_CREDIT_UPDATE
    );
    stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
    assert(pb_decode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update));
    assert(update.channel_id == channel && update.units == 1u);
}

static void test_bluetooth_adv_channel(void) {
    fixture_t fixture;
    uint8_t frame[4096];
    size_t frame_size;
    unsigned tx_before;
    unsigned attempt;
    /* LE Meta advertising report (subevent 0x02). */
    static const uint8_t adv_report[] = {
        0x3eu,
        0x0cu,
        0x02u,
        0x01u,
        0x00u,
        0x00u,
        0x01u,
        0x02u,
        0x03u,
        0x04u,
        0x05u,
        0x06u,
        0x00u,
        0xd0u
    };
    static const uint8_t acl_packet[] = {
        0x01u, 0x00u, 0x02u, 0x00u, 0xaau, 0xbbu
    };
    fixture_init(&fixture);
    assert(wlh_host_start(&fixture.host) == WLH_HOST_OK);
    wait_for_state(&fixture, WLH_HOST_STATE_NEGOTIATING);
    wait_for_tx(&fixture, 1u);

    /* The HelloRequest declares the best-effort ADV channel capability. */
    {
        wlh_frame_header_t header;
        wlh_rpc_envelope_t rpc;
        wlh_protocol_v1_HelloRequest hello =
            wlh_protocol_v1_HelloRequest_init_zero;
        const uint8_t *tx_payload;
        const uint8_t *rpc_payload;
        size_t tx_payload_size = 0u;
        size_t rpc_payload_size = 0u;
        size_t index;
        bool adv_declared = false;
        pb_istream_t stream;
        assert(
            wlh_frame_decode(
                &header,
                &tx_payload,
                &tx_payload_size,
                fixture.tx,
                fixture.tx_size,
                sizeof(fixture.tx)
            ) == WLH_WIRE_OK
        );
        assert(
            wlh_rpc_decode(
                &rpc,
                &rpc_payload,
                &rpc_payload_size,
                tx_payload,
                tx_payload_size,
                sizeof(fixture.tx)
            ) == WLH_WIRE_OK
        );
        assert(
            rpc.service_id == WLH_SERVICE_LINK &&
            rpc.method_id == WLH_LINK_METHOD_HELLO
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        assert(pb_decode(&stream, wlh_protocol_v1_HelloRequest_fields, &hello));
        for (index = 0; index < hello.channels_count; ++index) {
            if (hello.channels[index].channel_id ==
                WLH_CHANNEL_BLUETOOTH_HCI_ADV)
                adv_declared = true;
        }
        assert(adv_declared);
    }
    send_bluetooth_hello(&fixture, 42u);

    /* A valid advertising report on the ADV channel reaches the adapter and
       the credit comes back on the same channel. */
    tx_before = fixture.tx_count;
    frame_size = make_hci_channel_frame(
        frame,
        WLH_CHANNEL_BLUETOOTH_HCI_ADV,
        42u,
        0u,
        WLH_H4_TYPE_EVENT,
        adv_report,
        sizeof(adv_report)
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    wait_for_tx(&fixture, tx_before + 1u);
    assert(fixture.hci_rx_count == 1u);
    assert(fixture.hci_rx_type == WLH_H4_TYPE_EVENT);
    assert(
        fixture.hci_rx_size == sizeof(adv_report) &&
        memcmp(fixture.hci_rx_payload, adv_report, sizeof(adv_report)) == 0
    );
    assert_last_tx_credit_update(&fixture, WLH_CHANNEL_BLUETOOTH_HCI_ADV);

    /* Adapter rejection still returns the ADV credit; only diagnostics
       record the drop. */
    tx_before = fixture.tx_count;
    fixture.hci_rx_return = WLH_HOST_PENDING_FULL;
    frame_size = make_hci_channel_frame(
        frame,
        WLH_CHANNEL_BLUETOOTH_HCI_ADV,
        42u,
        1u,
        WLH_H4_TYPE_EVENT,
        adv_report,
        sizeof(adv_report)
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    wait_for_tx(&fixture, tx_before + 1u);
    fixture.hci_rx_return = WLH_HOST_OK;
    assert_last_tx_credit_update(&fixture, WLH_CHANNEL_BLUETOOTH_HCI_ADV);
    {
        wlh_host_diagnostics_t diagnostics;
        wlh_host_get_diagnostics(&fixture.host, &diagnostics);
        assert(diagnostics.hci_drops == 1u && diagnostics.hci_malformed == 0u);
    }

    /* ACL records are reliable traffic and must not ride the best-effort
       channel: treat them as malformed HCI. */
    {
        unsigned events_before = fixture.events;
        const wlh_host_bluetooth_state_event_t *decoded;
        frame_size = make_hci_channel_frame(
            frame,
            WLH_CHANNEL_BLUETOOTH_HCI_ADV,
            42u,
            2u,
            WLH_H4_TYPE_ACL,
            acl_packet,
            sizeof(acl_packet)
        );
        assert(
            wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK
        );
        for (attempt = 0; attempt < 1000u && fixture.events == events_before;
             ++attempt)
            wait_milliseconds(1u);
        assert(fixture.events == events_before + 1u);
        assert(
            fixture.last_event_kind == WLH_HOST_EVENT_BLUETOOTH_STATE_CHANGED
        );
        decoded = (const wlh_host_bluetooth_state_event_t *)
                      fixture.last_event_payload;
        assert(
            decoded->state == WLH_BLUETOOTH_STATE_ERROR &&
            decoded->reason == WLH_HOST_BLUETOOTH_REASON_MALFORMED_HCI
        );
        assert(fixture.hci_rx_count == 1u);
    }
    assert(wlh_host_stop(&fixture.host) == WLH_HOST_OK);
}

int main(void) {
    test_handshake_and_rpc();
    test_timeout_credit_and_session();
    test_ap_ethernet();
    test_asynchronous_transport_start();
    test_device_info_and_user_passthrough();
    test_io_adc_kv_clients();
    test_wifi_softap();
    test_large_message_allocation_failures();
    test_bluetooth_not_negotiated();
    test_bluetooth_lifecycle_and_info();
    test_bluetooth_hci_channel();
    test_bluetooth_adv_channel();
    puts("host core tests passed");
    return 0;
}
