#include "wlh/coproc.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#include "adc.pb.h"
#include "bluetooth.pb.h"
#include "common.pb.h"
#include "device_info.pb.h"
#include "diagnostics.pb.h"
#include "io.pb.h"
#include "kv.pb.h"
#include "link.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "user_passthrough.pb.h"
#include "wifi.pb.h"
#include "wlh/posix_osal.h"

static int failures;
#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);            \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

typedef struct fixture {
    wlh_coproc_t *core;
    uint8_t sent[4096];
    size_t sent_size;
    uint64_t now;
    int initialized;
    size_t ethernet_size;
    size_t ethernet_ap_size;
    unsigned sent_count;
    int device_info_queries;
    wlh_coproc_device_info_t device_info;
    int device_info_status;
    unsigned user_messages;
    wlh_coproc_user_message_t last_user_message;
    uint8_t last_user_payload[512];
    unsigned start_ap_calls;
    unsigned stop_ap_calls;
    wlh_coproc_wifi_ap_t last_ap_request;
    unsigned io_configures;
    unsigned io_reads;
    unsigned io_writes;
    wlh_coproc_io_config_t last_io_config;
    uint32_t last_io_pin;
    bool last_io_level;
    wlh_coproc_io_state_t io_state;
    int io_status;
    unsigned adc_reads;
    uint32_t last_adc_pin;
    uint32_t adc_millivolts;
    int adc_status;
    unsigned kv_reads;
    unsigned kv_writes;
    unsigned kv_erases;
    char last_kv_key[WLH_COPROC_KV_MAX_KEY_SIZE + 1u];
    char last_kv_written[WLH_COPROC_KV_MAX_VALUE_SIZE + 1u];
    char kv_value[WLH_COPROC_KV_MAX_VALUE_SIZE + 1u];
    size_t kv_value_size;
    int kv_status;
    unsigned bt_initializes;
    unsigned bt_enables;
    unsigned bt_disables;
    unsigned bt_deinitializes;
    unsigned bt_get_infos;
    uint32_t bt_last_operation_id;
    uint32_t bt_last_feature_flags;
    uint32_t bt_last_mode_flags;
    bool bt_last_release_memory;
    int bt_status;
    int bt_hci_status;
    unsigned bt_hci_packets;
    uint8_t bt_last_h4_type;
    uint8_t bt_last_hci[64];
    size_t bt_last_hci_size;
    unsigned bt_tx_ready_calls;
    wlh_posix_osal_t posix;
} fixture_t;

static int submit_frame(
    void *context,
    uint8_t *frame,
    size_t size,
    wlh_coproc_tx_complete_fn completion,
    void *completion_context
) {
    fixture_t *f = context;
    memcpy(f->sent, frame, size);
    f->sent_size = size;
    ++f->sent_count;
    completion(completion_context, frame, size, 0);
    return 0;
}
static uint8_t *buffer_alloc(void *context, size_t size) {
    uint8_t *buffer = malloc(size);
    (void)context;
    /* Poison the allocation so any byte the core forgets to initialize shows
       up in the emitted frame instead of silently reading back as zero. */
    if (buffer != NULL)
        memset(buffer, 0xa5, size);
    return buffer;
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

    CHECK(buffer != NULL);
    CHECK(allocator->outstanding != 0u);
    if (allocator->outstanding != 0u)
        --allocator->outstanding;
    free(buffer);
}
/* Validate an emitted Ethernet payload exactly the way the peer does, so an
   uninitialized raw-header byte fails here instead of on hardware. */
static void check_raw_record(
    const uint8_t *payload,
    size_t payload_size,
    const uint8_t *expected,
    size_t expected_size
) {
    CHECK(payload_size == expected_size + 8u);
    if (payload_size != expected_size + 8u)
        return;
    CHECK(payload[0] == 1u);
    CHECK(payload[1] == 0u);
    CHECK(payload[2] == 8u);
    CHECK(payload[3] == 0u);
    CHECK(
        ((uint32_t)payload[4] | ((uint32_t)payload[5] << 8) |
         ((uint32_t)payload[6] << 16) | ((uint32_t)payload[7] << 24)) ==
        (uint32_t)expected_size
    );
    CHECK(memcmp(payload + 8u, expected, expected_size) == 0);
}

static void ethernet_rx(void *context, const uint8_t *frame, size_t size) {
    (void)frame;
    ((fixture_t *)context)->ethernet_size = size;
}
static void ethernet_ap_rx(void *context, const uint8_t *frame, size_t size) {
    (void)frame;
    ((fixture_t *)context)->ethernet_ap_size = size;
}
static int wifi_init(
    void *context, uint32_t operation_id, uint32_t interface_flags
) {
    fixture_t *fixture = context;
    ++fixture->initialized;
    (void)interface_flags;
    (void)wlh_coproc_wifi_initialized(fixture->core, operation_id, 0);
    return 0;
}
static int get_device_info(void *context, wlh_coproc_device_info_t *info) {
    fixture_t *fixture = context;
    ++fixture->device_info_queries;
    *info = fixture->device_info;
    return fixture->device_info_status;
}
static int on_user_message(
    void *context, const wlh_coproc_user_message_t *message
) {
    fixture_t *fixture = context;
    ++fixture->user_messages;
    fixture->last_user_message = *message;
    memcpy(fixture->last_user_payload, message->payload, message->payload_size);
    fixture->last_user_message.payload = fixture->last_user_payload;
    return 0;
}
static int wifi_start_ap(void *context, const wlh_coproc_wifi_ap_t *request) {
    fixture_t *fixture = context;
    ++fixture->start_ap_calls;
    fixture->last_ap_request = *request;
    return 0;
}
static int wifi_stop_ap(void *context) {
    fixture_t *fixture = context;
    ++fixture->stop_ap_calls;
    return 0;
}

static int io_configure(void *context, const wlh_coproc_io_config_t *config) {
    fixture_t *fixture = context;
    ++fixture->io_configures;
    fixture->last_io_config = *config;
    return fixture->io_status;
}
static int io_read(
    void *context, uint32_t pin_id, wlh_coproc_io_state_t *state
) {
    fixture_t *fixture = context;
    ++fixture->io_reads;
    fixture->last_io_pin = pin_id;
    *state = fixture->io_state;
    return fixture->io_status;
}
static int io_write(void *context, uint32_t pin_id, bool level) {
    fixture_t *fixture = context;
    ++fixture->io_writes;
    fixture->last_io_pin = pin_id;
    fixture->last_io_level = level;
    return fixture->io_status;
}
static int adc_read(void *context, uint32_t pin_id, uint32_t *millivolts) {
    fixture_t *fixture = context;
    ++fixture->adc_reads;
    fixture->last_adc_pin = pin_id;
    *millivolts = fixture->adc_millivolts;
    return fixture->adc_status;
}
static int kv_read(
    void *context,
    const char *key,
    char *value,
    size_t value_capacity,
    size_t *value_size
) {
    fixture_t *fixture = context;
    ++fixture->kv_reads;
    snprintf(fixture->last_kv_key, sizeof(fixture->last_kv_key), "%s", key);
    if (fixture->kv_status != 0)
        return fixture->kv_status;
    CHECK(fixture->kv_value_size < value_capacity);
    memcpy(value, fixture->kv_value, fixture->kv_value_size);
    value[fixture->kv_value_size] = '\0';
    *value_size = fixture->kv_value_size;
    return 0;
}
static int kv_write(
    void *context, const char *key, const char *value, size_t value_size
) {
    fixture_t *fixture = context;
    ++fixture->kv_writes;
    snprintf(fixture->last_kv_key, sizeof(fixture->last_kv_key), "%s", key);
    CHECK(value_size < sizeof(fixture->last_kv_written));
    CHECK(strlen(value) == value_size);
    memcpy(fixture->last_kv_written, value, value_size);
    fixture->last_kv_written[value_size] = '\0';
    return fixture->kv_status;
}
static int kv_erase(void *context, const char *key) {
    fixture_t *fixture = context;
    ++fixture->kv_erases;
    snprintf(fixture->last_kv_key, sizeof(fixture->last_kv_key), "%s", key);
    return fixture->kv_status;
}

static int bt_initialize(
    void *context, uint32_t operation_id, uint32_t feature_flags
) {
    fixture_t *fixture = context;
    ++fixture->bt_initializes;
    fixture->bt_last_operation_id = operation_id;
    fixture->bt_last_feature_flags = feature_flags;
    return fixture->bt_status;
}
static int bt_enable(
    void *context, uint32_t operation_id, uint32_t mode_flags
) {
    fixture_t *fixture = context;
    ++fixture->bt_enables;
    fixture->bt_last_operation_id = operation_id;
    fixture->bt_last_mode_flags = mode_flags;
    return fixture->bt_status;
}
static int bt_disable(void *context, uint32_t operation_id) {
    fixture_t *fixture = context;
    ++fixture->bt_disables;
    fixture->bt_last_operation_id = operation_id;
    return fixture->bt_status;
}
static int bt_deinitialize(
    void *context, uint32_t operation_id, bool release_memory
) {
    fixture_t *fixture = context;
    ++fixture->bt_deinitializes;
    fixture->bt_last_operation_id = operation_id;
    fixture->bt_last_release_memory = release_memory;
    return fixture->bt_status;
}
static int bt_get_info(void *context, uint32_t operation_id) {
    fixture_t *fixture = context;
    ++fixture->bt_get_infos;
    fixture->bt_last_operation_id = operation_id;
    return fixture->bt_status;
}
static int bt_hci_send(
    void *context, uint8_t h4_type, const uint8_t *payload, size_t payload_size
) {
    fixture_t *fixture = context;
    ++fixture->bt_hci_packets;
    fixture->bt_last_h4_type = h4_type;
    fixture->bt_last_hci_size = payload_size;
    if (payload_size <= sizeof(fixture->bt_last_hci))
        memcpy(fixture->bt_last_hci, payload, payload_size);
    return fixture->bt_hci_status;
}
static void bt_hci_tx_ready(void *context) {
    ++((fixture_t *)context)->bt_tx_ready_calls;
}

static void wait_milliseconds(uint32_t milliseconds) {
    struct timespec value = {
        (time_t)(milliseconds / 1000u), (long)(milliseconds % 1000u) * 1000000L
    };
    (void)nanosleep(&value, NULL);
}

static void wait_for_state(wlh_coproc_t *core, wlh_coproc_state_t state) {
    unsigned attempt;
    for (attempt = 0; attempt < 1000u; ++attempt) {
        wlh_coproc_diagnostics_t diagnostics;
        wlh_coproc_get_diagnostics(core, &diagnostics);
        if (diagnostics.state == state)
            return;
        wait_milliseconds(1u);
    }
    CHECK(false);
}

