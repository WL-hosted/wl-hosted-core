#include "host_test_support.h"

void test_bluetooth_not_negotiated(void) {
    fixture_t fixture;
    static const uint8_t reset_command[] = {0x03u, 0x0cu, 0x00u};
    fixture_init(&fixture);
    establish_ready(&fixture);
    /* The Hello response omitted the Bluetooth service and channel, so every
       Bluetooth entry point reports NOT_SUPPORTED before touching the wire. */
    assert(
        wlh_host_bluetooth_initialize(
            &fixture.host, 0u, on_completion, &fixture
        ) == WLH_HOST_NOT_SUPPORTED
    );
    assert(
        wlh_host_bluetooth_enable(&fixture.host, 0u, on_completion, &fixture) ==
        WLH_HOST_NOT_SUPPORTED
    );
    assert(
        wlh_host_bluetooth_disable(&fixture.host, on_completion, &fixture) ==
        WLH_HOST_NOT_SUPPORTED
    );
    assert(
        wlh_host_bluetooth_deinitialize(
            &fixture.host, false, on_completion, &fixture
        ) == WLH_HOST_NOT_SUPPORTED
    );
    assert(
        wlh_host_bluetooth_get_info(
            &fixture.host, on_bluetooth_info, &fixture
        ) == WLH_HOST_NOT_SUPPORTED
    );
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host,
            WLH_H4_TYPE_COMMAND,
            reset_command,
            sizeof(reset_command)
        ) == WLH_HOST_NOT_SUPPORTED
    );
    assert(fixture.completions == 0u && fixture.bt_info_callbacks == 0u);
    assert(wlh_host_stop(&fixture.host) == WLH_HOST_OK);
}

