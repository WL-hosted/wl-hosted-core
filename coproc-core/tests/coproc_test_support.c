#include "coproc_test_support.h"

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
#include "ota.pb.h"
#include "pb_decode.h"
#include "pb_encode.h"
#include "user_passthrough.pb.h"
#include "wifi.pb.h"
#include "wlh/posix_osal.h"
#include "wlh/protocol/raw_record.h"

int failures;
#define CHECK(x)                                                               \
    do {                                                                       \
        if (!(x)) {                                                            \
            fprintf(stderr, "%s:%d: %s\n", __FILE__, __LINE__, #x);            \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

int submit_frame(
    void *context,
    uint8_t *frame,
    size_t size,
    wlh_coproc_tx_complete_fn completion,
    void *completion_context
) {
    fixture_t *f = context;
    size_t slot = f->sent_count % 16u;
    memcpy(f->sent, frame, size);
    f->sent_size = size;
    memcpy(f->sent_log[slot], frame, size);
    f->sent_log_size[slot] = size;
    ++f->sent_count;
    completion(completion_context, frame, size, 0);
    return 0;
}
uint8_t *buffer_alloc(void *context, size_t size) {
    uint8_t *buffer = malloc(size);
    (void)context;
    /* Poison the allocation so any byte the core forgets to initialize shows
       up in the emitted frame instead of silently reading back as zero. */
    if (buffer != NULL)
        memset(buffer, 0xa5, size);
    return buffer;
}
void buffer_free(void *context, uint8_t *buffer) {
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

    CHECK(buffer != NULL);
    CHECK(allocator->outstanding != 0u);
    if (allocator->outstanding != 0u)
        --allocator->outstanding;
    free(buffer);
}
/* Validate an emitted Ethernet payload exactly the way the peer does, so an
   uninitialized raw-header byte fails here instead of on hardware. */
void check_raw_record(
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

wlh_coproc_ethernet_rx_result_t ethernet_rx(
    void *context,
    uint32_t session_id,
    uint8_t channel,
    const uint8_t *frame,
    size_t size
) {
    (void)frame;
    fixture_t *fixture = context;
    fixture->ethernet_size = size;
    fixture->ethernet_rx_session_id = session_id;
    fixture->ethernet_rx_channel = channel;
    ++fixture->ethernet_rx_calls;
    return fixture->ethernet_rx_result;
}
wlh_coproc_ethernet_rx_result_t ethernet_ap_rx(
    void *context,
    uint32_t session_id,
    uint8_t channel,
    const uint8_t *frame,
    size_t size
) {
    (void)frame;
    (void)session_id;
    (void)channel;
    ((fixture_t *)context)->ethernet_ap_size = size;
    return WLH_COPROC_ETHERNET_RX_COMPLETE;
}
int wifi_init(void *context, uint32_t operation_id, uint32_t interface_flags) {
    fixture_t *fixture = context;
    ++fixture->initialized;
    (void)interface_flags;
    (void)wlh_coproc_wifi_initialized(fixture->core, operation_id, 0);
    return 0;
}
int get_device_info(void *context, wlh_coproc_device_info_t *info) {
    fixture_t *fixture = context;
    ++fixture->device_info_queries;
    *info = fixture->device_info;
    return fixture->device_info_status;
}
int on_user_message(void *context, const wlh_coproc_user_message_t *message) {
    fixture_t *fixture = context;
    ++fixture->user_messages;
    fixture->last_user_message = *message;
    memcpy(fixture->last_user_payload, message->payload, message->payload_size);
    fixture->last_user_message.payload = fixture->last_user_payload;
    return 0;
}
int wifi_start_ap(void *context, const wlh_coproc_wifi_ap_t *request) {
    fixture_t *fixture = context;
    ++fixture->start_ap_calls;
    fixture->last_ap_request = *request;
    return 0;
}
int wifi_stop_ap(void *context) {
    fixture_t *fixture = context;
    ++fixture->stop_ap_calls;
    return 0;
}

int io_configure(void *context, const wlh_coproc_io_config_t *config) {
    fixture_t *fixture = context;
    ++fixture->io_configures;
    fixture->last_io_config = *config;
    return fixture->io_status;
}
int io_read(void *context, uint32_t pin_id, wlh_coproc_io_state_t *state) {
    fixture_t *fixture = context;
    ++fixture->io_reads;
    fixture->last_io_pin = pin_id;
    *state = fixture->io_state;
    return fixture->io_status;
}
int io_write(void *context, uint32_t pin_id, bool level) {
    fixture_t *fixture = context;
    ++fixture->io_writes;
    fixture->last_io_pin = pin_id;
    fixture->last_io_level = level;
    return fixture->io_status;
}
int adc_read(void *context, uint32_t pin_id, uint32_t *millivolts) {
    fixture_t *fixture = context;
    ++fixture->adc_reads;
    fixture->last_adc_pin = pin_id;
    *millivolts = fixture->adc_millivolts;
    return fixture->adc_status;
}
int kv_read(
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
int kv_write(
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
int kv_erase(void *context, const char *key) {
    fixture_t *fixture = context;
    ++fixture->kv_erases;
    snprintf(fixture->last_kv_key, sizeof(fixture->last_kv_key), "%s", key);
    return fixture->kv_status;
}

int bt_initialize(
    void *context, uint32_t operation_id, uint32_t feature_flags
) {
    fixture_t *fixture = context;
    ++fixture->bt_initializes;
    fixture->bt_last_operation_id = operation_id;
    fixture->bt_last_feature_flags = feature_flags;
    return fixture->bt_status;
}
int bt_enable(void *context, uint32_t operation_id, uint32_t mode_flags) {
    fixture_t *fixture = context;
    ++fixture->bt_enables;
    fixture->bt_last_operation_id = operation_id;
    fixture->bt_last_mode_flags = mode_flags;
    return fixture->bt_status;
}
int bt_disable(void *context, uint32_t operation_id) {
    fixture_t *fixture = context;
    ++fixture->bt_disables;
    fixture->bt_last_operation_id = operation_id;
    return fixture->bt_status;
}
int bt_deinitialize(void *context, uint32_t operation_id, bool release_memory) {
    fixture_t *fixture = context;
    ++fixture->bt_deinitializes;
    fixture->bt_last_operation_id = operation_id;
    fixture->bt_last_release_memory = release_memory;
    return fixture->bt_status;
}
int bt_get_info(void *context, uint32_t operation_id) {
    fixture_t *fixture = context;
    ++fixture->bt_get_infos;
    fixture->bt_last_operation_id = operation_id;
    return fixture->bt_status;
}
int bt_hci_send(
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
void bt_hci_tx_ready(void *context) {
    ++((fixture_t *)context)->bt_tx_ready_calls;
}

void wait_milliseconds(uint32_t milliseconds) {
    struct timespec value = {
        (time_t)(milliseconds / 1000u), (long)(milliseconds % 1000u) * 1000000L
    };
    (void)nanosleep(&value, NULL);
}

void wait_for_state(wlh_coproc_t *core, wlh_coproc_state_t state) {
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

void wait_for_sent(fixture_t *fixture, unsigned count) {
    unsigned attempt;
    for (attempt = 0; attempt < 1000u && fixture->sent_count < count; ++attempt)
        wait_milliseconds(1u);
    CHECK(fixture->sent_count >= count);
}

size_t make_rpc_frame(
    uint8_t *output,
    uint32_t session,
    uint32_t sequence,
    uint16_t service,
    uint16_t method,
    uint32_t request_id,
    const pb_msgdesc_t *fields,
    const void *message
) {
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

void prepare_ready_core(
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

void wait_for_counter(const unsigned *counter, unsigned value) {
    unsigned attempt;
    for (attempt = 0; attempt < 1000u && *counter < value; ++attempt)
        wait_milliseconds(1u);
    CHECK(*counter >= value);
}

void wait_for_bt_mismatches(wlh_coproc_t *core, uint32_t value) {
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

void wait_for_hci_drops(wlh_coproc_t *core, uint32_t value) {
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

void prepare_ready_bt_core(fixture_t *f, wlh_coproc_t *core, bool declare_adv) {
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
    if (declare_adv) {
        hello.channels_count = 2u;
        hello.channels[0].channel_id = WLH_CHANNEL_BLUETOOTH_HCI;
        hello.channels[0].max_frame_payload = 4096u;
        hello.channels[1].channel_id = WLH_CHANNEL_BLUETOOTH_HCI_ADV;
        hello.channels[1].max_frame_payload = 4096u;
    }
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

size_t decode_last_sent(
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

/* An oversized ssid_size from an adapter must be rejected, not copied into the
   fixed-size nanopb ssid field. Every event path that copies a BSS ssid is
   covered here. */

void bt_send_request(
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

void bt_expect_status(
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

void hci_host_frame(
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

void check_sent_hci_record_on(
    fixture_t *f,
    uint8_t channel,
    uint8_t h4_type,
    const uint8_t *expected,
    size_t expected_size
) {
    wlh_frame_header_t header;
    const uint8_t *payload;
    size_t payload_size;

    CHECK(
        wlh_frame_decode(
            &header, &payload, &payload_size, f->sent, f->sent_size, 4096
        ) == WLH_WIRE_OK
    );
    CHECK(header.channel == channel);
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

void check_sent_hci_record(
    fixture_t *f, uint8_t h4_type, const uint8_t *expected, size_t expected_size
) {
    check_sent_hci_record_on(
        f, WLH_CHANNEL_BLUETOOTH_HCI, h4_type, expected, expected_size
    );
}

int ota_begin(
    void *context,
    uint32_t operation_id,
    uint32_t transfer_id,
    const wlh_coproc_ota_begin_params_t *params
) {
    fixture_t *fixture = context;
    ++fixture->ota_begins;
    fixture->ota_last_operation_id = operation_id;
    fixture->ota_last_transfer_id = transfer_id;
    fixture->ota_last_params = *params;
    fixture->ota_written_total = 0u;
    if (fixture->ota_submit_status == 0 && fixture->ota_auto_complete) {
        (void)wlh_coproc_ota_begin_complete(
            fixture->core, operation_id, fixture->ota_backend_status
        );
    }
    return fixture->ota_submit_status;
}

int ota_write(
    void *context,
    uint32_t transfer_id,
    uint64_t offset,
    const uint8_t *data,
    size_t size
) {
    fixture_t *fixture = context;
    ++fixture->ota_writes;
    fixture->ota_last_transfer_id = transfer_id;
    fixture->ota_last_offset = offset;
    fixture->ota_last_data_size = size;
    if (size <= sizeof(fixture->ota_last_data))
        memcpy(fixture->ota_last_data, data, size);
    if (fixture->ota_write_status == 0)
        fixture->ota_written_total += size;
    return fixture->ota_write_status;
}

int ota_finalize(
    void *context,
    uint32_t operation_id,
    uint32_t transfer_id,
    uint64_t bytes_sent
) {
    fixture_t *fixture = context;
    ++fixture->ota_finalizes;
    fixture->ota_last_operation_id = operation_id;
    fixture->ota_last_transfer_id = transfer_id;
    fixture->ota_last_bytes_sent = bytes_sent;
    if (fixture->ota_submit_status == 0 && fixture->ota_auto_complete) {
        (void)wlh_coproc_ota_finalize_complete(
            fixture->core, operation_id, fixture->ota_backend_status
        );
    }
    return fixture->ota_submit_status;
}

int ota_abort(void *context, uint32_t operation_id, uint32_t transfer_id) {
    fixture_t *fixture = context;
    ++fixture->ota_aborts;
    fixture->ota_last_operation_id = operation_id;
    fixture->ota_last_transfer_id = transfer_id;
    if (fixture->ota_submit_status == 0 && fixture->ota_auto_complete) {
        (void)wlh_coproc_ota_abort_complete(
            fixture->core, operation_id, fixture->ota_backend_status
        );
    }
    return fixture->ota_submit_status;
}

int ota_activate(
    void *context, uint32_t operation_id, uint32_t transfer_id, bool reboot
) {
    fixture_t *fixture = context;
    ++fixture->ota_activates;
    fixture->ota_last_operation_id = operation_id;
    fixture->ota_last_transfer_id = transfer_id;
    fixture->ota_last_reboot = reboot;
    if (fixture->ota_submit_status == 0 && fixture->ota_auto_complete) {
        (void)wlh_coproc_ota_activate_complete(
            fixture->core, operation_id, fixture->ota_backend_status
        );
    }
    return fixture->ota_submit_status;
}

void prepare_ready_ota_core(fixture_t *f, wlh_coproc_t *core) {
    wlh_coproc_config_t config;
    uint8_t incoming[4096];
    size_t incoming_size;
    wlh_protocol_v1_HelloRequest hello = wlh_protocol_v1_HelloRequest_init_zero;

    f->core = core;
    f->ota_auto_complete = true;
    memset(&config, 0, sizeof(config));
    config.port.context = f;
    config.port.submit_tx = submit_frame;
    config.port.ethernet_rx = ethernet_rx;
    config.buffers = (wlh_coproc_buffer_ops_t){f, buffer_alloc, buffer_free};
    config.osal = wlh_posix_osal_ops(&f->posix);
    config.ota.context = f;
    config.ota.begin = ota_begin;
    config.ota.write = ota_write;
    config.ota.finalize = ota_finalize;
    config.ota.abort = ota_abort;
    config.ota.activate = ota_activate;
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

void ota_send_request(
    wlh_coproc_t *core,
    uint16_t method,
    uint32_t request_id,
    const pb_msgdesc_t *fields,
    const void *message
) {
    uint8_t incoming[4096];
    size_t incoming_size = make_rpc_frame(
        incoming, 42, 0, WLH_SERVICE_OTA, method, request_id, fields, message
    );
    CHECK(wlh_coproc_on_frame(core, incoming, incoming_size) == WLH_COPROC_OK);
}

void ota_expect_status(
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

    ota_send_request(core, method, request_id, fields, message);
    wait_for_sent(f, sent_before + 1u);
    decode_last_sent(f, &rpc, &rpc_payload, &rpc_payload_size);
    CHECK(rpc.service_id == WLH_SERVICE_OTA && rpc.method_id == method);
    CHECK(rpc.request_id == request_id && rpc.kind == WLH_RPC_KIND_RESPONSE);
    CHECK(rpc.status_code == status_code);
    CHECK(
        rpc.status_domain == (status_code == WLH_STATUS_OK
                                  ? WLH_STATUS_DOMAIN_NONE
                                  : WLH_STATUS_DOMAIN_OTA)
    );
}

size_t make_ota_stream_frame(
    uint8_t *output,
    size_t output_capacity,
    uint32_t session,
    uint32_t transfer_id,
    uint64_t offset,
    const uint8_t *data,
    size_t data_size
) {
    uint8_t subheader[WLH_COPROC_OTA_STREAM_HEADER_SIZE];
    uint8_t
        payload[WLH_COPROC_OTA_STREAM_HEADER_SIZE + WLH_COPROC_OTA_CHUNK_SIZE];
    uint8_t record[sizeof(payload) + WLH_RAW_RECORD_HEADER_SIZE];
    size_t record_size = 0;
    size_t frame_size = 0;
    wlh_frame_header_t header;

    CHECK(data_size <= WLH_COPROC_OTA_CHUNK_SIZE);
    subheader[0] = (uint8_t)(transfer_id & 0xffu);
    subheader[1] = (uint8_t)((transfer_id >> 8) & 0xffu);
    subheader[2] = (uint8_t)((transfer_id >> 16) & 0xffu);
    subheader[3] = (uint8_t)((transfer_id >> 24) & 0xffu);
    subheader[4] = (uint8_t)(offset & 0xffu);
    subheader[5] = (uint8_t)((offset >> 8) & 0xffu);
    subheader[6] = (uint8_t)((offset >> 16) & 0xffu);
    subheader[7] = (uint8_t)((offset >> 24) & 0xffu);
    subheader[8] = (uint8_t)((offset >> 32) & 0xffu);
    subheader[9] = (uint8_t)((offset >> 40) & 0xffu);
    subheader[10] = (uint8_t)((offset >> 48) & 0xffu);
    subheader[11] = (uint8_t)((offset >> 56) & 0xffu);
    subheader[12] = (uint8_t)(data_size & 0xffu);
    subheader[13] = (uint8_t)((data_size >> 8) & 0xffu);
    subheader[14] = 0u;
    subheader[15] = 0u;

    memcpy(payload, subheader, sizeof(subheader));
    if (data_size != 0u)
        memcpy(payload + sizeof(subheader), data, data_size);

    CHECK(
        wlh_raw_record_encode(
            record,
            sizeof(record),
            &record_size,
            WLH_COPROC_OTA_RECORD_TYPE,
            0u,
            payload,
            sizeof(subheader) + data_size
        ) == WLH_WIRE_OK
    );

    wlh_frame_header_init(&header, WLH_CHANNEL_OTA_STREAM);
    header.session_id = session;
    CHECK(
        wlh_frame_encode(
            output, output_capacity, &frame_size, &header, record, record_size
        ) == WLH_WIRE_OK
    );
    return frame_size;
}

void ota_stream_send(
    wlh_coproc_t *core,
    uint32_t session,
    uint32_t transfer_id,
    uint64_t offset,
    const uint8_t *data,
    size_t data_size
) {
    uint8_t incoming[4096];
    size_t size = make_ota_stream_frame(
        incoming,
        sizeof(incoming),
        session,
        transfer_id,
        offset,
        data,
        data_size
    );
    CHECK(wlh_coproc_on_frame(core, incoming, size) == WLH_COPROC_OK);
}

bool find_sent_rpc(
    fixture_t *f,
    uint16_t service_id,
    uint16_t method_id,
    wlh_rpc_kind_t kind,
    wlh_rpc_envelope_t *rpc,
    const uint8_t **rpc_payload,
    size_t *rpc_payload_size
) {
    unsigned available = f->sent_count < 16u ? f->sent_count : 16u;
    unsigned i;
    for (i = 0; i < available; ++i) {
        size_t slot = (size_t)((f->sent_count - 1u - i) % 16u);
        wlh_frame_header_t frame_header;
        const uint8_t *frame_payload;
        size_t frame_payload_size;
        if (wlh_frame_decode(
                &frame_header,
                &frame_payload,
                &frame_payload_size,
                f->sent_log[slot],
                f->sent_log_size[slot],
                4096
            ) != WLH_WIRE_OK) {
            continue;
        }
        if (wlh_rpc_decode(
                rpc,
                rpc_payload,
                rpc_payload_size,
                frame_payload,
                frame_payload_size,
                1536
            ) != WLH_WIRE_OK) {
            continue;
        }
        if (rpc->service_id == service_id && rpc->method_id == method_id &&
            rpc->kind == kind) {
            return true;
        }
    }
    return false;
}