static void wait_for_sent(fixture_t *fixture, unsigned count) {
    unsigned attempt;
    for (attempt = 0; attempt < 1000u && fixture->sent_count < count; ++attempt)
        wait_milliseconds(1u);
    CHECK(fixture->sent_count >= count);
}

// clang-format off
static size_t make_rpc_frame(uint8_t *output,
                             uint32_t session, uint32_t sequence,
                             uint16_t service, uint16_t method,
                             uint32_t request_id,
                             const pb_msgdesc_t *fields,
                             const void *message) {
    // clang-format on
    uint8_t protobuf[1024];
    uint8_t rpc[1200];
    size_t rpc_size = 0, frame_size = 0;
    pb_ostream_t stream = pb_ostream_from_buffer(protobuf, sizeof(protobuf));
    wlh_rpc_envelope_t envelope;
    wlh_frame_header_t header;

    CHECK(pb_encode(&stream, fields, message));

    memset(&envelope, 0, sizeof(envelope));
    envelope.service_id = service;
    envelope.method_id = method;
    envelope.request_id = request_id;
    envelope.kind = WLH_RPC_KIND_REQUEST;

    CHECK(
        wlh_rpc_encode(
            rpc,
            sizeof(rpc),
            &rpc_size,
            &envelope,
            protobuf,
            stream.bytes_written
        ) == WLH_WIRE_OK
    );

    wlh_frame_header_init(&header, service == WLH_SERVICE_LINK ? 0 : 1);
    header.session_id = session;
    header.sequence = sequence;

    CHECK(
        wlh_frame_encode(output, 4096, &frame_size, &header, rpc, rpc_size) ==
        WLH_WIRE_OK
    );
    return frame_size;
}

static void test_hello_wifi_and_ethernet(void) {
    fixture_t f;
    wlh_coproc_t core;
    wlh_coproc_config_t config;
    uint8_t incoming[4096];
    size_t incoming_size;
    wlh_protocol_v1_HelloRequest hello = wlh_protocol_v1_HelloRequest_init_zero;
    wlh_frame_header_t frame_header;
    wlh_rpc_envelope_t rpc;
    const uint8_t *frame_payload, *rpc_payload;
    size_t frame_payload_size, rpc_payload_size;
    wlh_protocol_v1_HelloResponse response =
        wlh_protocol_v1_HelloResponse_init_zero;
    pb_istream_t stream;

    memset(&f, 0, sizeof(f));
    memset(&config, 0, sizeof(config));
    wlh_posix_osal_init(&f.posix);
    f.core = &core;

    config.port.context = &f;
    config.port.submit_tx = submit_frame;
    config.port.ethernet_rx = ethernet_rx;
    config.port.ethernet_ap_rx = ethernet_ap_rx;
    config.buffers = (wlh_coproc_buffer_ops_t){&f, buffer_alloc, buffer_free};
    config.osal = wlh_posix_osal_ops(&f.posix);

    config.wifi.context = &f;
    config.wifi.initialize = wifi_init;

    config.max_frame_size = 4096;
    config.heartbeat_interval_ms = 1000;
    config.initial_credit = 8;
    config.initial_session_id = 42;
    config.core_queue_depth = 8u;

    CHECK(wlh_coproc_init(&core, &config) == WLH_COPROC_OK);
    CHECK(wlh_coproc_start(&core) == WLH_COPROC_OK);
    wait_for_state(&core, WLH_COPROC_STATE_WAITING_FOR_HELLO);

    hello.protocol_versions_count = 1;
    hello.protocol_versions[0].major = 1;
    hello.max_frame_size = 4096;
    incoming_size = make_rpc_frame(
        incoming,
        0,
        0,
        WLH_SERVICE_LINK,
        WLH_LINK_METHOD_HELLO,
        7,
        wlh_protocol_v1_HelloRequest_fields,
        &hello
    );
    CHECK(wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK);
    wait_for_state(&core, WLH_COPROC_STATE_READY);
    wait_for_sent(&f, 1u);
    CHECK(
        wlh_frame_decode(
            &frame_header,
            &frame_payload,
            &frame_payload_size,
            f.sent,
            f.sent_size,
            4096
        ) == WLH_WIRE_OK
    );
    CHECK(frame_header.session_id == 0);
    CHECK(
        wlh_rpc_decode(
            &rpc,
            &rpc_payload,
            &rpc_payload_size,
            frame_payload,
            frame_payload_size,
            1536
        ) == WLH_WIRE_OK
    );
    CHECK(rpc.request_id == 7 && rpc.kind == WLH_RPC_KIND_RESPONSE);
    stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
    CHECK(pb_decode(&stream, wlh_protocol_v1_HelloResponse_fields, &response));
    CHECK(response.session_id == 42 && response.initial_credits_count == 4);

    {
        unsigned sent_before = f.sent_count;
        wlh_protocol_v1_WifiInitializeRequest init =
            wlh_protocol_v1_WifiInitializeRequest_init_zero;
        init.interface_flags = 1u;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            0,
            WLH_SERVICE_WIFI,
            WLH_WIFI_METHOD_INITIALIZE,
            8,
            wlh_protocol_v1_WifiInitializeRequest_fields,
            &init
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        {
            unsigned attempt;
            for (attempt = 0; attempt < 1000u && f.initialized != 1; ++attempt)
                wait_milliseconds(1u);
        }
        CHECK(f.initialized == 1);
    }

    {
        unsigned sent_before = f.sent_count;
        wlh_protocol_v1_DiagnosticsPingRequest ping =
            wlh_protocol_v1_DiagnosticsPingRequest_init_zero;
        wlh_protocol_v1_DiagnosticsPingResponse pong =
            wlh_protocol_v1_DiagnosticsPingResponse_init_zero;
        ping.cookie = 0x1234u;
        ping.host_time_us = 99u;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            1,
            WLH_SERVICE_DIAGNOSTICS,
            WLH_DIAGNOSTICS_METHOD_PING,
            9,
            wlh_protocol_v1_DiagnosticsPingRequest_fields,
            &ping
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(
            wlh_frame_decode(
                &frame_header,
                &frame_payload,
                &frame_payload_size,
                f.sent,
                f.sent_size,
                4096
            ) == WLH_WIRE_OK
        );
        CHECK(
            wlh_rpc_decode(
                &rpc,
                &rpc_payload,
                &rpc_payload_size,
                frame_payload,
                frame_payload_size,
                1536
            ) == WLH_WIRE_OK
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(
            &stream, wlh_protocol_v1_DiagnosticsPingResponse_fields, &pong
        ));
        CHECK(
            pong.cookie == ping.cookie && pong.host_time_us == ping.host_time_us
        );
    }

    {
        uint8_t raw[11] = {1, 0, 8, 0, 3, 0, 0, 0, 1, 2, 3};
        wlh_frame_header_t header;
        wlh_protocol_v1_CreditUpdate update =
            wlh_protocol_v1_CreditUpdate_init_zero;
        size_t size = 0;
        unsigned sent_before = f.sent_count;
        wlh_frame_header_init(&header, WLH_CHANNEL_ETHERNET_STA);
        header.session_id = 42;
        CHECK(
            wlh_frame_encode(
                incoming, sizeof(incoming), &size, &header, raw, sizeof(raw)
            ) == WLH_WIRE_OK
        );
        CHECK(wlh_coproc_on_frame(&core, incoming, size) == WLH_COPROC_OK);
        {
            unsigned attempt;
            for (attempt = 0; attempt < 1000u && f.ethernet_size != 3u;
                 ++attempt)
                wait_milliseconds(1u);
        }
        CHECK(f.ethernet_size == 3);
        wait_for_sent(&f, sent_before + 1u);
        CHECK(
            wlh_frame_decode(
                &header,
                &frame_payload,
                &frame_payload_size,
                f.sent,
                f.sent_size,
                4096
            ) == WLH_WIRE_OK
        );
        CHECK(header.channel == WLH_CHANNEL_LINK_CONTROL);
        CHECK(
            wlh_rpc_decode(
                &rpc,
                &rpc_payload,
                &rpc_payload_size,
                frame_payload,
                frame_payload_size,
                1536
            ) == WLH_WIRE_OK
        );
        CHECK(
            rpc.service_id == WLH_SERVICE_LINK &&
            rpc.method_id == WLH_LINK_METHOD_CREDIT_UPDATE &&
            rpc.kind == WLH_RPC_KIND_EVENT
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update));
        CHECK(
            update.channel_id == WLH_CHANNEL_ETHERNET_STA && update.units == 1u
        );
    }
    {
        uint8_t raw[11] = {1, 0, 8, 0, 3, 0, 0, 0, 4, 5, 6};
        wlh_frame_header_t header;
        size_t size = 0;
        unsigned sent_before = f.sent_count;
        wlh_frame_header_init(&header, WLH_CHANNEL_ETHERNET_AP);
        header.session_id = 42;
        CHECK(
            wlh_frame_encode(
                incoming, sizeof(incoming), &size, &header, raw, sizeof(raw)
            ) == WLH_WIRE_OK
        );
        CHECK(wlh_coproc_on_frame(&core, incoming, size) == WLH_COPROC_OK);
        {
            unsigned attempt;
            for (attempt = 0; attempt < 1000u && f.ethernet_ap_size != 3u;
                 ++attempt)
                wait_milliseconds(1u);
        }
        CHECK(f.ethernet_ap_size == 3u);
        CHECK(
            wlh_coproc_ethernet_ap_send(&core, raw + 8u, 3u) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 2u);
        CHECK(
            wlh_frame_decode(
                &header,
                &frame_payload,
                &frame_payload_size,
                f.sent,
                f.sent_size,
                4096
            ) == WLH_WIRE_OK
        );
        CHECK(header.channel == WLH_CHANNEL_ETHERNET_AP);
        check_raw_record(frame_payload, frame_payload_size, raw + 8u, 3u);
    }
    {
        /* The STA send path builds the same 8-byte raw record. Both are
           checked byte for byte: the header lives in a flexible array member,
           so a sizeof(*job)-sized memset leaves bytes 1 and 3 poisoned and the
           peer drops every frame, leaking a credit each time. */
        uint8_t payload[3] = {7, 8, 9};
        wlh_frame_header_t header;
        unsigned sent_before = f.sent_count;
        CHECK(
            wlh_coproc_ethernet_sta_send(&core, payload, sizeof(payload)) ==
            WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(
            wlh_frame_decode(
                &header,
                &frame_payload,
                &frame_payload_size,
                f.sent,
                f.sent_size,
                4096
            ) == WLH_WIRE_OK
        );
        CHECK(header.channel == WLH_CHANNEL_ETHERNET_STA);
        check_raw_record(
            frame_payload, frame_payload_size, payload, sizeof(payload)
        );
    }
    CHECK(wlh_coproc_stop(&core) == WLH_COPROC_OK);
}

