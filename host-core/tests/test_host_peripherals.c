#include "host_test_support.h"

void test_io_adc_kv_clients(void) {
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