void test_bluetooth_lifecycle_and_info(void) {
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
    establish_ready_bluetooth(&fixture);

    /* INITIALIZE encodes the HCI transport and completes on the ack. */
    tx_before = fixture.tx_count;
    assert(
        wlh_host_bluetooth_initialize(
            &fixture.host, 5u, on_completion, &fixture
        ) == WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    request_id = captured_request_id(&fixture, &service, &method);
    assert(
        service == WLH_SERVICE_BLUETOOTH &&
        method == WLH_BLUETOOTH_METHOD_INITIALIZE
    );
    {
        wlh_protocol_v1_BluetoothInitializeRequest decoded =
            wlh_protocol_v1_BluetoothInitializeRequest_init_zero;
        decode_tx_message(
            &fixture,
            wlh_protocol_v1_BluetoothInitializeRequest_fields,
            &decoded
        );
        assert(
            decoded.transport ==
                wlh_protocol_v1_BluetoothTransport_BLUETOOTH_TRANSPORT_HCI &&
            decoded.feature_flags == 5u
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

    /* GET_INFO decodes and validates the controller descriptor. */
    tx_before = fixture.tx_count;
    assert(
        wlh_host_bluetooth_get_info(
            &fixture.host, on_bluetooth_info, &fixture
        ) == WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    request_id = captured_request_id(&fixture, &service, &method);
    assert(
        service == WLH_SERVICE_BLUETOOTH &&
        method == WLH_BLUETOOTH_METHOD_GET_INFO
    );
    {
        wlh_protocol_v1_BluetoothControllerInfo info =
            wlh_protocol_v1_BluetoothControllerInfo_init_zero;
        pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
        info.state =
            wlh_protocol_v1_BluetoothControllerState_BLUETOOTH_CONTROLLER_STATE_ENABLED;
        info.public_address.size = 6u;
        memcpy(info.public_address.bytes, "\x11\x22\x33\x44\x55\x66", 6u);
        info.hci_version = 12u;
        info.manufacturer_id = 0x02e5u;
        info.feature_bits = 0x123456789abcdef0ull;
        info.max_hci_packet = 1021u;
        assert(pb_encode(
            &stream, wlh_protocol_v1_BluetoothControllerInfo_fields, &info
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
    for (attempt = 0; attempt < 1000u && fixture.bt_info_callbacks == 0u;
         ++attempt)
        wait_milliseconds(1u);
    assert(fixture.bt_info_callbacks == 1u);
    assert(fixture.bt_info_result == WLH_HOST_OK);
    assert(fixture.bt_info.state == WLH_BLUETOOTH_STATE_ENABLED);
    assert(fixture.bt_info.has_public_address);
    assert(
        memcmp(
            fixture.bt_info.public_address, "\x11\x22\x33\x44\x55\x66", 6u
        ) == 0
    );
    assert(fixture.bt_info.hci_version == 12u);
    assert(fixture.bt_info.manufacturer_id == 0x02e5u);
    assert(fixture.bt_info.feature_bits == 0x123456789abcdef0ull);
    assert(fixture.bt_info.max_hci_packet == 1021u);

    /* A truncated public address is a protocol error, not a guess. */
    tx_before = fixture.tx_count;
    assert(
        wlh_host_bluetooth_get_info(
            &fixture.host, on_bluetooth_info, &fixture
        ) == WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    request_id = captured_request_id(&fixture, &service, &method);
    {
        wlh_protocol_v1_BluetoothControllerInfo info =
            wlh_protocol_v1_BluetoothControllerInfo_init_zero;
        pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
        info.public_address.size = 3u;
        assert(pb_encode(
            &stream, wlh_protocol_v1_BluetoothControllerInfo_fields, &info
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
    for (attempt = 0; attempt < 1000u && fixture.bt_info_callbacks == 1u;
         ++attempt)
        wait_milliseconds(1u);
    assert(fixture.bt_info_callbacks == 2u);
    assert(fixture.bt_info_result == WLH_HOST_PROTOCOL_ERROR);

    /* STATE_CHANGED is normalized into the host event struct. */
    {
        unsigned events_before = fixture.events;
        wlh_protocol_v1_BluetoothStateChangedEvent event =
            wlh_protocol_v1_BluetoothStateChangedEvent_init_zero;
        pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
        const wlh_host_bluetooth_state_event_t *decoded;
        event.state =
            wlh_protocol_v1_BluetoothControllerState_BLUETOOTH_CONTROLLER_STATE_ENABLED;
        event.reason = 7u;
        assert(pb_encode(
            &stream, wlh_protocol_v1_BluetoothStateChangedEvent_fields, &event
        ));
        frame_size = make_rpc_frame(
            frame,
            42u,
            sequence++,
            WLH_SERVICE_BLUETOOTH,
            WLH_BLUETOOTH_EVENT_STATE_CHANGED,
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
            fixture.last_event_kind == WLH_HOST_EVENT_BLUETOOTH_STATE_CHANGED
        );
        assert(
            fixture.last_event_payload_size ==
            sizeof(wlh_host_bluetooth_state_event_t)
        );
        decoded = (const wlh_host_bluetooth_state_event_t *)
                      fixture.last_event_payload;
        assert(
            decoded->state == WLH_BLUETOOTH_STATE_ENABLED &&
            decoded->reason == 7u
        );
    }
    assert(wlh_host_stop(&fixture.host) == WLH_HOST_OK);
}

void test_bluetooth_hci_channel(void) {
    fixture_t fixture;
    uint8_t frame[4096];
    uint8_t payload[512];
    uint8_t record_payload[1100];
    uint8_t record_type;
    size_t record_size;
    size_t frame_size;
    unsigned tx_before;
    unsigned attempt;
    static const uint8_t reset_command[] = {0x03u, 0x0cu, 0x00u};
    static const uint8_t acl_packet[] = {
        0x01u, 0x00u, 0x02u, 0x00u, 0xaau, 0xbbu
    };
    static const uint8_t event_packet[] = {
        0x0eu, 0x04u, 0x01u, 0x03u, 0x0cu, 0x00u
    };
    fixture_init(&fixture);
    establish_ready_bluetooth(&fixture);

    /* Malformed packets and unsupported H4 types never reach the wire. */
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host, WLH_H4_TYPE_SCO, acl_packet, sizeof(acl_packet)
        ) == WLH_HOST_NOT_SUPPORTED
    );
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host, WLH_H4_TYPE_ISO, acl_packet, sizeof(acl_packet)
        ) == WLH_HOST_NOT_SUPPORTED
    );
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host, WLH_H4_TYPE_COMMAND, reset_command, 2u
        ) == WLH_HOST_INVALID_ARGUMENT
    );
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host, WLH_H4_TYPE_ACL, acl_packet, sizeof(acl_packet) - 1u
        ) == WLH_HOST_INVALID_ARGUMENT
    );
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
        ) == WLH_HOST_INVALID_ARGUMENT
    );

    /* A command goes out as one H4 raw record on the HCI channel. */
    tx_before = fixture.tx_count;
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host,
            WLH_H4_TYPE_COMMAND,
            reset_command,
            sizeof(reset_command)
        ) == WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    decode_tx_hci(&fixture, &record_type, record_payload, &record_size);
    assert(record_type == WLH_H4_TYPE_COMMAND);
    assert(
        record_size == sizeof(reset_command) &&
        memcmp(record_payload, reset_command, record_size) == 0
    );

    /* ACL data consumes the second credit. */
    tx_before = fixture.tx_count;
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host, WLH_H4_TYPE_ACL, acl_packet, sizeof(acl_packet)
        ) == WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    decode_tx_hci(&fixture, &record_type, record_payload, &record_size);
    assert(record_type == WLH_H4_TYPE_ACL);
    assert(
        record_size == sizeof(acl_packet) &&
        memcmp(record_payload, acl_packet, record_size) == 0
    );

    /* Out of credit: the send is refused without dropping anything, and the
       tx-ready edge fires exactly when usable credit returns. */
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host,
            WLH_H4_TYPE_COMMAND,
            reset_command,
            sizeof(reset_command)
        ) == WLH_HOST_NO_CREDIT
    );
    assert(fixture.hci_tx_ready_count == 0u);
    {
        wlh_protocol_v1_CreditUpdate update =
            wlh_protocol_v1_CreditUpdate_init_zero;
        pb_ostream_t stream = pb_ostream_from_buffer(payload, sizeof(payload));
        update.channel_id = WLH_CHANNEL_BLUETOOTH_HCI;
        update.units = 1u;
        assert(
            pb_encode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update)
        );
        frame_size = make_rpc_frame(
            frame,
            42u,
            0u,
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
    for (attempt = 0; attempt < 1000u && fixture.hci_tx_ready_count == 0u;
         ++attempt)
        wait_milliseconds(1u);
    assert(fixture.hci_tx_ready_count == 1u);
    tx_before = fixture.tx_count;
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host,
            WLH_H4_TYPE_COMMAND,
            reset_command,
            sizeof(reset_command)
        ) == WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);

    /* A valid inbound event reaches the adapter and returns the credit. */
    tx_before = fixture.tx_count;
    frame_size = make_hci_frame(
        frame, 42u, 0u, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    wait_for_tx(&fixture, tx_before + 1u);
    assert(fixture.hci_rx_count == 1u);
    assert(fixture.hci_rx_type == WLH_H4_TYPE_EVENT);
    assert(
        fixture.hci_rx_size == sizeof(event_packet) &&
        memcmp(fixture.hci_rx_payload, event_packet, sizeof(event_packet)) == 0
    );
    {
        wlh_frame_header_t header;
        wlh_rpc_envelope_t rpc;
        wlh_protocol_v1_CreditUpdate update =
            wlh_protocol_v1_CreditUpdate_init_zero;
        const uint8_t *tx_payload;
        const uint8_t *rpc_payload;
        size_t tx_payload_size = 0u;
        size_t rpc_payload_size = 0u;
        pb_istream_t stream;
        assert(
            wlh_frame_decode(
                &header,
                &tx_payload,
                &tx_payload_size,
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
                tx_payload,
                tx_payload_size,
                sizeof(fixture.tx)
            ) == WLH_WIRE_OK
        );
        assert(
            rpc.service_id == WLH_SERVICE_LINK &&
            rpc.method_id == WLH_LINK_METHOD_CREDIT_UPDATE &&
            rpc.kind == WLH_RPC_KIND_EVENT
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        assert(
            pb_decode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update)
        );
        assert(
            update.channel_id == WLH_CHANNEL_BLUETOOTH_HCI && update.units == 1u
        );
    }

    /* When the adapter rejects a packet the drop is counted but the credit is
       still returned: withholding it would permanently shrink the channel
       window because nothing re-runs Hello after a local drop. */
    tx_before = fixture.tx_count;
    fixture.hci_rx_return = WLH_HOST_PENDING_FULL;
    frame_size = make_hci_frame(
        frame, 42u, 1u, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    wait_for_tx(&fixture, tx_before + 1u);
    assert(fixture.hci_rx_count == 1u);
    fixture.hci_rx_return = WLH_HOST_OK;
    {
        wlh_frame_header_t header;
        wlh_rpc_envelope_t rpc;
        wlh_protocol_v1_CreditUpdate update =
            wlh_protocol_v1_CreditUpdate_init_zero;
        const uint8_t *tx_payload;
        const uint8_t *rpc_payload;
        size_t tx_payload_size = 0u;
        size_t rpc_payload_size = 0u;
        pb_istream_t stream;
        assert(
            wlh_frame_decode(
                &header,
                &tx_payload,
                &tx_payload_size,
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
                tx_payload,
                tx_payload_size,
                sizeof(fixture.tx)
            ) == WLH_WIRE_OK
        );
        assert(
            rpc.service_id == WLH_SERVICE_LINK &&
            rpc.method_id == WLH_LINK_METHOD_CREDIT_UPDATE
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        assert(
            pb_decode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update)
        );
        assert(
            update.channel_id == WLH_CHANNEL_BLUETOOTH_HCI && update.units == 1u
        );
    }
    {
        wlh_host_diagnostics_t diagnostics;
        wlh_host_get_diagnostics(&fixture.host, &diagnostics);
        assert(diagnostics.hci_drops == 1u && diagnostics.hci_malformed == 0u);
    }

    /* A malformed packet stops session HCI: STATE_CHANGED(ERROR) fires, later
       traffic is refused, and nothing was truncated or partially delivered. */
    {
        unsigned events_before = fixture.events;
        static const uint8_t bad_event[] = {
            0x0eu, 0x05u, 0x01u, 0x03u, 0x0cu, 0x00u
        };
        const wlh_host_bluetooth_state_event_t *decoded;
        frame_size = make_hci_frame(
            frame, 42u, 2u, WLH_H4_TYPE_EVENT, bad_event, sizeof(bad_event)
        );
        assert(
            wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK
        );
        for (attempt = 0; attempt < 1000u && fixture.events == events_before;
             ++attempt)
            wait_milliseconds(1u);
        assert(fixture.events == events_before + 1u);
        assert(
            fixture.last_event_kind == WLH_HOST_EVENT_BLUETOOTH_STATE_CHANGED
        );
        decoded = (const wlh_host_bluetooth_state_event_t *)
                      fixture.last_event_payload;
        assert(
            decoded->state == WLH_BLUETOOTH_STATE_ERROR &&
            decoded->reason == WLH_HOST_BLUETOOTH_REASON_MALFORMED_HCI
        );
        assert(fixture.hci_rx_count == 1u);
    }
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host,
            WLH_H4_TYPE_COMMAND,
            reset_command,
            sizeof(reset_command)
        ) == WLH_HOST_INVALID_STATE
    );
    frame_size = make_hci_frame(
        frame, 42u, 3u, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    wait_milliseconds(20u);
    assert(fixture.hci_rx_count == 1u);
    {
        wlh_host_diagnostics_t diagnostics;
        wlh_host_get_diagnostics(&fixture.host, &diagnostics);
        assert(diagnostics.hci_malformed == 1u && diagnostics.hci_drops == 2u);
    }

    /* Link recovery re-negotiates the session and reopens HCI. */
    tx_before = fixture.tx_count;
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
    wait_for_tx(&fixture, tx_before + 1u);
    send_bluetooth_hello(&fixture, 43u);
    tx_before = fixture.tx_count;
    assert(
        wlh_host_bluetooth_hci_send(
            &fixture.host,
            WLH_H4_TYPE_COMMAND,
            reset_command,
            sizeof(reset_command)
        ) == WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    decode_tx_hci(&fixture, &record_type, record_payload, &record_size);
    assert(record_type == WLH_H4_TYPE_COMMAND);
    assert(wlh_host_stop(&fixture.host) == WLH_HOST_OK);
}

static void assert_last_tx_credit_update(
    const fixture_t *fixture, uint8_t channel
) {
    wlh_frame_header_t header;
    wlh_rpc_envelope_t rpc;
    wlh_protocol_v1_CreditUpdate update =
        wlh_protocol_v1_CreditUpdate_init_zero;
    const uint8_t *tx_payload;
    const uint8_t *rpc_payload;
    size_t tx_payload_size = 0u;
    size_t rpc_payload_size = 0u;
    pb_istream_t stream;
    assert(
        wlh_frame_decode(
            &header,
            &tx_payload,
            &tx_payload_size,
            fixture->tx,
            fixture->tx_size,
            sizeof(fixture->tx)
        ) == WLH_WIRE_OK
    );
    assert(header.channel == WLH_CHANNEL_LINK_CONTROL);
    assert(
        wlh_rpc_decode(
            &rpc,
            &rpc_payload,
            &rpc_payload_size,
            tx_payload,
            tx_payload_size,
            sizeof(fixture->tx)
        ) == WLH_WIRE_OK
    );
    assert(
        rpc.service_id == WLH_SERVICE_LINK &&
        rpc.method_id == WLH_LINK_METHOD_CREDIT_UPDATE
    );
    stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
    assert(pb_decode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update));
    assert(update.channel_id == channel && update.units == 1u);
}

