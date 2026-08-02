#include "coproc_test_support.h"

static void send_eth_request(
    wlh_coproc_t *core, uint16_t method, uint32_t request_id
) {
    uint8_t incoming[4096];
    size_t incoming_size;
    wlh_protocol_v1_EthGetInfoRequest message =
        wlh_protocol_v1_EthGetInfoRequest_init_zero;
    incoming_size = make_rpc_frame(
        incoming,
        42,
        0,
        WLH_SERVICE_ETH,
        method,
        request_id,
        wlh_protocol_v1_EthGetInfoRequest_fields,
        &message
    );
    CHECK(wlh_coproc_on_frame(core, incoming, incoming_size) == WLH_COPROC_OK);
}

static void decode_hello_response(
    fixture_t *f, wlh_protocol_v1_HelloResponse *response
) {
    wlh_rpc_envelope_t rpc;
    const uint8_t *rpc_payload;
    size_t rpc_payload_size;
    pb_istream_t stream;
    decode_last_sent(f, &rpc, &rpc_payload, &rpc_payload_size);
    CHECK(rpc.request_id == 7 && rpc.kind == WLH_RPC_KIND_RESPONSE);
    stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
    CHECK(pb_decode(&stream, wlh_protocol_v1_HelloResponse_fields, response));
}

static bool hello_advertises(
    const wlh_protocol_v1_HelloResponse *response,
    uint16_t service_id,
    uint8_t channel_id
) {
    bool service_found = false;
    bool channel_found = false;
    bool credit_found = false;
    size_t index;
    for (index = 0; index < response->services_count; ++index) {
        if (response->services[index].service_id == service_id)
            service_found = true;
    }
    for (index = 0; index < response->channels_count; ++index) {
        if (response->channels[index].channel_id == channel_id)
            channel_found = true;
    }
    for (index = 0; index < response->initial_credits_count; ++index) {
        if (response->initial_credits[index].channel_id == channel_id)
            credit_found = true;
    }
    return service_found && channel_found && credit_found;
}

