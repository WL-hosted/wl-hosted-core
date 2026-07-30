#include "host_test_support.h"

void test_ap_ethernet(void) {
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
    establish_ready(&fixture);
    events_before = fixture.events;
    wlh_frame_header_init(&header, WLH_CHANNEL_ETHERNET_AP);
    header.session_id = 42u;
    assert(
        wlh_frame_encode(
            frame, sizeof(frame), &frame_size, &header, raw, sizeof(raw)
        ) == WLH_WIRE_OK
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    while (fixture.events == events_before)
        wait_milliseconds(1u);
    assert(fixture.last_event_kind == WLH_HOST_EVENT_ETHERNET_AP_RX);
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
    assert(update.channel_id == WLH_CHANNEL_ETHERNET_AP && update.units == 1u);

    /*
     * Dropping an event because the application executor is full must not
     * permanently consume the peer's transport credit.
     */
    tx_before = fixture.tx_count;
    events_before = fixture.events;
    fixture.reject_executor = true;
    wlh_frame_header_init(&header, WLH_CHANNEL_ETHERNET_AP);
    header.session_id = 42u;
    header.sequence = 1u;
    assert(
        wlh_frame_encode(
            frame, sizeof(frame), &frame_size, &header, raw, sizeof(raw)
        ) == WLH_WIRE_OK
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    wait_for_tx(&fixture, tx_before + 1u);
    assert(fixture.events == events_before);
    fixture.reject_executor = false;
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
        rpc.method_id == WLH_LINK_METHOD_CREDIT_UPDATE
    );

    tx_before = fixture.tx_count;
    assert(
        wlh_host_ethernet_ap_send(&fixture.host, ethernet, sizeof(ethernet)) ==
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
    assert(header.channel == WLH_CHANNEL_ETHERNET_AP);
    assert(payload_size == sizeof(ethernet) + 8u);
    assert(wlh_host_stop(&fixture.host) == WLH_HOST_OK);
}

/*
 * The peer charges one credit unit per raw record, and the coprocessor
 * aggregates several Ethernet frames into one wire frame. Returning a single
 * unit for an aggregated frame leaks records-1 credits every time, which
 * drains the channel monotonically and stalls the data path. Verify the
 * returned unit count tracks the record count rather than the frame count.
 */
void test_ethernet_aggregated_credit_accounting(void) {
    fixture_t fixture;
    uint8_t frame[4096];
    uint8_t payload_buffer[256];
    /* Three raw records in one frame payload; 4-byte body each. */
    static const uint8_t bodies[3][4] = {
        {0xA1u, 0xA2u, 0xA3u, 0xA4u},
        {0xB1u, 0xB2u, 0xB3u, 0xB4u},
        {0xC1u, 0xC2u, 0xC3u, 0xC4u}
    };
    wlh_frame_header_t header;
    wlh_rpc_envelope_t rpc;
    wlh_protocol_v1_CreditUpdate update =
        wlh_protocol_v1_CreditUpdate_init_zero;
    const uint8_t *payload;
    const uint8_t *rpc_payload;
    size_t frame_size = 0u;
    size_t payload_size = 0u;
    size_t rpc_payload_size = 0u;
    size_t aggregate_size = 0u;
    unsigned index;
    unsigned events_before;
    unsigned tx_before;
    pb_istream_t stream;

    fixture_init(&fixture);
    establish_ready(&fixture);

    for (index = 0u; index < 3u; ++index) {
        size_t record_size = 0u;
        assert(
            wlh_raw_record_encode(
                payload_buffer + aggregate_size,
                sizeof(payload_buffer) - aggregate_size,
                &record_size,
                1u,
                0u,
                bodies[index],
                sizeof(bodies[index])
            ) == WLH_WIRE_OK
        );
        aggregate_size += record_size;
    }

    events_before = fixture.events;
    tx_before = fixture.tx_count;
    wlh_frame_header_init(&header, WLH_CHANNEL_ETHERNET_AP);
    header.session_id = 42u;
    assert(
        wlh_frame_encode(
            frame,
            sizeof(frame),
            &frame_size,
            &header,
            payload_buffer,
            aggregate_size
        ) == WLH_WIRE_OK
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);

    /* All three records must be delivered upward. */
    while (fixture.events < events_before + 3u)
        wait_milliseconds(1u);
    assert(fixture.events == events_before + 3u);

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
        rpc.method_id == WLH_LINK_METHOD_CREDIT_UPDATE
    );
    stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
    assert(pb_decode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update));
    assert(update.channel_id == WLH_CHANNEL_ETHERNET_AP);
    assert(update.units == 3u);

    assert(wlh_host_stop(&fixture.host) == WLH_HOST_OK);
}