void test_bluetooth_adv_channel(void) {
    fixture_t fixture;
    uint8_t frame[4096];
    size_t frame_size;
    unsigned tx_before;
    unsigned attempt;
    /* LE Meta advertising report (subevent 0x02). */
    static const uint8_t adv_report[] = {
        0x3eu,
        0x0cu,
        0x02u,
        0x01u,
        0x00u,
        0x00u,
        0x01u,
        0x02u,
        0x03u,
        0x04u,
        0x05u,
        0x06u,
        0x00u,
        0xd0u
    };
    static const uint8_t acl_packet[] = {
        0x01u, 0x00u, 0x02u, 0x00u, 0xaau, 0xbbu
    };
    fixture_init(&fixture);
    assert(wlh_host_start(&fixture.host) == WLH_HOST_OK);
    wait_for_state(&fixture, WLH_HOST_STATE_NEGOTIATING);
    wait_for_tx(&fixture, 1u);

    /* The HelloRequest declares the best-effort ADV channel capability. */
    {
        wlh_frame_header_t header;
        wlh_rpc_envelope_t rpc;
        wlh_protocol_v1_HelloRequest hello =
            wlh_protocol_v1_HelloRequest_init_zero;
        const uint8_t *tx_payload;
        const uint8_t *rpc_payload;
        size_t tx_payload_size = 0u;
        size_t rpc_payload_size = 0u;
        size_t index;
        bool adv_declared = false;
        pb_istream_t stream;
        assert(
            wlh_frame_decode(
                &header,
                &tx_payload,
                &tx_payload_size,
                fixture.tx,
                fixture.tx_size,
                sizeof(fixture.tx)
            ) == WLH_WIRE_OK
        );
        assert(
            wlh_rpc_decode(
                &rpc,
                &rpc_payload,
                &rpc_payload_size,
                tx_payload,
                tx_payload_size,
                sizeof(fixture.tx)
            ) == WLH_WIRE_OK
        );
        assert(
            rpc.service_id == WLH_SERVICE_LINK &&
            rpc.method_id == WLH_LINK_METHOD_HELLO
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        assert(pb_decode(&stream, wlh_protocol_v1_HelloRequest_fields, &hello));
        for (index = 0; index < hello.channels_count; ++index) {
            if (hello.channels[index].channel_id ==
                WLH_CHANNEL_BLUETOOTH_HCI_ADV)
                adv_declared = true;
        }
        assert(adv_declared);
    }
    send_bluetooth_hello(&fixture, 42u);

    /* A valid advertising report on the ADV channel reaches the adapter and
       the credit comes back on the same channel. */
    tx_before = fixture.tx_count;
    frame_size = make_hci_channel_frame(
        frame,
        WLH_CHANNEL_BLUETOOTH_HCI_ADV,
        42u,
        0u,
        WLH_H4_TYPE_EVENT,
        adv_report,
        sizeof(adv_report)
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    wait_for_tx(&fixture, tx_before + 1u);
    assert(fixture.hci_rx_count == 1u);
    assert(fixture.hci_rx_type == WLH_H4_TYPE_EVENT);
    assert(
        fixture.hci_rx_size == sizeof(adv_report) &&
        memcmp(fixture.hci_rx_payload, adv_report, sizeof(adv_report)) == 0
    );
    assert_last_tx_credit_update(&fixture, WLH_CHANNEL_BLUETOOTH_HCI_ADV);

    /* Adapter rejection still returns the ADV credit; only diagnostics
       record the drop. */
    tx_before = fixture.tx_count;
    fixture.hci_rx_return = WLH_HOST_PENDING_FULL;
    frame_size = make_hci_channel_frame(
        frame,
        WLH_CHANNEL_BLUETOOTH_HCI_ADV,
        42u,
        1u,
        WLH_H4_TYPE_EVENT,
        adv_report,
        sizeof(adv_report)
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    wait_for_tx(&fixture, tx_before + 1u);
    fixture.hci_rx_return = WLH_HOST_OK;
    assert_last_tx_credit_update(&fixture, WLH_CHANNEL_BLUETOOTH_HCI_ADV);
    {
        wlh_host_diagnostics_t diagnostics;
        wlh_host_get_diagnostics(&fixture.host, &diagnostics);
        assert(diagnostics.hci_drops == 1u && diagnostics.hci_malformed == 0u);
    }

    /* ACL records are reliable traffic and must not ride the best-effort
       channel: treat them as malformed HCI. */
    {
        unsigned events_before = fixture.events;
        const wlh_host_bluetooth_state_event_t *decoded;
        frame_size = make_hci_channel_frame(
            frame,
            WLH_CHANNEL_BLUETOOTH_HCI_ADV,
            42u,
            2u,
            WLH_H4_TYPE_ACL,
            acl_packet,
            sizeof(acl_packet)
        );
        assert(
            wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK
        );
        for (attempt = 0; attempt < 1000u && fixture.events == events_before;
             ++attempt)
            wait_milliseconds(1u);
        assert(fixture.events == events_before + 1u);
        assert(
            fixture.last_event_kind == WLH_HOST_EVENT_BLUETOOTH_STATE_CHANGED
        );
        decoded = (const wlh_host_bluetooth_state_event_t *)
                      fixture.last_event_payload;
        assert(
            decoded->state == WLH_BLUETOOTH_STATE_ERROR &&
            decoded->reason == WLH_HOST_BLUETOOTH_REASON_MALFORMED_HCI
        );
        assert(fixture.hci_rx_count == 1u);
    }
    assert(wlh_host_stop(&fixture.host) == WLH_HOST_OK);
}