static void prepare_ready_core(
    fixture_t *f, wlh_coproc_t *core, bool with_optional_services
) {
    wlh_coproc_config_t config;
    uint8_t incoming[4096];
    size_t incoming_size;
    wlh_protocol_v1_HelloRequest hello = wlh_protocol_v1_HelloRequest_init_zero;

    memset(&config, 0, sizeof(config));
    config.port.context = f;
    config.port.submit_tx = submit_frame;
    config.port.ethernet_rx = ethernet_rx;
    config.buffers = (wlh_coproc_buffer_ops_t){f, buffer_alloc, buffer_free};
    config.osal = wlh_posix_osal_ops(&f->posix);
    if (with_optional_services) {
        config.device_info.context = f;
        config.device_info.get_info = get_device_info;
        config.user_passthrough.context = f;
        config.user_passthrough.on_message = on_user_message;
        config.wifi.context = f;
        config.wifi.start_ap = wifi_start_ap;
        config.wifi.stop_ap = wifi_stop_ap;
        config.io.context = f;
        config.io.configure = io_configure;
        config.io.read = io_read;
        config.io.write = io_write;
        config.adc.context = f;
        config.adc.read = adc_read;
        config.kv.context = f;
        config.kv.read = kv_read;
        config.kv.write = kv_write;
        config.kv.erase = kv_erase;
    }
    config.max_frame_size = 4096;
    config.heartbeat_interval_ms = 1000;
    config.initial_credit = 8;
    config.initial_session_id = 42;
    config.core_queue_depth = 8u;

    CHECK(wlh_coproc_init(core, &config) == WLH_COPROC_OK);
    CHECK(wlh_coproc_start(core) == WLH_COPROC_OK);
    wait_for_state(core, WLH_COPROC_STATE_WAITING_FOR_HELLO);

    hello.protocol_versions_count = 1;
    hello.protocol_versions[0].major = 1;
    hello.max_frame_size = 4096;
    incoming_size = make_rpc_frame(
        incoming,
        0,
        0,
        WLH_SERVICE_LINK,
        WLH_LINK_METHOD_HELLO,
        7,
        wlh_protocol_v1_HelloRequest_fields,
        &hello
    );
    CHECK(wlh_coproc_on_frame(core, incoming, incoming_size) == WLH_COPROC_OK);
    wait_for_state(core, WLH_COPROC_STATE_READY);
}

static void wait_for_counter(const unsigned *counter, unsigned value) {
    unsigned attempt;
    for (attempt = 0; attempt < 1000u && *counter < value; ++attempt)
        wait_milliseconds(1u);
    CHECK(*counter >= value);
}

static void wait_for_bt_mismatches(wlh_coproc_t *core, uint32_t value) {
    unsigned attempt;
    for (attempt = 0; attempt < 1000u; ++attempt) {
        wlh_coproc_diagnostics_t diagnostics;
        wlh_coproc_get_diagnostics(core, &diagnostics);
        if (diagnostics.bluetooth_mismatches >= value)
            return;
        wait_milliseconds(1u);
    }
    CHECK(false);
}

static void wait_for_hci_drops(wlh_coproc_t *core, uint32_t value) {
    unsigned attempt;
    for (attempt = 0; attempt < 1000u; ++attempt) {
        wlh_coproc_diagnostics_t diagnostics;
        wlh_coproc_get_diagnostics(core, &diagnostics);
        if (diagnostics.hci_drops >= value)
            return;
        wait_milliseconds(1u);
    }
    CHECK(false);
}

static void prepare_ready_bt_core(fixture_t *f, wlh_coproc_t *core) {
    wlh_coproc_config_t config;
    uint8_t incoming[4096];
    size_t incoming_size;
    wlh_protocol_v1_HelloRequest hello = wlh_protocol_v1_HelloRequest_init_zero;

    memset(&config, 0, sizeof(config));
    config.port.context = f;
    config.port.submit_tx = submit_frame;
    config.port.ethernet_rx = ethernet_rx;
    config.buffers = (wlh_coproc_buffer_ops_t){f, buffer_alloc, buffer_free};
    config.osal = wlh_posix_osal_ops(&f->posix);
    config.bluetooth.context = f;
    config.bluetooth.initialize = bt_initialize;
    config.bluetooth.enable = bt_enable;
    config.bluetooth.disable = bt_disable;
    config.bluetooth.deinitialize = bt_deinitialize;
    config.bluetooth.get_info = bt_get_info;
    config.bluetooth.hci_send = bt_hci_send;
    config.bluetooth.hci_tx_ready = bt_hci_tx_ready;
    config.max_frame_size = 4096;
    /* Long heartbeat so decode_last_sent never races a heartbeat frame. */
    config.heartbeat_interval_ms = 60000;
    config.initial_credit = 64;
    config.initial_session_id = 42;
    config.core_queue_depth = 8u;

    CHECK(wlh_coproc_init(core, &config) == WLH_COPROC_OK);
    CHECK(wlh_coproc_start(core) == WLH_COPROC_OK);
    wait_for_state(core, WLH_COPROC_STATE_WAITING_FOR_HELLO);

    hello.protocol_versions_count = 1;
    hello.protocol_versions[0].major = 1;
    hello.max_frame_size = 4096;
    incoming_size = make_rpc_frame(
        incoming,
        0,
        0,
        WLH_SERVICE_LINK,
        WLH_LINK_METHOD_HELLO,
        7,
        wlh_protocol_v1_HelloRequest_fields,
        &hello
    );
    CHECK(wlh_coproc_on_frame(core, incoming, incoming_size) == WLH_COPROC_OK);
    wait_for_state(core, WLH_COPROC_STATE_READY);
    wait_for_sent(f, 1u);
}

static size_t decode_last_sent(
    fixture_t *f,
    wlh_rpc_envelope_t *rpc,
    const uint8_t **rpc_payload,
    size_t *rpc_payload_size
) {
    wlh_frame_header_t frame_header;
    const uint8_t *frame_payload;
    size_t frame_payload_size;
    CHECK(
        wlh_frame_decode(
            &frame_header,
            &frame_payload,
            &frame_payload_size,
            f->sent,
            f->sent_size,
            4096
        ) == WLH_WIRE_OK
    );
    CHECK(
        wlh_rpc_decode(
            rpc,
            rpc_payload,
            rpc_payload_size,
            frame_payload,
            frame_payload_size,
            1536
        ) == WLH_WIRE_OK
    );
    return frame_payload_size;
}

static void test_device_info_and_user_passthrough(void) {
    fixture_t f;
    wlh_coproc_t core;
    uint8_t incoming[4096];
    size_t incoming_size;
    wlh_rpc_envelope_t rpc;
    const uint8_t *rpc_payload;
    size_t rpc_payload_size;
    pb_istream_t stream;
    unsigned sent_before;

    memset(&f, 0, sizeof(f));
    f.core = &core;
    wlh_posix_osal_init(&f.posix);
    memcpy(f.device_info.vendor, "espressif", sizeof("espressif"));
    memcpy(f.device_info.mcu_model, "ESP32-S3", sizeof("ESP32-S3"));
    f.device_info.uid_size = 6u;
    memcpy(f.device_info.uid, "\x01\x02\x03\x04\x05\x06", 6u);
    memcpy(
        f.device_info.board_profile,
        "espressif.esp32s3.coreboard.usb-wifi",
        sizeof("espressif.esp32s3.coreboard.usb-wifi")
    );
    prepare_ready_core(&f, &core, true);

    /* GET_INFO returns the configured fields. */
    sent_before = f.sent_count;
    {
        wlh_protocol_v1_Empty empty = wlh_protocol_v1_Empty_init_zero;
        wlh_protocol_v1_DeviceInfoResponse info =
            wlh_protocol_v1_DeviceInfoResponse_init_zero;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            0,
            WLH_SERVICE_DEVICE_INFO,
            WLH_DEVICE_INFO_METHOD_GET_INFO,
            20,
            wlh_protocol_v1_Empty_fields,
            &empty
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.device_info_queries == 1);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.request_id == 20 && rpc.kind == WLH_RPC_KIND_RESPONSE &&
            rpc.status_code == WLH_STATUS_OK
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(
            pb_decode(&stream, wlh_protocol_v1_DeviceInfoResponse_fields, &info)
        );
        CHECK(strcmp(info.vendor, "espressif") == 0);
        CHECK(strcmp(info.mcu_model, "ESP32-S3") == 0);
        CHECK(
            info.uid.size == 6u && info.uid.bytes[0] == 1u &&
            info.uid.bytes[5] == 6u
        );
        CHECK(
            strcmp(
                info.board_profile, "espressif.esp32s3.coreboard.usb-wifi"
            ) == 0
        );
    }

    /* GET_INFO propagates a backend failure as DEVICE_INFO domain error. */
    sent_before = f.sent_count;
    f.device_info_status = -1;
    {
        wlh_protocol_v1_Empty empty = wlh_protocol_v1_Empty_init_zero;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            1,
            WLH_SERVICE_DEVICE_INFO,
            WLH_DEVICE_INFO_METHOD_GET_INFO,
            21,
            wlh_protocol_v1_Empty_fields,
            &empty
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.status_domain == WLH_STATUS_DOMAIN_DEVICE_INFO &&
            rpc.status_code == WLH_STATUS_INTERNAL
        );
    }
    f.device_info_status = 0;

    /* SEND dispatches to the adapter and is acked with OK. */
    sent_before = f.sent_count;
    {
        static const uint8_t payload[] = "hello";
        wlh_protocol_v1_UserMessageSendRequest send_request =
            wlh_protocol_v1_UserMessageSendRequest_init_zero;
        send_request.endpoint_id = 7;
        send_request.message_type = 3;
        send_request.flags = 1;
        send_request.payload.size = sizeof(payload) - 1u;
        memcpy(send_request.payload.bytes, payload, sizeof(payload) - 1u);
        incoming_size = make_rpc_frame(
            incoming,
            42,
            2,
            WLH_SERVICE_USER_PASSTHROUGH,
            WLH_USER_PASSTHROUGH_METHOD_SEND,
            22,
            wlh_protocol_v1_UserMessageSendRequest_fields,
            &send_request
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.user_messages == 1u);
        CHECK(
            f.last_user_message.endpoint_id == 7 &&
            f.last_user_message.message_type == 3 &&
            f.last_user_message.flags == 1 &&
            f.last_user_message.request_id == 22
        );
        CHECK(
            f.last_user_message.payload_size == sizeof(payload) - 1u &&
            memcmp(
                f.last_user_message.payload, payload, sizeof(payload) - 1u
            ) == 0
        );
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.request_id == 22 && rpc.kind == WLH_RPC_KIND_RESPONSE &&
            rpc.status_code == WLH_STATUS_OK
        );
    }

    /* wlh_coproc_user_message_result emits a RESULT event. */
    sent_before = f.sent_count;
    {
        static const uint8_t result_payload[] = "done";
        wlh_protocol_v1_UserMessageResultEvent event =
            wlh_protocol_v1_UserMessageResultEvent_init_zero;
        CHECK(
            wlh_coproc_user_message_result(
                &core, 7, 3, 22, -5, result_payload, sizeof(result_payload) - 1u
            ) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.service_id == WLH_SERVICE_USER_PASSTHROUGH &&
            rpc.method_id == WLH_USER_PASSTHROUGH_EVENT_RESULT &&
            rpc.kind == WLH_RPC_KIND_EVENT
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(
            &stream, wlh_protocol_v1_UserMessageResultEvent_fields, &event
        ));
        CHECK(
            event.endpoint_id == 7 && event.message_type == 3 &&
            event.correlation_id == 22 && event.result == -5 &&
            event.payload.size == sizeof(result_payload) - 1u &&
            memcmp(
                event.payload.bytes, result_payload, sizeof(result_payload) - 1u
            ) == 0
        );
    }
    CHECK(wlh_coproc_stop(&core) == WLH_COPROC_OK);
}

