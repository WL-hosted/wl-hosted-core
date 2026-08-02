#include "host_test_support.h"

void test_eth_get_info_and_link_event(void) {
    fixture_t fixture;
    uint8_t frame[4096];
    uint8_t payload[512];
    uint16_t service, method;
    uint32_t request_id;
    size_t frame_size;
    unsigned tx_before;
    unsigned attempt;
    uint32_t sequence = 0u;
    fixture_init(&fixture);
    establish_ready_eth(&fixture);

    /* GET_INFO encodes an empty request and decodes the interface info. */
    tx_before = fixture.tx_count;
    assert(
        wlh_host_eth_get_info(&fixture.host, on_eth_info, &fixture) ==
        WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    request_id = captured_request_id(&fixture, &service, &method);
    assert(service == WLH_SERVICE_ETH && method == WLH_ETH_METHOD_GET_INFO);
    {
        wlh_protocol_v1_EthGetInfoResponse response =
            wlh_protocol_v1_EthGetInfoResponse_init_zero;
        pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
        response.has_info = true;
        response.info.link_state =
            wlh_protocol_v1_EthLinkState_ETH_LINK_STATE_UP;
        response.info.mac_address.size = 6u;
        memcpy(response.info.mac_address.bytes, "\xaa\xbb\xcc\xdd\xee\xff", 6u);
        response.info.speed = wlh_protocol_v1_EthSpeed_ETH_SPEED_1000M;
        response.info.duplex = wlh_protocol_v1_EthDuplex_ETH_DUPLEX_FULL;
        assert(pb_encode(
            &stream, wlh_protocol_v1_EthGetInfoResponse_fields, &response
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
    for (attempt = 0; attempt < 1000u && fixture.eth_info_callbacks == 0u;
         ++attempt)
        wait_milliseconds(1u);
    assert(fixture.eth_info_callbacks == 1u);
    assert(fixture.eth_info_result == WLH_HOST_OK);
    assert(fixture.eth_info.link_state == WLH_HOST_ETH_LINK_STATE_UP);
    assert(
        memcmp(fixture.eth_info.mac_address, "\xaa\xbb\xcc\xdd\xee\xff", 6u) ==
        0
    );
    assert(fixture.eth_info.speed == WLH_HOST_ETH_SPEED_1000M);
    assert(fixture.eth_info.duplex == WLH_HOST_ETH_DUPLEX_FULL);

    /* A truncated MAC address is a protocol error, not a guess. */
    tx_before = fixture.tx_count;
    assert(
        wlh_host_eth_get_info(&fixture.host, on_eth_info, &fixture) ==
        WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    request_id = captured_request_id(&fixture, &service, &method);
    {
        wlh_protocol_v1_EthGetInfoResponse response =
            wlh_protocol_v1_EthGetInfoResponse_init_zero;
        pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
        response.has_info = true;
        response.info.link_state =
            wlh_protocol_v1_EthLinkState_ETH_LINK_STATE_UP;
        response.info.mac_address.size = 3u;
        assert(pb_encode(
            &stream, wlh_protocol_v1_EthGetInfoResponse_fields, &response
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
    for (attempt = 0; attempt < 1000u && fixture.eth_info_callbacks == 1u;
         ++attempt)
        wait_milliseconds(1u);
    assert(fixture.eth_info_callbacks == 2u);
    assert(fixture.eth_info_result == WLH_HOST_PROTOCOL_ERROR);

    /* LINK_STATE_CHANGED is normalized into the host event struct. */
    {
        unsigned events_before = fixture.events;
        wlh_protocol_v1_EthLinkStateChangedEvent event =
            wlh_protocol_v1_EthLinkStateChangedEvent_init_zero;
        pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
        const wlh_host_eth_link_state_event_t *decoded;
        event.link_state = wlh_protocol_v1_EthLinkState_ETH_LINK_STATE_DOWN;
        event.speed = wlh_protocol_v1_EthSpeed_ETH_SPEED_UNSPECIFIED;
        event.duplex = wlh_protocol_v1_EthDuplex_ETH_DUPLEX_UNSPECIFIED;
        assert(pb_encode(
            &stream, wlh_protocol_v1_EthLinkStateChangedEvent_fields, &event
        ));
        frame_size = make_rpc_frame(
            frame,
            42u,
            sequence++,
            WLH_SERVICE_ETH,
            WLH_ETH_EVENT_LINK_STATE_CHANGED,
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
        assert(
            fixture.last_event_kind == WLH_HOST_EVENT_ETH_LINK_STATE_CHANGED
        );
        assert(
            fixture.last_event_payload_size ==
            sizeof(wlh_host_eth_link_state_event_t)
        );
        decoded =
            (const wlh_host_eth_link_state_event_t *)fixture.last_event_payload;
        assert(
            decoded->link_state == WLH_HOST_ETH_LINK_STATE_DOWN &&
            decoded->speed == WLH_HOST_ETH_SPEED_UNSPECIFIED &&
            decoded->duplex == WLH_HOST_ETH_DUPLEX_UNSPECIFIED
        );
    }
    assert(wlh_host_stop(&fixture.host) == WLH_HOST_OK);
}

void test_eth_data_channel(void) {
    fixture_t fixture;
    uint8_t frame[4096];
    uint8_t raw[11] = {1u, 0u, 8u, 0u, 3u, 0u, 0u, 0u, 1u, 2u, 3u};
    uint8_t ethernet[60] = {0};
    wlh_frame_header_t header;
    wlh_rpc_envelope_t rpc;
    wlh_protocol_v1_CreditUpdate update =
        wlh_protocol_v1_CreditUpdate_init_zero;
    const uint8_t *payload;
    const uint8_t *rpc_payload;
    size_t frame_size = 0u;
    size_t payload_size = 0u;
    size_t rpc_payload_size = 0u;
    pb_istream_t stream;
    unsigned events_before;
    unsigned tx_before;

    fixture_init(&fixture);
    establish_ready_eth(&fixture);

    /* An inbound record on the ETH channel dispatches the ETH RX event and
       returns the credit on the same channel. */
    events_before = fixture.events;
    tx_before = fixture.tx_count;
    wlh_frame_header_init(&header, WLH_CHANNEL_ETHERNET_ETH);
    header.session_id = 42u;
    assert(
        wlh_frame_encode(
            frame, sizeof(frame), &frame_size, &header, raw, sizeof(raw)
        ) == WLH_WIRE_OK
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    while (fixture.events == events_before)
        wait_milliseconds(1u);
    fixture.now += 1u;
    wait_for_tx(&fixture, tx_before + 1u);
    assert(fixture.last_event_kind == WLH_HOST_EVENT_ETHERNET_ETH_RX);
    assert(
        fixture.last_event_payload_size == 3u &&
        memcmp(fixture.last_event_payload, raw + 8u, 3u) == 0
    );
    assert(
        wlh_frame_decode(
            &header,
            &payload,
            &payload_size,
            fixture.tx,
            fixture.tx_size,
            sizeof(fixture.tx)
        ) == WLH_WIRE_OK
    );
    assert(header.channel == WLH_CHANNEL_LINK_CONTROL);
    assert(
        wlh_rpc_decode(
            &rpc,
            &rpc_payload,
            &rpc_payload_size,
            payload,
            payload_size,
            sizeof(fixture.tx)
        ) == WLH_WIRE_OK
    );
    assert(
        rpc.service_id == WLH_SERVICE_LINK &&
        rpc.method_id == WLH_LINK_METHOD_CREDIT_UPDATE &&
        rpc.kind == WLH_RPC_KIND_EVENT
    );
    stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
    assert(pb_decode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update));
    assert(update.channel_id == WLH_CHANNEL_ETHERNET_ETH && update.units == 1u);

    /* Two outbound frames fit the negotiated window; the third is refused
       without touching the wire. */
    tx_before = fixture.tx_count;
    assert(
        wlh_host_ethernet_eth_send(&fixture.host, ethernet, sizeof(ethernet)) ==
        WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    assert(
        wlh_frame_decode(
            &header,
            &payload,
            &payload_size,
            fixture.tx,
            fixture.tx_size,
            sizeof(fixture.tx)
        ) == WLH_WIRE_OK
    );
    assert(header.channel == WLH_CHANNEL_ETHERNET_ETH);
    assert(payload_size == sizeof(ethernet) + 8u);

    tx_before = fixture.tx_count;
    assert(
        wlh_host_ethernet_eth_send(&fixture.host, ethernet, sizeof(ethernet)) ==
        WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    assert(
        wlh_host_ethernet_eth_send(&fixture.host, ethernet, sizeof(ethernet)) ==
        WLH_HOST_NO_CREDIT
    );
    wait_milliseconds(20u);
    assert(fixture.tx_count == tx_before + 1u);
    assert(wlh_host_stop(&fixture.host) == WLH_HOST_OK);
}