void test_coproc_eth_hello_and_get_info(void) {
    fixture_t f;
    wlh_coproc_t core;
    wlh_protocol_v1_HelloResponse hello =
        wlh_protocol_v1_HelloResponse_init_zero;
    wlh_rpc_envelope_t rpc;
    const uint8_t *rpc_payload;
    size_t rpc_payload_size;
    unsigned sent_before;
    unsigned attempt;

    memset(&f, 0, sizeof(f));
    wlh_posix_osal_init(&f.posix);
    prepare_ready_eth_core(&f, &core, true);

    /* The HelloResponse advertises service, channel and initial credit. */
    decode_hello_response(&f, &hello);
    CHECK(hello_advertises(&hello, WLH_SERVICE_ETH, WLH_CHANNEL_ETHERNET_ETH));

    /* GET_INFO is dispatched to the backend and answered on completion. */
    f.eth_info.link_state = WLH_COPROC_ETH_LINK_STATE_UP;
    memcpy(f.eth_info.mac_address, "\x02\x00\x00\x00\x00\x01", 6u);
    f.eth_info.speed = WLH_COPROC_ETH_SPEED_1000M;
    f.eth_info.duplex = WLH_COPROC_ETH_DUPLEX_FULL;
    sent_before = f.sent_count;
    send_eth_request(&core, WLH_ETH_METHOD_GET_INFO, 20u);
    for (attempt = 0; attempt < 1000u && f.eth_get_infos == 0u; ++attempt)
        wait_milliseconds(1u);
    CHECK(f.eth_get_infos == 1u);
    CHECK(f.sent_count == sent_before);
    CHECK(
        wlh_coproc_eth_info_ready(
            &core, f.eth_last_operation_id, 0, &f.eth_info
        ) == WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);
    decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
    CHECK(
        rpc.service_id == WLH_SERVICE_ETH &&
        rpc.method_id == WLH_ETH_METHOD_GET_INFO
    );
    CHECK(rpc.request_id == 20u && rpc.kind == WLH_RPC_KIND_RESPONSE);
    CHECK(rpc.status_code == WLH_STATUS_OK);
    {
        wlh_protocol_v1_EthGetInfoResponse response =
            wlh_protocol_v1_EthGetInfoResponse_init_zero;
        pb_istream_t stream =
            pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(
            &stream, wlh_protocol_v1_EthGetInfoResponse_fields, &response
        ));
        CHECK(response.has_info);
        CHECK(
            response.info.link_state ==
            wlh_protocol_v1_EthLinkState_ETH_LINK_STATE_UP
        );
        CHECK(
            response.info.mac_address.size == 6u &&
            memcmp(
                response.info.mac_address.bytes, "\x02\x00\x00\x00\x00\x01", 6u
            ) == 0
        );
        CHECK(response.info.speed == wlh_protocol_v1_EthSpeed_ETH_SPEED_1000M);
        CHECK(
            response.info.duplex == wlh_protocol_v1_EthDuplex_ETH_DUPLEX_FULL
        );
    }

    /* A second request while one is outstanding is rejected BUSY; the
       outstanding request is then answered normally. */
    send_eth_request(&core, WLH_ETH_METHOD_GET_INFO, 21u);
    for (attempt = 0; attempt < 1000u && f.eth_get_infos == 1u; ++attempt)
        wait_milliseconds(1u);
    CHECK(f.eth_get_infos == 2u);
    sent_before = f.sent_count;
    send_eth_request(&core, WLH_ETH_METHOD_GET_INFO, 22u);
    wait_for_sent(&f, sent_before + 1u);
    decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
    CHECK(rpc.request_id == 22u && rpc.kind == WLH_RPC_KIND_RESPONSE);
    CHECK(
        rpc.status_code == WLH_STATUS_BUSY &&
        rpc.status_domain == WLH_STATUS_DOMAIN_ETH
    );
    CHECK(f.eth_get_infos == 2u);
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_eth_info_ready(
            &core, f.eth_last_operation_id, 0, &f.eth_info
        ) == WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);
    decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
    CHECK(rpc.request_id == 21u && rpc.status_code == WLH_STATUS_OK);

    /* A backend failure surfaces as an ETH-domain error response. */
    send_eth_request(&core, WLH_ETH_METHOD_GET_INFO, 23u);
    for (attempt = 0; attempt < 1000u && f.eth_get_infos == 2u; ++attempt)
        wait_milliseconds(1u);
    CHECK(f.eth_get_infos == 3u);
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_eth_info_ready(&core, f.eth_last_operation_id, -1, NULL) ==
        WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);
    decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
    CHECK(rpc.request_id == 23u && rpc.kind == WLH_RPC_KIND_RESPONSE);
    CHECK(
        rpc.status_code == WLH_STATUS_INTERNAL &&
        rpc.status_domain == WLH_STATUS_DOMAIN_ETH
    );

    /* A submission rejected by the backend fails the request immediately. */
    f.eth_submit_status = -1;
    sent_before = f.sent_count;
    send_eth_request(&core, WLH_ETH_METHOD_GET_INFO, 24u);
    wait_for_sent(&f, sent_before + 1u);
    decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
    CHECK(rpc.request_id == 24u && rpc.kind == WLH_RPC_KIND_RESPONSE);
    CHECK(
        rpc.status_code == WLH_STATUS_INTERNAL &&
        rpc.status_domain == WLH_STATUS_DOMAIN_ETH
    );

    CHECK(wlh_coproc_stop(&core) == WLH_COPROC_OK);
}

void test_coproc_eth_not_configured(void) {
    fixture_t f;
    wlh_coproc_t core;
    wlh_protocol_v1_HelloResponse hello =
        wlh_protocol_v1_HelloResponse_init_zero;
    wlh_rpc_envelope_t rpc;
    const uint8_t *rpc_payload;
    size_t rpc_payload_size;
    unsigned sent_before;

    memset(&f, 0, sizeof(f));
    wlh_posix_osal_init(&f.posix);
    prepare_ready_eth_core(&f, &core, false);

    decode_hello_response(&f, &hello);
    CHECK(!hello_advertises(&hello, WLH_SERVICE_ETH, WLH_CHANNEL_ETHERNET_ETH));

    sent_before = f.sent_count;
    send_eth_request(&core, WLH_ETH_METHOD_GET_INFO, 30u);
    wait_for_sent(&f, sent_before + 1u);
    decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
    CHECK(rpc.request_id == 30u && rpc.kind == WLH_RPC_KIND_RESPONSE);
    CHECK(
        rpc.status_code == WLH_STATUS_NOT_SUPPORTED &&
        rpc.status_domain == WLH_STATUS_DOMAIN_PROTOCOL
    );

    CHECK(wlh_coproc_stop(&core) == WLH_COPROC_OK);
}