static void test_io_adc_kv(void) {
    fixture_t f;
    wlh_coproc_t core;
    uint8_t incoming[4096];
    size_t incoming_size;
    wlh_rpc_envelope_t rpc;
    const uint8_t *rpc_payload;
    size_t rpc_payload_size;
    pb_istream_t stream;
    unsigned sent_before;
    uint32_t request_id = 100u;
    uint32_t sequence = 0u;

    memset(&f, 0, sizeof(f));
    f.core = &core;
    wlh_posix_osal_init(&f.posix);
    prepare_ready_core(&f, &core, true);

    /* This test issues more RPCs than the Hello credit covers, so top the RPC
       channel up the way a real host would. */
    sent_before = f.sent_count;
    {
        wlh_protocol_v1_CreditUpdate update =
            wlh_protocol_v1_CreditUpdate_init_zero;
        update.channel_id = WLH_CHANNEL_CONTROL_RPC;
        update.units = 64u;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            1,
            WLH_SERVICE_LINK,
            WLH_LINK_METHOD_CREDIT_UPDATE,
            request_id++,
            wlh_protocol_v1_CreditUpdate_fields,
            &update
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
    }

    /* CONFIGURE forwards the request verbatim and acks with OK. */
    sent_before = f.sent_count;
    {
        wlh_protocol_v1_IoConfigureRequest configure =
            wlh_protocol_v1_IoConfigureRequest_init_zero;
        configure.pin_id = 3u;
        configure.mode = wlh_protocol_v1_IoMode_IO_MODE_OPEN_DRAIN;
        configure.pull = wlh_protocol_v1_IoPull_IO_PULL_UP;
        configure.initial_level = true;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            sequence++,
            WLH_SERVICE_IO,
            WLH_IO_METHOD_CONFIGURE,
            request_id,
            wlh_protocol_v1_IoConfigureRequest_fields,
            &configure
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.io_configures == 1u);
        CHECK(
            f.last_io_config.pin_id == 3u &&
            f.last_io_config.mode == WLH_COPROC_IO_MODE_OPEN_DRAIN &&
            f.last_io_config.pull == WLH_COPROC_IO_PULL_UP &&
            f.last_io_config.initial_level
        );
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.request_id == request_id && rpc.kind == WLH_RPC_KIND_RESPONSE &&
            rpc.status_code == WLH_STATUS_OK
        );
    }

    /* An UNSPECIFIED mode is rejected by the core; the backend never sees it.
     */
    sent_before = f.sent_count;
    ++request_id;
    {
        wlh_protocol_v1_IoConfigureRequest configure =
            wlh_protocol_v1_IoConfigureRequest_init_zero;
        configure.pin_id = 3u;
        configure.pull = wlh_protocol_v1_IoPull_IO_PULL_NONE;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            sequence++,
            WLH_SERVICE_IO,
            WLH_IO_METHOD_CONFIGURE,
            request_id,
            wlh_protocol_v1_IoConfigureRequest_fields,
            &configure
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.io_configures == 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.status_domain == WLH_STATUS_DOMAIN_PERIPHERAL &&
            rpc.status_code == WLH_STATUS_INVALID_ARGUMENT
        );
    }

    /* READ echoes the pin and reports the configuration actually in effect. */
    sent_before = f.sent_count;
    ++request_id;
    f.io_state.level = true;
    f.io_state.mode = WLH_COPROC_IO_MODE_INPUT;
    f.io_state.pull = WLH_COPROC_IO_PULL_DOWN;
    {
        wlh_protocol_v1_IoReadRequest read =
            wlh_protocol_v1_IoReadRequest_init_zero;
        wlh_protocol_v1_IoReadResponse response =
            wlh_protocol_v1_IoReadResponse_init_zero;
        read.pin_id = 5u;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            sequence++,
            WLH_SERVICE_IO,
            WLH_IO_METHOD_READ,
            request_id,
            wlh_protocol_v1_IoReadRequest_fields,
            &read
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.io_reads == 1u && f.last_io_pin == 5u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(rpc.status_code == WLH_STATUS_OK);
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(
            pb_decode(&stream, wlh_protocol_v1_IoReadResponse_fields, &response)
        );
        CHECK(
            response.pin_id == 5u && response.level &&
            response.mode == wlh_protocol_v1_IoMode_IO_MODE_INPUT &&
            response.pull == wlh_protocol_v1_IoPull_IO_PULL_DOWN
        );
    }

    /* Writing an INPUT pin is INVALID_STATE, which the wire spells NOT_READY.
     */
    sent_before = f.sent_count;
    ++request_id;
    f.io_status = WLH_COPROC_SERVICE_INVALID_STATE;
    {
        wlh_protocol_v1_IoWriteRequest write =
            wlh_protocol_v1_IoWriteRequest_init_zero;
        write.pin_id = 5u;
        write.level = true;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            sequence++,
            WLH_SERVICE_IO,
            WLH_IO_METHOD_WRITE,
            request_id,
            wlh_protocol_v1_IoWriteRequest_fields,
            &write
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.io_writes == 1u && f.last_io_pin == 5u && f.last_io_level);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.status_domain == WLH_STATUS_DOMAIN_PERIPHERAL &&
            rpc.status_code == WLH_STATUS_NOT_READY
        );
    }
    f.io_status = 0;

    /* An unknown IO method is a protocol-level NOT_SUPPORTED. */
    sent_before = f.sent_count;
    ++request_id;
    {
        wlh_protocol_v1_IoReadRequest read =
            wlh_protocol_v1_IoReadRequest_init_zero;
        read.pin_id = 1u;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            sequence++,
            WLH_SERVICE_IO,
            0x0044,
            request_id,
            wlh_protocol_v1_IoReadRequest_fields,
            &read
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.status_domain == WLH_STATUS_DOMAIN_PROTOCOL &&
            rpc.status_code == WLH_STATUS_NOT_SUPPORTED
        );
    }

    /* ADC READ echoes the pin and reports calibrated millivolts. */
    sent_before = f.sent_count;
    ++request_id;
    f.adc_millivolts = 1234u;
    {
        wlh_protocol_v1_AdcReadRequest read =
            wlh_protocol_v1_AdcReadRequest_init_zero;
        wlh_protocol_v1_AdcReadResponse response =
            wlh_protocol_v1_AdcReadResponse_init_zero;
        read.pin_id = 2u;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            sequence++,
            WLH_SERVICE_ADC,
            WLH_ADC_METHOD_READ,
            request_id,
            wlh_protocol_v1_AdcReadRequest_fields,
            &read
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.adc_reads == 1u && f.last_adc_pin == 2u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(rpc.status_code == WLH_STATUS_OK);
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(
            &stream, wlh_protocol_v1_AdcReadResponse_fields, &response
        ));
        CHECK(response.pin_id == 2u && response.millivolts == 1234u);
    }

    /* A pin without ADC capability is NOT_SUPPORTED in the PERIPHERAL domain.
     */
    sent_before = f.sent_count;
    ++request_id;
    f.adc_status = WLH_COPROC_SERVICE_NOT_SUPPORTED;
    {
        wlh_protocol_v1_AdcReadRequest read =
            wlh_protocol_v1_AdcReadRequest_init_zero;
        read.pin_id = 7u;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            sequence++,
            WLH_SERVICE_ADC,
            WLH_ADC_METHOD_READ,
            request_id,
            wlh_protocol_v1_AdcReadRequest_fields,
            &read
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.status_domain == WLH_STATUS_DOMAIN_PERIPHERAL &&
            rpc.status_code == WLH_STATUS_NOT_SUPPORTED
        );
    }
    f.adc_status = 0;

    /* KV WRITE hands the adapter the decoded key and value. */
    sent_before = f.sent_count;
    ++request_id;
    {
        wlh_protocol_v1_KvWriteRequest write =
            wlh_protocol_v1_KvWriteRequest_init_zero;
        snprintf(write.key, sizeof(write.key), "boot_count");
        snprintf(write.value, sizeof(write.value), "7");
        incoming_size = make_rpc_frame(
            incoming,
            42,
            sequence++,
            WLH_SERVICE_KV,
            WLH_KV_METHOD_WRITE,
            request_id,
            wlh_protocol_v1_KvWriteRequest_fields,
            &write
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.kv_writes == 1u);
        CHECK(strcmp(f.last_kv_key, "boot_count") == 0);
        CHECK(strcmp(f.last_kv_written, "7") == 0);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(rpc.status_code == WLH_STATUS_OK);
    }

    /* KV READ returns the stored value. */
    sent_before = f.sent_count;
    ++request_id;
    snprintf(f.kv_value, sizeof(f.kv_value), "7");
    f.kv_value_size = 1u;
    {
        wlh_protocol_v1_KvReadRequest read =
            wlh_protocol_v1_KvReadRequest_init_zero;
        wlh_protocol_v1_KvReadResponse response =
            wlh_protocol_v1_KvReadResponse_init_zero;
        snprintf(read.key, sizeof(read.key), "boot_count");
        incoming_size = make_rpc_frame(
            incoming,
            42,
            sequence++,
            WLH_SERVICE_KV,
            WLH_KV_METHOD_READ,
            request_id,
            wlh_protocol_v1_KvReadRequest_fields,
            &read
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.kv_reads == 1u && strcmp(f.last_kv_key, "boot_count") == 0);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(rpc.status_code == WLH_STATUS_OK);
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(
            pb_decode(&stream, wlh_protocol_v1_KvReadResponse_fields, &response)
        );
        CHECK(strcmp(response.value, "7") == 0);
    }

    /* Erasing a missing key is NOT_FOUND in the STORAGE domain. */
    sent_before = f.sent_count;
    ++request_id;
    f.kv_status = WLH_COPROC_SERVICE_NOT_FOUND;
    {
        wlh_protocol_v1_KvEraseRequest erase =
            wlh_protocol_v1_KvEraseRequest_init_zero;
        snprintf(erase.key, sizeof(erase.key), "absent");
        incoming_size = make_rpc_frame(
            incoming,
            42,
            sequence++,
            WLH_SERVICE_KV,
            WLH_KV_METHOD_ERASE,
            request_id,
            wlh_protocol_v1_KvEraseRequest_fields,
            &erase
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.kv_erases == 1u && strcmp(f.last_kv_key, "absent") == 0);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.status_domain == WLH_STATUS_DOMAIN_STORAGE &&
            rpc.status_code == WLH_STATUS_NOT_FOUND
        );
    }
    f.kv_status = 0;

    /* An empty key never reaches the adapter. */
    sent_before = f.sent_count;
    ++request_id;
    {
        wlh_protocol_v1_KvReadRequest read =
            wlh_protocol_v1_KvReadRequest_init_zero;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            sequence++,
            WLH_SERVICE_KV,
            WLH_KV_METHOD_READ,
            request_id,
            wlh_protocol_v1_KvReadRequest_fields,
            &read
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.kv_reads == 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.status_domain == WLH_STATUS_DOMAIN_STORAGE &&
            rpc.status_code == WLH_STATUS_INVALID_ARGUMENT
        );
    }

    /* Nor does a key that is not valid UTF-8. */
    sent_before = f.sent_count;
    ++request_id;
    {
        wlh_protocol_v1_KvReadRequest read =
            wlh_protocol_v1_KvReadRequest_init_zero;
        read.key[0] = (char)0xffu;
        read.key[1] = (char)0xfeu;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            sequence++,
            WLH_SERVICE_KV,
            WLH_KV_METHOD_READ,
            request_id,
            wlh_protocol_v1_KvReadRequest_fields,
            &read
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.kv_reads == 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.status_domain == WLH_STATUS_DOMAIN_STORAGE &&
            rpc.status_code == WLH_STATUS_INVALID_ARGUMENT
        );
    }

    /* A multi-byte UTF-8 key is accepted and reaches the adapter intact. */
    sent_before = f.sent_count;
    ++request_id;
    f.kv_value_size = 0u;
    f.kv_value[0] = '\0';
    {
        wlh_protocol_v1_KvReadRequest read =
            wlh_protocol_v1_KvReadRequest_init_zero;
        snprintf(read.key, sizeof(read.key), "\xe6\xb8\xa9\xe5\xba\xa6");
        incoming_size = make_rpc_frame(
            incoming,
            42,
            sequence++,
            WLH_SERVICE_KV,
            WLH_KV_METHOD_READ,
            request_id,
            wlh_protocol_v1_KvReadRequest_fields,
            &read
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.kv_reads == 2u);
        CHECK(strcmp(f.last_kv_key, "\xe6\xb8\xa9\xe5\xba\xa6") == 0);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(rpc.status_code == WLH_STATUS_OK);
    }
    CHECK(wlh_coproc_stop(&core) == WLH_COPROC_OK);
}

