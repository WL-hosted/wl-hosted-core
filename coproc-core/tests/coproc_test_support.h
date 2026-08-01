#ifndef WLH_COPROC_TEST_SUPPORT_H
#define WLH_COPROC_TEST_SUPPORT_H

#include "wlh/coproc.h"
#include "wlh/posix_osal.h"

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
#include "diagnostics.pb.h"
#include "io.pb.h"
#include "kv.pb.h"
#include "link.pb.h"
#include "user_passthrough.pb.h"
#include "wifi.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>

extern int failures;
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
    unsigned ethernet_rx_calls;
    uint32_t ethernet_rx_session_id;
    uint8_t ethernet_rx_channel;
    wlh_coproc_ethernet_rx_result_t ethernet_rx_result;
    unsigned sent_count;
    uint8_t sent_log[16][4096];
    size_t sent_log_size[16];
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
    unsigned ota_begins;
    unsigned ota_writes;
    unsigned ota_finalizes;
    unsigned ota_aborts;
    unsigned ota_activates;
    uint32_t ota_last_operation_id;
    uint32_t ota_last_transfer_id;
    uint64_t ota_last_offset;
    uint64_t ota_last_bytes_sent;
    bool ota_last_reboot;
    wlh_coproc_ota_begin_params_t ota_last_params;
    uint8_t ota_last_data[WLH_COPROC_OTA_CHUNK_SIZE];
    size_t ota_last_data_size;
    uint64_t ota_written_total;
    int ota_submit_status;
    int ota_write_status;
    int ota_backend_status;
    bool ota_auto_complete;
    wlh_posix_osal_t posix;
} fixture_t;

typedef struct failing_allocator {
    size_t attempts;
    size_t fail_at;
    size_t outstanding;
} failing_allocator_t;

int submit_frame(
    void *context,
    uint8_t *frame,
    size_t size,
    wlh_coproc_tx_complete_fn completion,
    void *completion_context
);
uint8_t *buffer_alloc(void *context, size_t size);
void buffer_free(void *context, uint8_t *buffer);
uint8_t *failing_buffer_alloc(void *context, size_t size);
void failing_buffer_free(void *context, uint8_t *buffer);
void check_raw_record(
    const uint8_t *payload,
    size_t payload_size,
    const uint8_t *expected,
    size_t expected_size
);
wlh_coproc_ethernet_rx_result_t ethernet_rx(
    void *context,
    uint32_t session_id,
    uint8_t channel,
    const uint8_t *frame,
    size_t size
);
wlh_coproc_ethernet_rx_result_t ethernet_ap_rx(
    void *context,
    uint32_t session_id,
    uint8_t channel,
    const uint8_t *frame,
    size_t size
);
int wifi_init(void *context, uint32_t operation_id, uint32_t interface_flags);
int get_device_info(void *context, wlh_coproc_device_info_t *info);
int on_user_message(void *context, const wlh_coproc_user_message_t *message);
int wifi_start_ap(void *context, const wlh_coproc_wifi_ap_t *request);
int wifi_stop_ap(void *context);
int io_configure(void *context, const wlh_coproc_io_config_t *config);
int io_read(void *context, uint32_t pin_id, wlh_coproc_io_state_t *state);
int io_write(void *context, uint32_t pin_id, bool level);
int adc_read(void *context, uint32_t pin_id, uint32_t *millivolts);
int kv_read(
    void *context,
    const char *key,
    char *value,
    size_t value_capacity,
    size_t *value_size
);
int kv_write(
    void *context, const char *key, const char *value, size_t value_size
);
int kv_erase(void *context, const char *key);
int bt_initialize(void *context, uint32_t operation_id, uint32_t feature_flags);
int bt_enable(void *context, uint32_t operation_id, uint32_t mode_flags);
int bt_disable(void *context, uint32_t operation_id);
int bt_deinitialize(void *context, uint32_t operation_id, bool release_memory);
int bt_get_info(void *context, uint32_t operation_id);
int bt_hci_send(
    void *context, uint8_t h4_type, const uint8_t *payload, size_t payload_size
);
void bt_hci_tx_ready(void *context);
void wait_milliseconds(uint32_t milliseconds);
void wait_for_state(wlh_coproc_t *core, wlh_coproc_state_t state);
void wait_for_sent(fixture_t *fixture, unsigned count);
size_t make_rpc_frame(
    uint8_t *output,
    uint32_t session,
    uint32_t sequence,
    uint16_t service,
    uint16_t method,
    uint32_t request_id,
    const pb_msgdesc_t *fields,
    const void *message
);
void prepare_ready_core(
    fixture_t *fixture, wlh_coproc_t *core, bool with_optional_services
);
void wait_for_counter(const unsigned *counter, unsigned value);
void wait_for_bt_mismatches(wlh_coproc_t *core, uint32_t value);
void wait_for_hci_drops(wlh_coproc_t *core, uint32_t value);
void prepare_ready_bt_core(
    fixture_t *fixture, wlh_coproc_t *core, bool declare_adv
);
size_t decode_last_sent(
    fixture_t *fixture,
    wlh_rpc_envelope_t *rpc,
    const uint8_t **rpc_payload,
    size_t *rpc_payload_size
);
void bt_send_request(
    wlh_coproc_t *core,
    uint16_t method,
    uint32_t request_id,
    const pb_msgdesc_t *fields,
    const void *message
);
void bt_expect_status(
    fixture_t *fixture,
    wlh_coproc_t *core,
    uint16_t method,
    uint32_t request_id,
    const pb_msgdesc_t *fields,
    const void *message,
    int16_t status_code
);
void hci_host_frame(
    wlh_coproc_t *core,
    uint32_t session,
    const uint8_t *records,
    size_t records_size
);
void check_sent_hci_record_on(
    fixture_t *fixture,
    uint8_t channel,
    uint8_t h4_type,
    const uint8_t *expected,
    size_t expected_size
);
void check_sent_hci_record(
    fixture_t *fixture,
    uint8_t h4_type,
    const uint8_t *expected,
    size_t expected_size
);

