#ifndef WLH_HOST_TEST_SUPPORT_H
#define WLH_HOST_TEST_SUPPORT_H

#include "wlh/host.h"

#include <assert.h>
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
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

typedef struct failing_allocator {
    size_t attempts;
    size_t fail_at;
    size_t outstanding;
} failing_allocator_t;

uint8_t *failing_buffer_alloc(void *context, size_t size);
void failing_buffer_free(void *context, uint8_t *buffer);
void on_completion(
    void *context,
    wlh_host_result_t result,
    uint16_t domain,
    int16_t status,
    const uint8_t *payload,
    size_t payload_size
);
void on_device_info(
    void *context,
    wlh_host_result_t result,
    uint16_t domain,
    int16_t status,
    const wlh_host_device_info_t *info
);
void on_io_read(
    void *context,
    wlh_host_result_t result,
    uint16_t domain,
    int16_t status,
    const wlh_host_io_state_t *state
);
void on_adc_read(
    void *context,
    wlh_host_result_t result,
    uint16_t domain,
    int16_t status,
    const wlh_host_adc_sample_t *sample
);
void on_kv_read(
    void *context,
    wlh_host_result_t result,
    uint16_t domain,
    int16_t status,
    const char *value,
    size_t value_size
);
void on_bluetooth_info(
    void *context,
    wlh_host_result_t result,
    uint16_t domain,
    int16_t status,
    const wlh_bluetooth_controller_info_t *info
);
void fixture_init(fixture_t *fixture);
void wait_milliseconds(uint32_t milliseconds);
void wait_for_state(fixture_t *fixture, wlh_host_state_t state);
void wait_for_tx(fixture_t *fixture, unsigned count);
void wait_for_completion(fixture_t *fixture, unsigned count);
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
);
void establish_ready(fixture_t *fixture);
void send_bluetooth_hello(fixture_t *fixture, uint32_t session_id);
void establish_ready_bluetooth(fixture_t *fixture);
size_t make_hci_channel_frame(
    uint8_t *output,
    uint8_t channel,
    uint32_t session_id,
    uint32_t sequence,
    uint8_t record_type,
    const uint8_t *payload,
    size_t payload_size
);
size_t make_hci_frame(
    uint8_t *output,
    uint32_t session_id,
    uint32_t sequence,
    uint8_t record_type,
    const uint8_t *payload,
    size_t payload_size
);
void decode_tx_hci(
    const fixture_t *fixture,
    uint8_t *record_type,
    uint8_t *payload_out,
    size_t *payload_size
);
uint32_t captured_request_id(
    const fixture_t *fixture, uint16_t *service, uint16_t *method
);
void decode_tx_message(
    const fixture_t *fixture, const pb_msgdesc_t *fields, void *message
);

#endif