static void test_optional_services_not_configured(void) {
    fixture_t f;
    wlh_coproc_t core;
    uint8_t incoming[4096];
    size_t incoming_size;
    wlh_rpc_envelope_t rpc;
    const uint8_t *rpc_payload;
    size_t rpc_payload_size;
    unsigned sent_before;

    memset(&f, 0, sizeof(f));
    f.core = &core;
    wlh_posix_osal_init(&f.posix);
    prepare_ready_core(&f, &core, false);

    sent_before = f.sent_count;
    {
        wlh_protocol_v1_Empty empty = wlh_protocol_v1_Empty_init_zero;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            0,
            WLH_SERVICE_DEVICE_INFO,
            WLH_DEVICE_INFO_METHOD_GET_INFO,
            30,
            wlh_protocol_v1_Empty_fields,
            &empty
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.request_id == 30 &&
            rpc.status_domain == WLH_STATUS_DOMAIN_PROTOCOL &&
            rpc.status_code == WLH_STATUS_NOT_SUPPORTED
        );
    }

    /* IO/ADC/KV without a backend fall through to protocol NOT_SUPPORTED. */
    sent_before = f.sent_count;
    {
        wlh_protocol_v1_IoConfigureRequest configure =
            wlh_protocol_v1_IoConfigureRequest_init_zero;
        configure.mode = wlh_protocol_v1_IoMode_IO_MODE_OUTPUT;
        configure.pull = wlh_protocol_v1_IoPull_IO_PULL_NONE;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            1,
            WLH_SERVICE_IO,
            WLH_IO_METHOD_CONFIGURE,
            31,
            wlh_protocol_v1_IoConfigureRequest_fields,
            &configure
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.request_id == 31 &&
            rpc.status_domain == WLH_STATUS_DOMAIN_PROTOCOL &&
            rpc.status_code == WLH_STATUS_NOT_SUPPORTED
        );
    }

    sent_before = f.sent_count;
    {
        wlh_protocol_v1_WifiStartApRequest start =
            wlh_protocol_v1_WifiStartApRequest_init_zero;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            2,
            WLH_SERVICE_WIFI,
            WLH_WIFI_METHOD_START_AP,
            32,
            wlh_protocol_v1_WifiStartApRequest_fields,
            &start
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.request_id == 32 &&
            rpc.status_domain == WLH_STATUS_DOMAIN_WIFI &&
            rpc.status_code == WLH_STATUS_INTERNAL
        );
    }
    CHECK(wlh_coproc_stop(&core) == WLH_COPROC_OK);
}

static void test_softap(void) {
    fixture_t f;
    wlh_coproc_t core;
    uint8_t incoming[4096];
    size_t incoming_size;
    wlh_rpc_envelope_t rpc;
    const uint8_t *rpc_payload;
    size_t rpc_payload_size;
    pb_istream_t stream;
    unsigned sent_before;

    memset(&f, 0, sizeof(f));
    f.core = &core;
    wlh_posix_osal_init(&f.posix);
    prepare_ready_core(&f, &core, true);

    /* START_AP dispatches to the backend op and is acked with OK. */
    sent_before = f.sent_count;
    {
        static const uint8_t ssid[] = "wlh-test-ap";
        static const uint8_t credential[] = "test-password";
        wlh_protocol_v1_WifiStartApRequest start =
            wlh_protocol_v1_WifiStartApRequest_init_zero;
        start.ssid.size = sizeof(ssid) - 1u;
        memcpy(start.ssid.bytes, ssid, sizeof(ssid) - 1u);
        start.credential.size = sizeof(credential) - 1u;
        memcpy(start.credential.bytes, credential, sizeof(credential) - 1u);
        start.security = wlh_protocol_v1_WifiSecurity_WIFI_SECURITY_WPA2_PSK;
        start.channel = 6;
        start.max_clients = 4;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            0,
            WLH_SERVICE_WIFI,
            WLH_WIFI_METHOD_START_AP,
            40,
            wlh_protocol_v1_WifiStartApRequest_fields,
            &start
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.start_ap_calls == 1u);
        CHECK(
            f.last_ap_request.ssid_size == sizeof(ssid) - 1u &&
            memcmp(f.last_ap_request.ssid, ssid, sizeof(ssid) - 1u) == 0
        );
        CHECK(
            f.last_ap_request.credential_size == sizeof(credential) - 1u &&
            memcmp(
                f.last_ap_request.credential,
                credential,
                sizeof(credential) - 1u
            ) == 0
        );
        CHECK(
            f.last_ap_request.security ==
                (uint32_t)wlh_protocol_v1_WifiSecurity_WIFI_SECURITY_WPA2_PSK &&
            f.last_ap_request.channel == 6u &&
            f.last_ap_request.max_clients == 4u
        );
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.request_id == 40 && rpc.kind == WLH_RPC_KIND_RESPONSE &&
            rpc.status_domain == WLH_STATUS_DOMAIN_NONE &&
            rpc.status_code == WLH_STATUS_OK
        );
    }

    /* STOP_AP dispatches to the backend op and is acked with OK. */
    sent_before = f.sent_count;
    {
        wlh_protocol_v1_Empty empty = wlh_protocol_v1_Empty_init_zero;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            1,
            WLH_SERVICE_WIFI,
            WLH_WIFI_METHOD_STOP_AP,
            41,
            wlh_protocol_v1_Empty_fields,
            &empty
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.stop_ap_calls == 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.request_id == 41 && rpc.kind == WLH_RPC_KIND_RESPONSE &&
            rpc.status_domain == WLH_STATUS_DOMAIN_NONE &&
            rpc.status_code == WLH_STATUS_OK
        );
    }

    /* Scan result ingress emits an encoded WIFI event. */
    sent_before = f.sent_count;
    {
        static const uint8_t ssid[] = "wlh-scan";
        static const uint8_t bssid[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
        wlh_coproc_bss_t bss;
        wlh_protocol_v1_WifiScanResultEvent event =
            wlh_protocol_v1_WifiScanResultEvent_init_zero;

        memset(&bss, 0, sizeof(bss));
        bss.ssid = ssid;
        bss.ssid_size = sizeof(ssid) - 1u;
        memcpy(bss.bssid, bssid, sizeof(bssid));
        bss.security =
            (uint32_t)wlh_protocol_v1_WifiSecurity_WIFI_SECURITY_WPA2_PSK;
        bss.channel = 6u;
        bss.rssi_dbm = -42;

        CHECK(wlh_coproc_wifi_scan_result(&core, 11u, &bss) == WLH_COPROC_OK);
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.service_id == WLH_SERVICE_WIFI &&
            rpc.method_id == WLH_WIFI_EVENT_SCAN_RESULT &&
            rpc.kind == WLH_RPC_KIND_EVENT
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(
            &stream, wlh_protocol_v1_WifiScanResultEvent_fields, &event
        ));
        CHECK(
            event.scan_id == 11u && event.networks_count == 1u &&
            event.networks[0].ssid.size == sizeof(ssid) - 1u &&
            memcmp(event.networks[0].ssid.bytes, ssid, sizeof(ssid) - 1u) ==
                0 &&
            event.networks[0].bssid.size == sizeof(bssid) &&
            memcmp(event.networks[0].bssid.bytes, bssid, sizeof(bssid)) == 0 &&
            event.networks[0].security ==
                wlh_protocol_v1_WifiSecurity_WIFI_SECURITY_WPA2_PSK &&
            event.networks[0].channel == 6u && event.networks[0].rssi_dbm == -42
        );
    }

    /* Connected ingress includes the STA interface MAC for Host networking. */
    sent_before = f.sent_count;
    {
        static const uint8_t ssid[] = "wlh-link";
        static const uint8_t bssid[6] = {0x02, 0, 0, 0, 0, 2};
        static const uint8_t interface_mac[6] = {
            0x24, 0x6f, 0x28, 0xaa, 0xbb, 0xcc
        };
        wlh_coproc_bss_t bss;
        wlh_protocol_v1_WifiConnectedEvent event =
            wlh_protocol_v1_WifiConnectedEvent_init_zero;

        memset(&bss, 0, sizeof(bss));
        bss.ssid = ssid;
        bss.ssid_size = sizeof(ssid) - 1u;
        memcpy(bss.bssid, bssid, sizeof(bssid));
        memcpy(bss.interface_mac, interface_mac, sizeof(bss.interface_mac));
        bss.security =
            (uint32_t)wlh_protocol_v1_WifiSecurity_WIFI_SECURITY_WPA2_PSK;
        bss.channel = 6u;
        bss.rssi_dbm = -40;

        CHECK(wlh_coproc_wifi_connected(&core, &bss) == WLH_COPROC_OK);
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.service_id == WLH_SERVICE_WIFI &&
            rpc.method_id == WLH_WIFI_EVENT_CONNECTED &&
            rpc.kind == WLH_RPC_KIND_EVENT
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(
            &stream, wlh_protocol_v1_WifiConnectedEvent_fields, &event
        ));
        CHECK(
            event.has_link && event.link.mac.size == sizeof(interface_mac) &&
            memcmp(
                event.link.mac.bytes, interface_mac, sizeof(interface_mac)
            ) == 0
        );
    }

    /* AP client joined ingress emits a WIFI event. */
    sent_before = f.sent_count;
    {
        static const uint8_t mac[6] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x01};
        wlh_protocol_v1_WifiApClientJoinedEvent joined =
            wlh_protocol_v1_WifiApClientJoinedEvent_init_zero;
        CHECK(
            wlh_coproc_wifi_ap_client_joined(&core, mac, -55, 7) ==
            WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.service_id == WLH_SERVICE_WIFI &&
            rpc.method_id == WLH_WIFI_EVENT_AP_CLIENT_JOINED &&
            rpc.kind == WLH_RPC_KIND_EVENT
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(
            &stream, wlh_protocol_v1_WifiApClientJoinedEvent_fields, &joined
        ));
        CHECK(
            joined.client.mac.size == 6u &&
            memcmp(joined.client.mac.bytes, mac, 6u) == 0 &&
            joined.client.rssi_dbm == -55 && joined.client.association_id == 7u
        );
    }

    /* AP client left ingress emits a WIFI event. */
    sent_before = f.sent_count;
    {
        static const uint8_t mac[6] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x01};
        wlh_protocol_v1_WifiApClientLeftEvent left =
            wlh_protocol_v1_WifiApClientLeftEvent_init_zero;
        CHECK(
            wlh_coproc_wifi_ap_client_left(&core, mac, 7, 3) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.service_id == WLH_SERVICE_WIFI &&
            rpc.method_id == WLH_WIFI_EVENT_AP_CLIENT_LEFT &&
            rpc.kind == WLH_RPC_KIND_EVENT
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(
            &stream, wlh_protocol_v1_WifiApClientLeftEvent_fields, &left
        ));
        CHECK(
            left.mac.size == 6u && memcmp(left.mac.bytes, mac, 6u) == 0 &&
            left.association_id == 7u && left.ieee80211_reason == 3u
        );
    }
    CHECK(wlh_coproc_stop(&core) == WLH_COPROC_OK);
}