int ota_begin(
    void *context,
    uint32_t operation_id,
    uint32_t transfer_id,
    const wlh_coproc_ota_begin_params_t *params
);
int ota_write(
    void *context,
    uint32_t transfer_id,
    uint64_t offset,
    const uint8_t *data,
    size_t size
);
int ota_finalize(
    void *context,
    uint32_t operation_id,
    uint32_t transfer_id,
    uint64_t bytes_sent
);
int ota_abort(void *context, uint32_t operation_id, uint32_t transfer_id);
int ota_activate(
    void *context, uint32_t operation_id, uint32_t transfer_id, bool reboot
);
void prepare_ready_ota_core(fixture_t *fixture, wlh_coproc_t *core);
void ota_send_request(
    wlh_coproc_t *core,
    uint16_t method,
    uint32_t request_id,
    const pb_msgdesc_t *fields,
    const void *message
);
void ota_expect_status(
    fixture_t *fixture,
    wlh_coproc_t *core,
    uint16_t method,
    uint32_t request_id,
    const pb_msgdesc_t *fields,
    const void *message,
    int16_t status_code
);
size_t make_ota_stream_frame(
    uint8_t *output,
    size_t output_capacity,
    uint32_t session,
    uint32_t transfer_id,
    uint64_t offset,
    const uint8_t *data,
    size_t data_size
);
void ota_stream_send(
    wlh_coproc_t *core,
    uint32_t session,
    uint32_t transfer_id,
    uint64_t offset,
    const uint8_t *data,
    size_t data_size
);
bool find_sent_rpc(
    fixture_t *fixture,
    uint16_t service_id,
    uint16_t method_id,
    wlh_rpc_kind_t kind,
    wlh_rpc_envelope_t *rpc,
    const uint8_t **rpc_payload,
    size_t *rpc_payload_size
);

#endif
