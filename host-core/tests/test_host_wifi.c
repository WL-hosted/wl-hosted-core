#include "host_test_support.h"

void test_wifi_softap(void) {
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
