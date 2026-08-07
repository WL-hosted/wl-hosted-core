#include "host_test_support.h"

void test_handshake_and_rpc(void) {
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

void test_timeout_credit_and_session(void) {
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
    /* The two adjacent Ethernet jobs may be emitted as one aggregate, so the
       recovery Hello is the fourth transport frame rather than the fifth. */
    wait_for_tx(&fixture, 4u);
    request_id = captured_request_id(&fixture, &service, &method);
    assert(
        request_id != 0u && service == WLH_SERVICE_LINK &&
        method == WLH_LINK_METHOD_HELLO
    );
    assert(wlh_host_stop(&fixture.host) == WLH_HOST_OK);
}

void test_asynchronous_transport_start(void) {
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

void test_control_rpc_credit_refund(void) {
    fixture_t fixture;
    uint8_t frame[4096];
    size_t frame_size;
    unsigned i;
    fixture_init(&fixture);
    establish_ready(&fixture);

    /* The peer charges one credit per RPC frame on CONTROL_RPC but nothing
       used to return them, so the peer's session allowance drained until it
       went CONGESTED and dropped the next one-shot RPC (the OTA progress
       burst used to exhaust it and swallow the activate request). The host
       must refund every CONTROL_RPC frame it receives. */
    for (i = 0u; i < 5u; ++i) {
        static const uint8_t progress[2] = {0x08u, 0x00u};
        unsigned tx_before = fixture.tx_count;
        frame_size = make_rpc_frame(
            frame,
            42u,
            (uint32_t)i,
            WLH_SERVICE_OTA,
            WLH_OTA_EVENT_PROGRESS,
            0u,
            WLH_RPC_KIND_EVENT,
            0,
            progress,
            sizeof(progress)
        );
        assert(
            wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK
        );
        wait_for_tx(&fixture, tx_before + 1u);
        {
            wlh_frame_header_t header;
            const uint8_t *frame_payload;
            size_t frame_payload_size;
            wlh_rpc_envelope_t envelope;
            const uint8_t *payload;
            size_t payload_size;
            wlh_protocol_v1_CreditUpdate update =
                wlh_protocol_v1_CreditUpdate_init_zero;
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
            assert(header.channel == WLH_CHANNEL_LINK_CONTROL);
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
            assert(
                envelope.service_id == WLH_SERVICE_LINK &&
                envelope.method_id == WLH_LINK_METHOD_CREDIT_UPDATE
            );
            stream = pb_istream_from_buffer(payload, payload_size);
            assert(
                pb_decode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update)
            );
            assert(
                update.channel_id == WLH_CHANNEL_CONTROL_RPC &&
                update.units == 1u
            );
        }
    }
    assert(wlh_host_stop(&fixture.host) == WLH_HOST_OK);
}