/* An oversized ssid_size from an adapter must be rejected, not copied into the
   fixed-size nanopb ssid field. Every event path that copies a BSS ssid is
   covered here. */
static void test_oversized_ssid_rejected(void) {
    static uint8_t ssid[200];
    wlh_coproc_t core;
    wlh_coproc_bss_t bss;
    failing_allocator_t allocator;

    memset(ssid, 'A', sizeof(ssid));
    memset(&core, 0, sizeof(core));
    memset(&bss, 0, sizeof(bss));
    memset(&allocator, 0, sizeof(allocator));
    core.config.buffers = (wlh_coproc_buffer_ops_t){&allocator,
                                                    failing_buffer_alloc,
                                                    failing_buffer_free};
    bss.ssid = ssid;
    bss.ssid_size = sizeof(ssid);

    CHECK(
        wlh_coproc_wifi_scan_result(&core, 1u, &bss) ==
        WLH_COPROC_INVALID_ARGUMENT
    );
    CHECK(
        wlh_coproc_wifi_connected(&core, &bss) == WLH_COPROC_INVALID_ARGUMENT
    );
    CHECK(
        wlh_coproc_wifi_ap_started(&core, &bss) == WLH_COPROC_INVALID_ARGUMENT
    );
    /* Rejected before any allocation. */
    CHECK(allocator.attempts == 0u && allocator.outstanding == 0u);

    /* A non-NULL ssid is required whenever ssid_size is non-zero. */
    bss.ssid = NULL;
    bss.ssid_size = 8u;
    CHECK(
        wlh_coproc_wifi_scan_result(&core, 1u, &bss) ==
        WLH_COPROC_INVALID_ARGUMENT
    );
    CHECK(
        wlh_coproc_wifi_connected(&core, &bss) == WLH_COPROC_INVALID_ARGUMENT
    );
    CHECK(
        wlh_coproc_wifi_ap_started(&core, &bss) == WLH_COPROC_INVALID_ARGUMENT
    );

    /* The exact schema bound is still accepted: it reaches the allocator. */
    bss.ssid = ssid;
    bss.ssid_size = 32u;
    allocator.fail_at = 1u;
    CHECK(
        wlh_coproc_wifi_scan_result(&core, 1u, &bss) == WLH_COPROC_BACKEND_ERROR
    );
    CHECK(allocator.attempts == 1u && allocator.outstanding == 0u);
}

static void test_large_message_allocation_failures(void) {
    static const uint8_t ssid[] = "allocation-test";
    wlh_coproc_t core;
    wlh_coproc_bss_t bss;
    failing_allocator_t allocator;

    memset(&core, 0, sizeof(core));
    memset(&bss, 0, sizeof(bss));
    memset(&allocator, 0, sizeof(allocator));
    core.config.buffers = (wlh_coproc_buffer_ops_t){&allocator,
                                                    failing_buffer_alloc,
                                                    failing_buffer_free};
    bss.ssid = ssid;
    bss.ssid_size = sizeof(ssid) - 1u;

    allocator.fail_at = 1u;
    CHECK(
        wlh_coproc_wifi_scan_result(&core, 1u, &bss) == WLH_COPROC_BACKEND_ERROR
    );
    CHECK(allocator.outstanding == 0u);

    allocator.attempts = 0u;
    allocator.fail_at = 2u;
    CHECK(
        wlh_coproc_wifi_scan_result(&core, 1u, &bss) == WLH_COPROC_BACKEND_ERROR
    );
    CHECK(allocator.outstanding == 0u);
}

static void bt_send_request(
    wlh_coproc_t *core,
    uint16_t method,
    uint32_t request_id,
    const pb_msgdesc_t *fields,
    const void *message
) {
    uint8_t incoming[4096];
    size_t incoming_size = make_rpc_frame(
        incoming,
        42,
        0,
        WLH_SERVICE_BLUETOOTH,
        method,
        request_id,
        fields,
        message
    );
    CHECK(wlh_coproc_on_frame(core, incoming, incoming_size) == WLH_COPROC_OK);
}

static void bt_expect_status(
    fixture_t *f,
    wlh_coproc_t *core,
    uint16_t method,
    uint32_t request_id,
    const pb_msgdesc_t *fields,
    const void *message,
    int16_t status_code
) {
    wlh_rpc_envelope_t rpc;
    const uint8_t *rpc_payload;
    size_t rpc_payload_size;
    unsigned sent_before = f->sent_count;

    bt_send_request(core, method, request_id, fields, message);
    wait_for_sent(f, sent_before + 1u);
    decode_last_sent(f, &rpc, &rpc_payload, &rpc_payload_size);
    CHECK(rpc.service_id == WLH_SERVICE_BLUETOOTH && rpc.method_id == method);
    CHECK(rpc.request_id == request_id && rpc.kind == WLH_RPC_KIND_RESPONSE);
    CHECK(rpc.status_code == status_code);
    CHECK(
        rpc.status_domain == (status_code == WLH_STATUS_OK
                                  ? WLH_STATUS_DOMAIN_NONE
                                  : WLH_STATUS_DOMAIN_BLUETOOTH)
    );
}

static void hci_host_frame(
    wlh_coproc_t *core,
    uint32_t session,
    const uint8_t *records,
    size_t records_size
) {
    uint8_t incoming[4096];
    wlh_frame_header_t header;
    size_t size = 0;

    wlh_frame_header_init(&header, WLH_CHANNEL_BLUETOOTH_HCI);
    header.session_id = session;
    CHECK(
        wlh_frame_encode(
            incoming, sizeof(incoming), &size, &header, records, records_size
        ) == WLH_WIRE_OK
    );
    CHECK(wlh_coproc_on_frame(core, incoming, size) == WLH_COPROC_OK);
}

static void check_sent_hci_record(
    fixture_t *f, uint8_t h4_type, const uint8_t *expected, size_t expected_size
) {
    wlh_frame_header_t header;
    const uint8_t *payload;
    size_t payload_size;

    CHECK(
        wlh_frame_decode(
            &header, &payload, &payload_size, f->sent, f->sent_size, 4096
        ) == WLH_WIRE_OK
    );
    CHECK(header.channel == WLH_CHANNEL_BLUETOOTH_HCI);
    CHECK(payload_size == expected_size + 8u);
    if (payload_size != expected_size + 8u)
        return;
    CHECK(payload[0] == h4_type && payload[1] == 0u);
    CHECK(payload[2] == 8u && payload[3] == 0u);
    CHECK(
        ((uint32_t)payload[4] | ((uint32_t)payload[5] << 8) |
         ((uint32_t)payload[6] << 16) | ((uint32_t)payload[7] << 24)) ==
        (uint32_t)expected_size
    );
    CHECK(memcmp(payload + 8u, expected, expected_size) == 0);
}

