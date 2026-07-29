#include "coproc_test_support.h"

void test_io_adc_kv(void) {
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

void test_optional_services_not_configured(void) {
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
