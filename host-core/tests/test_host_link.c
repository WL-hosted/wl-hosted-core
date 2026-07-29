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
    wait_for_tx(&fixture, 5u);
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