static void test_bluetooth_lifecycle_and_info(void) {
    fixture_t f;
    wlh_coproc_t core;
    wlh_rpc_envelope_t rpc;
    const uint8_t *rpc_payload;
    size_t rpc_payload_size;
    pb_istream_t stream;
    unsigned sent_before;
    wlh_protocol_v1_Empty empty = wlh_protocol_v1_Empty_init_zero;
    wlh_protocol_v1_BluetoothInitializeRequest init =
        wlh_protocol_v1_BluetoothInitializeRequest_init_zero;
    wlh_protocol_v1_BluetoothEnableRequest enable =
        wlh_protocol_v1_BluetoothEnableRequest_init_zero;
    wlh_protocol_v1_BluetoothDeinitializeRequest deinit =
        wlh_protocol_v1_BluetoothDeinitializeRequest_init_zero;

    memset(&f, 0, sizeof(f));
    f.core = &core;
    wlh_posix_osal_init(&f.posix);
    prepare_ready_bt_core(&f, &core);

    /* Hello advertises the Bluetooth service, HCI channel and credits. */
    {
        wlh_protocol_v1_HelloResponse response =
            wlh_protocol_v1_HelloResponse_init_zero;
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(
            pb_decode(&stream, wlh_protocol_v1_HelloResponse_fields, &response)
        );
        CHECK(response.initial_credits_count == 5u);
        CHECK(
            response.initial_credits[4].channel_id ==
                WLH_CHANNEL_BLUETOOTH_HCI &&
            response.initial_credits[4].units ==
                WLH_COPROC_BLUETOOTH_INITIAL_CREDIT &&
            response.initial_credits[4].unit_bytes == 1u
        );
        CHECK(
            response.services_count == 1u &&
            response.services[0].service_id == WLH_SERVICE_BLUETOOTH &&
            response.services[0].major == 1u
        );
        CHECK(
            response.channels_count == 1u &&
            response.channels[0].channel_id == WLH_CHANNEL_BLUETOOTH_HCI &&
            response.channels[0].max_frame_payload == WLH_COPROC_MAX_HCI_PACKET
        );
    }

    /* ENABLE before INITIALIZE is NOT_READY. */
    bt_expect_status(
        &f,
        &core,
        WLH_BLUETOOTH_METHOD_ENABLE,
        200,
        wlh_protocol_v1_BluetoothEnableRequest_fields,
        &enable,
        WLH_STATUS_NOT_READY
    );

    /* Non-HCI transports are rejected. */
    bt_expect_status(
        &f,
        &core,
        WLH_BLUETOOTH_METHOD_INITIALIZE,
        201,
        wlh_protocol_v1_BluetoothInitializeRequest_fields,
        &init,
        WLH_STATUS_NOT_SUPPORTED
    );

    /* INITIALIZE runs asynchronously on the backend, without a response
       until the completion arrives. */
    init.transport = wlh_protocol_v1_BluetoothTransport_BLUETOOTH_TRANSPORT_HCI;
    init.feature_flags = 5u;
    sent_before = f.sent_count;
    bt_send_request(
        &core,
        WLH_BLUETOOTH_METHOD_INITIALIZE,
        202,
        wlh_protocol_v1_BluetoothInitializeRequest_fields,
        &init
    );
    wait_for_counter(&f.bt_initializes, 1u);
    CHECK(f.bt_last_feature_flags == 5u);
    CHECK(f.sent_count == sent_before);

    /* Overlapping requests are refused while an operation is pending. */
    bt_expect_status(
        &f,
        &core,
        WLH_BLUETOOTH_METHOD_INITIALIZE,
        203,
        wlh_protocol_v1_BluetoothInitializeRequest_fields,
        &init,
        WLH_STATUS_BUSY
    );

    /* Mismatched or wrong-kind completions are ignored and counted. */
    {
        wlh_coproc_bluetooth_info_t info;
        memset(&info, 0, sizeof(info));
        CHECK(
            wlh_coproc_bluetooth_operation_complete(
                &core, f.bt_last_operation_id + 99u, 0
            ) == WLH_COPROC_OK
        );
        wait_for_bt_mismatches(&core, 1u);
        CHECK(
            wlh_coproc_bluetooth_info_result(
                &core, f.bt_last_operation_id, 0, &info
            ) == WLH_COPROC_OK
        );
        wait_for_bt_mismatches(&core, 2u);
    }

    /* The matching completion resolves request 202 with OK. */
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_bluetooth_operation_complete(
            &core, f.bt_last_operation_id, 0
        ) == WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);
    decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
    CHECK(
        rpc.request_id == 202 && rpc.kind == WLH_RPC_KIND_RESPONSE &&
        rpc.status_code == WLH_STATUS_OK
    );

    /* Re-INITIALIZE after success is an idempotent OK. */
    bt_expect_status(
        &f,
        &core,
        WLH_BLUETOOTH_METHOD_INITIALIZE,
        204,
        wlh_protocol_v1_BluetoothInitializeRequest_fields,
        &init,
        WLH_STATUS_OK
    );
    CHECK(f.bt_initializes == 1u);

    /* ENABLE completes asynchronously and is idempotent afterwards. */
    enable.mode_flags = 3u;
    bt_send_request(
        &core,
        WLH_BLUETOOTH_METHOD_ENABLE,
        205,
        wlh_protocol_v1_BluetoothEnableRequest_fields,
        &enable
    );
    wait_for_counter(&f.bt_enables, 1u);
    CHECK(f.bt_last_mode_flags == 3u);
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_bluetooth_operation_complete(
            &core, f.bt_last_operation_id, 0
        ) == WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);
    decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
    CHECK(rpc.request_id == 205 && rpc.status_code == WLH_STATUS_OK);
    bt_expect_status(
        &f,
        &core,
        WLH_BLUETOOTH_METHOD_ENABLE,
        206,
        wlh_protocol_v1_BluetoothEnableRequest_fields,
        &enable,
        WLH_STATUS_OK
    );
    CHECK(f.bt_enables == 1u);

    /* GET_INFO ignores a completion of the wrong kind... */
    bt_send_request(
        &core,
        WLH_BLUETOOTH_METHOD_GET_INFO,
        207,
        wlh_protocol_v1_Empty_fields,
        &empty
    );
    wait_for_counter(&f.bt_get_infos, 1u);
    CHECK(
        wlh_coproc_bluetooth_operation_complete(
            &core, f.bt_last_operation_id, 0
        ) == WLH_COPROC_OK
    );
    wait_for_bt_mismatches(&core, 3u);

    /* ...then reports the backend info with the core-owned state. */
    {
        wlh_coproc_bluetooth_info_t info;
        wlh_protocol_v1_BluetoothControllerInfo decoded =
            wlh_protocol_v1_BluetoothControllerInfo_init_zero;
        memset(&info, 0, sizeof(info));
        memcpy(info.public_address, "\x11\x22\x33\x44\x55\x66", 6u);
        info.has_public_address = true;
        info.hci_version = 12u;
        info.manufacturer_id = 0x02e5u;
        info.feature_bits = 0x123456789abcdef0ull;
        info.max_hci_packet = 512u;
        sent_before = f.sent_count;
        CHECK(
            wlh_coproc_bluetooth_info_result(
                &core, f.bt_last_operation_id, 0, &info
            ) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(rpc.request_id == 207 && rpc.status_code == WLH_STATUS_OK);
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(
            &stream, wlh_protocol_v1_BluetoothControllerInfo_fields, &decoded
        ));
        CHECK(
            decoded.state ==
            wlh_protocol_v1_BluetoothControllerState_BLUETOOTH_CONTROLLER_STATE_ENABLED
        );
        CHECK(
            decoded.public_address.size == 6u &&
            decoded.public_address.bytes[0] == 0x11u &&
            decoded.public_address.bytes[5] == 0x66u
        );
        CHECK(decoded.hci_version == 12u && decoded.manufacturer_id == 0x02e5u);
        CHECK(
            decoded.feature_bits == 0x123456789abcdef0ull &&
            decoded.max_hci_packet == 512u
        );
    }

    /* DISABLE and DEINITIALIZE walk the state back down. */
    bt_send_request(
        &core,
        WLH_BLUETOOTH_METHOD_DISABLE,
        208,
        wlh_protocol_v1_Empty_fields,
        &empty
    );
    wait_for_counter(&f.bt_disables, 1u);
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_bluetooth_operation_complete(
            &core, f.bt_last_operation_id, 0
        ) == WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);
    decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
    CHECK(rpc.request_id == 208 && rpc.status_code == WLH_STATUS_OK);

    deinit.release_memory = true;
    bt_send_request(
        &core,
        WLH_BLUETOOTH_METHOD_DEINITIALIZE,
        209,
        wlh_protocol_v1_BluetoothDeinitializeRequest_fields,
        &deinit
    );
    wait_for_counter(&f.bt_deinitializes, 1u);
    CHECK(f.bt_last_release_memory);
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_bluetooth_operation_complete(
            &core, f.bt_last_operation_id, 0
        ) == WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);
    decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
    CHECK(rpc.request_id == 209 && rpc.status_code == WLH_STATUS_OK);

    /* Repeating DEINITIALIZE is an idempotent OK. */
    bt_expect_status(
        &f,
        &core,
        WLH_BLUETOOTH_METHOD_DEINITIALIZE,
        210,
        wlh_protocol_v1_BluetoothDeinitializeRequest_fields,
        &deinit,
        WLH_STATUS_OK
    );
    CHECK(f.bt_deinitializes == 1u);

    /* A backend submit failure resolves the request with INTERNAL... */
    f.bt_status = -1;
    bt_expect_status(
        &f,
        &core,
        WLH_BLUETOOTH_METHOD_INITIALIZE,
        211,
        wlh_protocol_v1_BluetoothInitializeRequest_fields,
        &init,
        WLH_STATUS_INTERNAL
    );
    CHECK(f.bt_initializes == 2u);

    /* ...and leaves no pending operation behind. */
    f.bt_status = 0;
    bt_send_request(
        &core,
        WLH_BLUETOOTH_METHOD_INITIALIZE,
        212,
        wlh_protocol_v1_BluetoothInitializeRequest_fields,
        &init
    );
    wait_for_counter(&f.bt_initializes, 3u);
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_bluetooth_operation_complete(
            &core, f.bt_last_operation_id, 0
        ) == WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);
    decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
    CHECK(rpc.request_id == 212 && rpc.status_code == WLH_STATUS_OK);

    /* A failing backend completion also resolves with INTERNAL. */
    bt_send_request(
        &core,
        WLH_BLUETOOTH_METHOD_ENABLE,
        213,
        wlh_protocol_v1_BluetoothEnableRequest_fields,
        &enable
    );
    wait_for_counter(&f.bt_enables, 2u);
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_bluetooth_operation_complete(
            &core, f.bt_last_operation_id, -7
        ) == WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);
    decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
    CHECK(
        rpc.request_id == 213 &&
        rpc.status_domain == WLH_STATUS_DOMAIN_BLUETOOTH &&
        rpc.status_code == WLH_STATUS_INTERNAL
    );

    CHECK(wlh_coproc_stop(&core) == WLH_COPROC_OK);
}