void test_coproc_eth_data_channel(void) {
    fixture_t f;
    wlh_coproc_t core;
    uint8_t incoming[4096];
    uint8_t raw[11] = {1, 0, 8, 0, 3, 0, 0, 0, 1, 2, 3};
    uint8_t payload[3] = {7, 8, 9};
    wlh_frame_header_t header;
    wlh_frame_header_t frame_header;
    wlh_rpc_envelope_t rpc;
    wlh_protocol_v1_CreditUpdate update =
        wlh_protocol_v1_CreditUpdate_init_zero;
    const uint8_t *frame_payload;
    const uint8_t *rpc_payload;
    size_t frame_payload_size;
    size_t rpc_payload_size;
    size_t size = 0;
    pb_istream_t stream;
    unsigned sent_before;
    unsigned attempt;

    memset(&f, 0, sizeof(f));
    wlh_posix_osal_init(&f.posix);
    prepare_ready_eth_core(&f, &core, true);

    /* An inbound record on the ETH channel reaches the ETH receive callback
       and returns the credit on the same channel. */
    sent_before = f.sent_count;
    wlh_frame_header_init(&header, WLH_CHANNEL_ETHERNET_ETH);
    header.session_id = 42u;
    CHECK(
        wlh_frame_encode(
            incoming, sizeof(incoming), &size, &header, raw, sizeof(raw)
        ) == WLH_WIRE_OK
    );
    CHECK(wlh_coproc_on_frame(&core, incoming, size) == WLH_COPROC_OK);
    for (attempt = 0; attempt < 1000u && f.eth_rx_calls == 0u; ++attempt)
        wait_milliseconds(1u);
    CHECK(f.eth_rx_calls == 1u);
    CHECK(f.eth_rx_size == 3u);
    CHECK(f.eth_rx_session_id == 42u);
    CHECK(f.eth_rx_channel == WLH_CHANNEL_ETHERNET_ETH);
    wait_for_sent(&f, sent_before + 1u);
    CHECK(
        wlh_frame_decode(
            &frame_header,
            &frame_payload,
            &frame_payload_size,
            f.sent,
            f.sent_size,
            4096
        ) == WLH_WIRE_OK
    );
    CHECK(frame_header.channel == WLH_CHANNEL_LINK_CONTROL);
    CHECK(
        wlh_rpc_decode(
            &rpc,
            &rpc_payload,
            &rpc_payload_size,
            frame_payload,
            frame_payload_size,
            1536
        ) == WLH_WIRE_OK
    );
    CHECK(
        rpc.service_id == WLH_SERVICE_LINK &&
        rpc.method_id == WLH_LINK_METHOD_CREDIT_UPDATE &&
        rpc.kind == WLH_RPC_KIND_EVENT
    );
    stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
    CHECK(pb_decode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update));
    CHECK(update.channel_id == WLH_CHANNEL_ETHERNET_ETH && update.units == 1u);

    /* The outbound path builds the same raw record on the ETH channel. */
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_ethernet_eth_send(&core, payload, sizeof(payload)) ==
        WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);
    CHECK(
        wlh_frame_decode(
            &frame_header,
            &frame_payload,
            &frame_payload_size,
            f.sent,
            f.sent_size,
            4096
        ) == WLH_WIRE_OK
    );
    CHECK(frame_header.channel == WLH_CHANNEL_ETHERNET_ETH);
    check_raw_record(
        frame_payload, frame_payload_size, payload, sizeof(payload)
    );

    /* LINK_STATE_CHANGED rides the CONTROL_RPC channel as an ETH event. */
    CHECK(
        wlh_coproc_eth_link_state_changed(
            &core, 0u, WLH_COPROC_ETH_SPEED_1000M, WLH_COPROC_ETH_DUPLEX_FULL
        ) == WLH_COPROC_INVALID_ARGUMENT
    );
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_eth_link_state_changed(
            &core,
            WLH_COPROC_ETH_LINK_STATE_UP,
            WLH_COPROC_ETH_SPEED_1000M,
            WLH_COPROC_ETH_DUPLEX_FULL
        ) == WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);
    decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
    CHECK(
        rpc.service_id == WLH_SERVICE_ETH &&
        rpc.method_id == WLH_ETH_EVENT_LINK_STATE_CHANGED &&
        rpc.kind == WLH_RPC_KIND_EVENT
    );
    {
        wlh_protocol_v1_EthLinkStateChangedEvent event =
            wlh_protocol_v1_EthLinkStateChangedEvent_init_zero;
        pb_istream_t event_stream =
            pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(
            &event_stream,
            wlh_protocol_v1_EthLinkStateChangedEvent_fields,
            &event
        ));
        CHECK(
            event.link_state == wlh_protocol_v1_EthLinkState_ETH_LINK_STATE_UP
        );
        CHECK(event.speed == wlh_protocol_v1_EthSpeed_ETH_SPEED_1000M);
        CHECK(event.duplex == wlh_protocol_v1_EthDuplex_ETH_DUPLEX_FULL);
    }

    CHECK(wlh_coproc_stop(&core) == WLH_COPROC_OK);
}
