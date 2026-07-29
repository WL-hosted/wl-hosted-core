#include "host_test_support.h"

void test_device_info_and_user_passthrough(void) {
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