static void test_bluetooth_hci(void) {
    fixture_t f;
    wlh_coproc_t core;
    wlh_rpc_envelope_t rpc;
    const uint8_t *rpc_payload;
    size_t rpc_payload_size;
    pb_istream_t stream;
    unsigned sent_before;
    static const uint8_t command_packet[3] = {0x03, 0x0c, 0x00};
    static const uint8_t event_packet[5] = {0x0e, 0x03, 0x01, 0x03, 0x0c};

    memset(&f, 0, sizeof(f));
    f.core = &core;
    wlh_posix_osal_init(&f.posix);
    prepare_ready_bt_core(&f, &core);

    /* A Host->Controller command reaches the backend and returns the
       credit. */
    {
        static const uint8_t records[11] = {
            1, 0, 8, 0, 3, 0, 0, 0, 0x03, 0x0c, 0x00
        };
        wlh_protocol_v1_CreditUpdate update =
            wlh_protocol_v1_CreditUpdate_init_zero;
        sent_before = f.sent_count;
        hci_host_frame(&core, 42u, records, sizeof(records));
        wait_for_counter(&f.bt_hci_packets, 1u);
        CHECK(f.bt_last_h4_type == WLH_H4_TYPE_COMMAND);
        CHECK(
            f.bt_last_hci_size == 3u &&
            memcmp(f.bt_last_hci, command_packet, 3u) == 0
        );
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.service_id == WLH_SERVICE_LINK &&
            rpc.method_id == WLH_LINK_METHOD_CREDIT_UPDATE &&
            rpc.kind == WLH_RPC_KIND_EVENT
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update));
        CHECK(
            update.channel_id == WLH_CHANNEL_BLUETOOTH_HCI && update.units == 1u
        );
    }

    /* An aggregated frame delivers every record for a single credit. */
    {
        static const uint8_t acl_packet[9] = {
            0x01, 0x00, 0x05, 0x00, 1, 2, 3, 4, 5
        };
        static const uint8_t records[28] = {
            1, 0, 8, 0, 3, 0,    0,    0,    0x03, 0x0c, 0x00, 2, 0, 8,
            0, 9, 0, 0, 0, 0x01, 0x00, 0x05, 0x00, 1,    2,    3, 4, 5
        };
        wlh_protocol_v1_CreditUpdate update =
            wlh_protocol_v1_CreditUpdate_init_zero;
        sent_before = f.sent_count;
        hci_host_frame(&core, 42u, records, sizeof(records));
        wait_for_counter(&f.bt_hci_packets, 3u);
        CHECK(f.bt_last_h4_type == WLH_H4_TYPE_ACL);
        CHECK(
            f.bt_last_hci_size == 9u &&
            memcmp(f.bt_last_hci, acl_packet, 9u) == 0
        );
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(rpc.method_id == WLH_LINK_METHOD_CREDIT_UPDATE);
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update));
        CHECK(
            update.channel_id == WLH_CHANNEL_BLUETOOTH_HCI && update.units == 1u
        );
    }

    /* Controller->Host validation rejects unsupported and malformed
       packets. */
    CHECK(
        wlh_coproc_bluetooth_hci_send(
            &core, WLH_H4_TYPE_SCO, event_packet, sizeof(event_packet)
        ) == WLH_COPROC_NOT_SUPPORTED
    );
    CHECK(
        wlh_coproc_bluetooth_hci_send(
            &core, WLH_H4_TYPE_ISO, event_packet, sizeof(event_packet)
        ) == WLH_COPROC_NOT_SUPPORTED
    );
    CHECK(
        wlh_coproc_bluetooth_hci_send(
            &core, WLH_H4_TYPE_COMMAND, command_packet, sizeof(command_packet)
        ) == WLH_COPROC_INVALID_ARGUMENT
    );
    CHECK(
        wlh_coproc_bluetooth_hci_send(
            &core, WLH_H4_TYPE_EVENT, event_packet, 4u
        ) == WLH_COPROC_INVALID_ARGUMENT
    );

    /* A valid event is framed as an H4 raw record on the HCI channel. */
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_bluetooth_hci_send(
            &core, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
        ) == WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);
    check_sent_hci_record(
        &f, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
    );

    /* Draining the advertised credit yields NO_CREDIT without dropping. */
    {
        unsigned i;
        for (i = 1u; i < WLH_COPROC_BLUETOOTH_INITIAL_CREDIT; ++i) {
            sent_before = f.sent_count;
            CHECK(
                wlh_coproc_bluetooth_hci_send(
                    &core, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
                ) == WLH_COPROC_OK
            );
            wait_for_sent(&f, sent_before + 1u);
        }
        CHECK(
            wlh_coproc_bluetooth_hci_send(
                &core, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
            ) == WLH_COPROC_NO_CREDIT
        );
        CHECK(f.bt_tx_ready_calls == 0u);
    }

    /* Only the zero-to-positive credit edge fires hci_tx_ready. */
    {
        wlh_protocol_v1_CreditUpdate update =
            wlh_protocol_v1_CreditUpdate_init_zero;
        uint8_t incoming[4096];
        size_t incoming_size;
        update.channel_id = WLH_CHANNEL_BLUETOOTH_HCI;
        update.units = 1u;
        sent_before = f.sent_count;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            0,
            WLH_SERVICE_LINK,
            WLH_LINK_METHOD_CREDIT_UPDATE,
            300,
            wlh_protocol_v1_CreditUpdate_fields,
            &update
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.bt_tx_ready_calls == 1u);

        sent_before = f.sent_count;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            0,
            WLH_SERVICE_LINK,
            WLH_LINK_METHOD_CREDIT_UPDATE,
            301,
            wlh_protocol_v1_CreditUpdate_fields,
            &update
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.bt_tx_ready_calls == 1u);

        sent_before = f.sent_count;
        CHECK(
            wlh_coproc_bluetooth_hci_send(
                &core, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
            ) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        check_sent_hci_record(
            &f, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
        );
    }

    /* A rejecting backend withholds the credit and counts the drop. */
    {
        static const uint8_t records[11] = {
            1, 0, 8, 0, 3, 0, 0, 0, 0x03, 0x0c, 0x00
        };
        unsigned packets_before = f.bt_hci_packets;
        f.bt_hci_status = -1;
        sent_before = f.sent_count;
        hci_host_frame(&core, 42u, records, sizeof(records));
        wait_for_hci_drops(&core, 1u);
        CHECK(f.bt_hci_packets == packets_before + 1u);
        CHECK(f.sent_count == sent_before);
        f.bt_hci_status = 0;
    }

    /* A malformed record poisons the frame and latches ERROR. */
    {
        static const uint8_t records[11] = {
            1, 0, 8, 0, 3, 0, 0, 0, 0x03, 0x0c, 0x05
        };
        wlh_protocol_v1_BluetoothStateChangedEvent event =
            wlh_protocol_v1_BluetoothStateChangedEvent_init_zero;
        wlh_coproc_diagnostics_t diagnostics;
        unsigned packets_before = f.bt_hci_packets;
        sent_before = f.sent_count;
        hci_host_frame(&core, 42u, records, sizeof(records));
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.service_id == WLH_SERVICE_BLUETOOTH &&
            rpc.method_id == WLH_BLUETOOTH_EVENT_STATE_CHANGED &&
            rpc.kind == WLH_RPC_KIND_EVENT
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(
            &stream, wlh_protocol_v1_BluetoothStateChangedEvent_fields, &event
        ));
        CHECK(
            event.state ==
                wlh_protocol_v1_BluetoothControllerState_BLUETOOTH_CONTROLLER_STATE_ERROR &&
            event.reason == WLH_COPROC_BLUETOOTH_REASON_MALFORMED_HCI
        );
        CHECK(f.bt_hci_packets == packets_before);
        wlh_coproc_get_diagnostics(&core, &diagnostics);
        CHECK(diagnostics.hci_malformed == 1u);

        /* Session HCI now stops in both directions. */
        {
            static const uint8_t good[11] = {
                1, 0, 8, 0, 3, 0, 0, 0, 0x03, 0x0c, 0x00
            };
            hci_host_frame(&core, 42u, good, sizeof(good));
            wait_for_hci_drops(&core, 2u);
            CHECK(f.bt_hci_packets == packets_before);
        }
        CHECK(
            wlh_coproc_bluetooth_hci_send(
                &core, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
            ) == WLH_COPROC_INVALID_STATE
        );
    }

    /* A new Hello restores HCI service on the next session. */
    {
        wlh_protocol_v1_HelloRequest hello =
            wlh_protocol_v1_HelloRequest_init_zero;
        wlh_protocol_v1_HelloResponse response =
            wlh_protocol_v1_HelloResponse_init_zero;
        uint8_t incoming[4096];
        size_t incoming_size;
        uint32_t new_session;
        unsigned packets_before = f.bt_hci_packets;
        hello.protocol_versions_count = 1;
        hello.protocol_versions[0].major = 1;
        hello.max_frame_size = 4096;
        sent_before = f.sent_count;
        incoming_size = make_rpc_frame(
            incoming,
            0,
            0,
            WLH_SERVICE_LINK,
            WLH_LINK_METHOD_HELLO,
            400,
            wlh_protocol_v1_HelloRequest_fields,
            &hello
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(rpc.request_id == 400 && rpc.kind == WLH_RPC_KIND_RESPONSE);
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(
            pb_decode(&stream, wlh_protocol_v1_HelloResponse_fields, &response)
        );
        new_session = response.session_id;
        CHECK(new_session != 0u && new_session != 42u);

        {
            static const uint8_t records[11] = {
                1, 0, 8, 0, 3, 0, 0, 0, 0x03, 0x0c, 0x00
            };
            sent_before = f.sent_count;
            hci_host_frame(&core, new_session, records, sizeof(records));
            wait_for_counter(&f.bt_hci_packets, packets_before + 1u);
            wait_for_sent(&f, sent_before + 1u);
        }
    }

    /* Backend-reported fatal errors latch ERROR with the given reason. */
    {
        wlh_protocol_v1_BluetoothStateChangedEvent event =
            wlh_protocol_v1_BluetoothStateChangedEvent_init_zero;
        sent_before = f.sent_count;
        CHECK(wlh_coproc_bluetooth_fatal_error(&core, 7u) == WLH_COPROC_OK);
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(rpc.method_id == WLH_BLUETOOTH_EVENT_STATE_CHANGED);
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(
            &stream, wlh_protocol_v1_BluetoothStateChangedEvent_fields, &event
        ));
        CHECK(
            event.state ==
                wlh_protocol_v1_BluetoothControllerState_BLUETOOTH_CONTROLLER_STATE_ERROR &&
            event.reason == 7u
        );
        CHECK(
            wlh_coproc_bluetooth_hci_send(
                &core, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
            ) == WLH_COPROC_INVALID_STATE
        );
    }

    CHECK(wlh_coproc_stop(&core) == WLH_COPROC_OK);
}

int main(void) {
    test_hello_wifi_and_ethernet();
    test_device_info_and_user_passthrough();
    test_io_adc_kv();
    test_optional_services_not_configured();
    test_softap();
    test_oversized_ssid_rejected();
    test_large_message_allocation_failures();
    test_bluetooth_lifecycle_and_info();
    test_bluetooth_hci();
    if (failures != 0)
        return 1;
    puts("coprocessor core tests passed");
    return 0;
}
