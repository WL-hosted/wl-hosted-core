#include "coproc_test_support.h"

void test_bluetooth_lifecycle_and_info(void) {
    fixture_t f;
    wlh_coproc_t core;
    wlh_rpc_envelope_t rpc;
    const uint8_t *rpc_payload;
    size_t rpc_payload_size;
    pb_istream_t stream;
    unsigned sent_before;
    wlh_protocol_v1_Empty empty = wlh_protocol_v1_Empty_init_zero;
    wlh_protocol_v1_BluetoothInitializeRequest init =
        wlh_protocol_v1_BluetoothInitializeRequest_init_zero;
    wlh_protocol_v1_BluetoothEnableRequest enable =
        wlh_protocol_v1_BluetoothEnableRequest_init_zero;
    wlh_protocol_v1_BluetoothDeinitializeRequest deinit =
        wlh_protocol_v1_BluetoothDeinitializeRequest_init_zero;

    memset(&f, 0, sizeof(f));
    f.core = &core;
    wlh_posix_osal_init(&f.posix);
    prepare_ready_bt_core(&f, &core, false);

    /* Hello advertises the Bluetooth service, HCI channel and credits. */
    {
        wlh_protocol_v1_HelloResponse response =
            wlh_protocol_v1_HelloResponse_init_zero;
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(
            pb_decode(&stream, wlh_protocol_v1_HelloResponse_fields, &response)
        );
        CHECK(response.initial_credits_count == 5u);
        CHECK(
            response.initial_credits[4].channel_id ==
                WLH_CHANNEL_BLUETOOTH_HCI &&
            response.initial_credits[4].units ==
                WLH_COPROC_BLUETOOTH_INITIAL_CREDIT &&
            response.initial_credits[4].unit_bytes == 1u
        );
        CHECK(
            response.services_count == 1u &&
            response.services[0].service_id == WLH_SERVICE_BLUETOOTH &&
            response.services[0].major == 1u
        );
        CHECK(
            response.channels_count == 1u &&
            response.channels[0].channel_id == WLH_CHANNEL_BLUETOOTH_HCI &&
            response.channels[0].max_frame_payload == WLH_COPROC_MAX_HCI_PACKET
        );
    }

    /* ENABLE before INITIALIZE is NOT_READY. */
    bt_expect_status(
        &f,
        &core,
        WLH_BLUETOOTH_METHOD_ENABLE,
        200,
        wlh_protocol_v1_BluetoothEnableRequest_fields,
        &enable,
        WLH_STATUS_NOT_READY
    );

    /* Non-HCI transports are rejected. */
    bt_expect_status(
        &f,
        &core,
        WLH_BLUETOOTH_METHOD_INITIALIZE,
        201,
        wlh_protocol_v1_BluetoothInitializeRequest_fields,
        &init,
        WLH_STATUS_NOT_SUPPORTED
    );

    /* INITIALIZE runs asynchronously on the backend, without a response
       until the completion arrives. */
    init.transport = wlh_protocol_v1_BluetoothTransport_BLUETOOTH_TRANSPORT_HCI;
    init.feature_flags = 5u;
    sent_before = f.sent_count;
    bt_send_request(
        &core,
        WLH_BLUETOOTH_METHOD_INITIALIZE,
        202,
        wlh_protocol_v1_BluetoothInitializeRequest_fields,
        &init
    );
    wait_for_counter(&f.bt_initializes, 1u);
    CHECK(f.bt_last_feature_flags == 5u);
    CHECK(f.sent_count == sent_before);

    /* Overlapping requests are refused while an operation is pending. */
    bt_expect_status(
        &f,
        &core,
        WLH_BLUETOOTH_METHOD_INITIALIZE,
        203,
        wlh_protocol_v1_BluetoothInitializeRequest_fields,
        &init,
        WLH_STATUS_BUSY
    );

    /* Mismatched or wrong-kind completions are ignored and counted. */
    {
        wlh_coproc_bluetooth_info_t info;
        memset(&info, 0, sizeof(info));
        CHECK(
            wlh_coproc_bluetooth_operation_complete(
                &core, f.bt_last_operation_id + 99u, 0
            ) == WLH_COPROC_OK
        );
        wait_for_bt_mismatches(&core, 1u);
        CHECK(
            wlh_coproc_bluetooth_info_result(
                &core, f.bt_last_operation_id, 0, &info
            ) == WLH_COPROC_OK
        );
        wait_for_bt_mismatches(&core, 2u);
    }

    /* The matching completion resolves request 202 with OK. */
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_bluetooth_operation_complete(
            &core, f.bt_last_operation_id, 0
        ) == WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);
    decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
    CHECK(
        rpc.request_id == 202 && rpc.kind == WLH_RPC_KIND_RESPONSE &&
        rpc.status_code == WLH_STATUS_OK
    );

    /* Re-INITIALIZE after success is an idempotent OK. */
    bt_expect_status(
        &f,
        &core,
        WLH_BLUETOOTH_METHOD_INITIALIZE,
        204,
        wlh_protocol_v1_BluetoothInitializeRequest_fields,
        &init,
        WLH_STATUS_OK
    );
    CHECK(f.bt_initializes == 1u);

    /* ENABLE completes asynchronously and is idempotent afterwards. */
    enable.mode_flags = 3u;
    bt_send_request(
        &core,
        WLH_BLUETOOTH_METHOD_ENABLE,
        205,
        wlh_protocol_v1_BluetoothEnableRequest_fields,
        &enable
    );
    wait_for_counter(&f.bt_enables, 1u);
    CHECK(f.bt_last_mode_flags == 3u);
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_bluetooth_operation_complete(
            &core, f.bt_last_operation_id, 0
        ) == WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);
    decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
    CHECK(rpc.request_id == 205 && rpc.status_code == WLH_STATUS_OK);
    bt_expect_status(
        &f,
        &core,
        WLH_BLUETOOTH_METHOD_ENABLE,
        206,
        wlh_protocol_v1_BluetoothEnableRequest_fields,
        &enable,
        WLH_STATUS_OK
    );
    CHECK(f.bt_enables == 1u);

    /* GET_INFO ignores a completion of the wrong kind... */
    bt_send_request(
        &core,
        WLH_BLUETOOTH_METHOD_GET_INFO,
        207,
        wlh_protocol_v1_Empty_fields,
        &empty
    );
    wait_for_counter(&f.bt_get_infos, 1u);
    CHECK(
        wlh_coproc_bluetooth_operation_complete(
            &core, f.bt_last_operation_id, 0
        ) == WLH_COPROC_OK
    );
    wait_for_bt_mismatches(&core, 3u);

    /* ...then reports the backend info with the core-owned state. */
    {
        wlh_coproc_bluetooth_info_t info;
        wlh_protocol_v1_BluetoothControllerInfo decoded =
            wlh_protocol_v1_BluetoothControllerInfo_init_zero;
        memset(&info, 0, sizeof(info));
        memcpy(info.public_address, "\x11\x22\x33\x44\x55\x66", 6u);
        info.has_public_address = true;
        info.hci_version = 12u;
        info.manufacturer_id = 0x02e5u;
        info.feature_bits = 0x123456789abcdef0ull;
        info.max_hci_packet = 512u;
        sent_before = f.sent_count;
        CHECK(
            wlh_coproc_bluetooth_info_result(
                &core, f.bt_last_operation_id, 0, &info
            ) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(rpc.request_id == 207 && rpc.status_code == WLH_STATUS_OK);
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(
            &stream, wlh_protocol_v1_BluetoothControllerInfo_fields, &decoded
        ));
        CHECK(
            decoded.state ==
            wlh_protocol_v1_BluetoothControllerState_BLUETOOTH_CONTROLLER_STATE_ENABLED
        );
        CHECK(
            decoded.public_address.size == 6u &&
            decoded.public_address.bytes[0] == 0x11u &&
            decoded.public_address.bytes[5] == 0x66u
        );
        CHECK(decoded.hci_version == 12u && decoded.manufacturer_id == 0x02e5u);
        CHECK(
            decoded.feature_bits == 0x123456789abcdef0ull &&
            decoded.max_hci_packet == 512u
        );
    }

    /* DISABLE and DEINITIALIZE walk the state back down. */
    bt_send_request(
        &core,
        WLH_BLUETOOTH_METHOD_DISABLE,
        208,
        wlh_protocol_v1_Empty_fields,
        &empty
    );
    wait_for_counter(&f.bt_disables, 1u);
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_bluetooth_operation_complete(
            &core, f.bt_last_operation_id, 0
        ) == WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);
    decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
    CHECK(rpc.request_id == 208 && rpc.status_code == WLH_STATUS_OK);

    deinit.release_memory = true;
    bt_send_request(
        &core,
        WLH_BLUETOOTH_METHOD_DEINITIALIZE,
        209,
        wlh_protocol_v1_BluetoothDeinitializeRequest_fields,
        &deinit
    );
    wait_for_counter(&f.bt_deinitializes, 1u);
    CHECK(f.bt_last_release_memory);
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_bluetooth_operation_complete(
            &core, f.bt_last_operation_id, 0
        ) == WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);
    decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
    CHECK(rpc.request_id == 209 && rpc.status_code == WLH_STATUS_OK);

    /* Repeating DEINITIALIZE is an idempotent OK. */
    bt_expect_status(
        &f,
        &core,
        WLH_BLUETOOTH_METHOD_DEINITIALIZE,
        210,
        wlh_protocol_v1_BluetoothDeinitializeRequest_fields,
        &deinit,
        WLH_STATUS_OK
    );
    CHECK(f.bt_deinitializes == 1u);

    /* A backend submit failure resolves the request with INTERNAL... */
    f.bt_status = -1;
    bt_expect_status(
        &f,
        &core,
        WLH_BLUETOOTH_METHOD_INITIALIZE,
        211,
        wlh_protocol_v1_BluetoothInitializeRequest_fields,
        &init,
        WLH_STATUS_INTERNAL
    );
    CHECK(f.bt_initializes == 2u);

    /* ...and leaves no pending operation behind. */
    f.bt_status = 0;
    bt_send_request(
        &core,
        WLH_BLUETOOTH_METHOD_INITIALIZE,
        212,
        wlh_protocol_v1_BluetoothInitializeRequest_fields,
        &init
    );
    wait_for_counter(&f.bt_initializes, 3u);
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_bluetooth_operation_complete(
            &core, f.bt_last_operation_id, 0
        ) == WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);
    decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
    CHECK(rpc.request_id == 212 && rpc.status_code == WLH_STATUS_OK);

    /* A failing backend completion also resolves with INTERNAL. */
    bt_send_request(
        &core,
        WLH_BLUETOOTH_METHOD_ENABLE,
        213,
        wlh_protocol_v1_BluetoothEnableRequest_fields,
        &enable
    );
    wait_for_counter(&f.bt_enables, 2u);
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_bluetooth_operation_complete(
            &core, f.bt_last_operation_id, -7
        ) == WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);
    decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
    CHECK(
        rpc.request_id == 213 &&
        rpc.status_domain == WLH_STATUS_DOMAIN_BLUETOOTH &&
        rpc.status_code == WLH_STATUS_INTERNAL
    );

    CHECK(wlh_coproc_stop(&core) == WLH_COPROC_OK);
}

void test_bluetooth_hci(void) {
    fixture_t f;
    wlh_coproc_t core;
    wlh_rpc_envelope_t rpc;
    const uint8_t *rpc_payload;
    size_t rpc_payload_size;
    pb_istream_t stream;
    unsigned sent_before;
    static const uint8_t command_packet[3] = {0x03, 0x0c, 0x00};
    static const uint8_t event_packet[5] = {0x0e, 0x03, 0x01, 0x03, 0x0c};

    memset(&f, 0, sizeof(f));
    f.core = &core;
    wlh_posix_osal_init(&f.posix);
    prepare_ready_bt_core(&f, &core, false);

    /* A Host->Controller command reaches the backend and returns the
       credit. */
    {
        static const uint8_t records[11] = {
            1, 0, 8, 0, 3, 0, 0, 0, 0x03, 0x0c, 0x00
        };
        wlh_protocol_v1_CreditUpdate update =
            wlh_protocol_v1_CreditUpdate_init_zero;
        sent_before = f.sent_count;
        hci_host_frame(&core, 42u, records, sizeof(records));
        wait_for_counter(&f.bt_hci_packets, 1u);
        CHECK(f.bt_last_h4_type == WLH_H4_TYPE_COMMAND);
        CHECK(
            f.bt_last_hci_size == 3u &&
            memcmp(f.bt_last_hci, command_packet, 3u) == 0
        );
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.service_id == WLH_SERVICE_LINK &&
            rpc.method_id == WLH_LINK_METHOD_CREDIT_UPDATE &&
            rpc.kind == WLH_RPC_KIND_EVENT
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update));
        CHECK(
            update.channel_id == WLH_CHANNEL_BLUETOOTH_HCI && update.units == 1u
        );
    }

    /* An aggregated frame delivers every record for a single credit. */
    {
        static const uint8_t acl_packet[9] = {
            0x01, 0x00, 0x05, 0x00, 1, 2, 3, 4, 5
        };
        static const uint8_t records[28] = {
            1, 0, 8, 0, 3, 0,    0,    0,    0x03, 0x0c, 0x00, 2, 0, 8,
            0, 9, 0, 0, 0, 0x01, 0x00, 0x05, 0x00, 1,    2,    3, 4, 5
        };
        wlh_protocol_v1_CreditUpdate update =
            wlh_protocol_v1_CreditUpdate_init_zero;
        sent_before = f.sent_count;
        hci_host_frame(&core, 42u, records, sizeof(records));
        wait_for_counter(&f.bt_hci_packets, 3u);
        CHECK(f.bt_last_h4_type == WLH_H4_TYPE_ACL);
        CHECK(
            f.bt_last_hci_size == 9u &&
            memcmp(f.bt_last_hci, acl_packet, 9u) == 0
        );
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(rpc.method_id == WLH_LINK_METHOD_CREDIT_UPDATE);
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update));
        CHECK(
            update.channel_id == WLH_CHANNEL_BLUETOOTH_HCI && update.units == 1u
        );
    }

    /* Controller->Host validation rejects unsupported and malformed
       packets. */
    CHECK(
        wlh_coproc_bluetooth_hci_send(
            &core, WLH_H4_TYPE_SCO, event_packet, sizeof(event_packet)
        ) == WLH_COPROC_NOT_SUPPORTED
    );
    CHECK(
        wlh_coproc_bluetooth_hci_send(
            &core, WLH_H4_TYPE_ISO, event_packet, sizeof(event_packet)
        ) == WLH_COPROC_NOT_SUPPORTED
    );
    CHECK(
        wlh_coproc_bluetooth_hci_send(
            &core, WLH_H4_TYPE_COMMAND, command_packet, sizeof(command_packet)
        ) == WLH_COPROC_INVALID_ARGUMENT
    );
    CHECK(
        wlh_coproc_bluetooth_hci_send(
            &core, WLH_H4_TYPE_EVENT, event_packet, 4u
        ) == WLH_COPROC_INVALID_ARGUMENT
    );

    /* A valid event is framed as an H4 raw record on the HCI channel. */
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_bluetooth_hci_send(
            &core, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
        ) == WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);
    check_sent_hci_record(
        &f, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
    );

    /* Without ADV negotiation advertising reports fall back to the reliable
       HCI channel. */
    {
        static const uint8_t adv_report[14] = {
            0x3e,
            0x0c,
            0x02,
            0x01,
            0x00,
            0x00,
            0x01,
            0x02,
            0x03,
            0x04,
            0x05,
            0x06,
            0x00,
            0xd0
        };
        sent_before = f.sent_count;
        CHECK(
            wlh_coproc_bluetooth_hci_send(
                &core, WLH_H4_TYPE_EVENT, adv_report, sizeof(adv_report)
            ) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        check_sent_hci_record(
            &f, WLH_H4_TYPE_EVENT, adv_report, sizeof(adv_report)
        );
    }

    /* Draining the advertised credit yields NO_CREDIT without dropping. */
    {
        unsigned i;
        for (i = 2u; i < WLH_COPROC_BLUETOOTH_INITIAL_CREDIT; ++i) {
            sent_before = f.sent_count;
            CHECK(
                wlh_coproc_bluetooth_hci_send(
                    &core, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
                ) == WLH_COPROC_OK
            );
            wait_for_sent(&f, sent_before + 1u);
        }
        CHECK(
            wlh_coproc_bluetooth_hci_send(
                &core, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
            ) == WLH_COPROC_NO_CREDIT
        );
        CHECK(f.bt_tx_ready_calls == 0u);
    }

    /* Only the zero-to-positive credit edge fires hci_tx_ready. */
    {
        wlh_protocol_v1_CreditUpdate update =
            wlh_protocol_v1_CreditUpdate_init_zero;
        uint8_t incoming[4096];
        size_t incoming_size;
        update.channel_id = WLH_CHANNEL_BLUETOOTH_HCI;
        update.units = 1u;
        sent_before = f.sent_count;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            0,
            WLH_SERVICE_LINK,
            WLH_LINK_METHOD_CREDIT_UPDATE,
            300,
            wlh_protocol_v1_CreditUpdate_fields,
            &update
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.bt_tx_ready_calls == 1u);

        sent_before = f.sent_count;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            0,
            WLH_SERVICE_LINK,
            WLH_LINK_METHOD_CREDIT_UPDATE,
            301,
            wlh_protocol_v1_CreditUpdate_fields,
            &update
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.bt_tx_ready_calls == 1u);

        sent_before = f.sent_count;
        CHECK(
            wlh_coproc_bluetooth_hci_send(
                &core, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
            ) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        check_sent_hci_record(
            &f, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
        );
    }

    /* A rejecting backend withholds the credit and counts the drop. */
    {
        static const uint8_t records[11] = {
            1, 0, 8, 0, 3, 0, 0, 0, 0x03, 0x0c, 0x00
        };
        unsigned packets_before = f.bt_hci_packets;
        f.bt_hci_status = -1;
        sent_before = f.sent_count;
        hci_host_frame(&core, 42u, records, sizeof(records));
        wait_for_hci_drops(&core, 1u);
        CHECK(f.bt_hci_packets == packets_before + 1u);
        CHECK(f.sent_count == sent_before);
        f.bt_hci_status = 0;
    }

    /* A malformed record poisons the frame and latches ERROR. */
    {
        static const uint8_t records[11] = {
            1, 0, 8, 0, 3, 0, 0, 0, 0x03, 0x0c, 0x05
        };
        wlh_protocol_v1_BluetoothStateChangedEvent event =
            wlh_protocol_v1_BluetoothStateChangedEvent_init_zero;
        wlh_coproc_diagnostics_t diagnostics;
        unsigned packets_before = f.bt_hci_packets;
        sent_before = f.sent_count;
        hci_host_frame(&core, 42u, records, sizeof(records));
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.service_id == WLH_SERVICE_BLUETOOTH &&
            rpc.method_id == WLH_BLUETOOTH_EVENT_STATE_CHANGED &&
            rpc.kind == WLH_RPC_KIND_EVENT
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(
            &stream, wlh_protocol_v1_BluetoothStateChangedEvent_fields, &event
        ));
        CHECK(
            event.state ==
                wlh_protocol_v1_BluetoothControllerState_BLUETOOTH_CONTROLLER_STATE_ERROR &&
            event.reason == WLH_COPROC_BLUETOOTH_REASON_MALFORMED_HCI
        );
        CHECK(f.bt_hci_packets == packets_before);
        wlh_coproc_get_diagnostics(&core, &diagnostics);
        CHECK(diagnostics.hci_malformed == 1u);

        /* Session HCI now stops in both directions. */
        {
            static const uint8_t good[11] = {
                1, 0, 8, 0, 3, 0, 0, 0, 0x03, 0x0c, 0x00
            };
            hci_host_frame(&core, 42u, good, sizeof(good));
            wait_for_hci_drops(&core, 2u);
            CHECK(f.bt_hci_packets == packets_before);
        }
        CHECK(
            wlh_coproc_bluetooth_hci_send(
                &core, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
            ) == WLH_COPROC_INVALID_STATE
        );
    }

    /* A new Hello restores HCI service on the next session. */
    {
        wlh_protocol_v1_HelloRequest hello =
            wlh_protocol_v1_HelloRequest_init_zero;
        wlh_protocol_v1_HelloResponse response =
            wlh_protocol_v1_HelloResponse_init_zero;
        uint8_t incoming[4096];
        size_t incoming_size;
        uint32_t new_session;
        unsigned packets_before = f.bt_hci_packets;
        hello.protocol_versions_count = 1;
        hello.protocol_versions[0].major = 1;
        hello.max_frame_size = 4096;
        sent_before = f.sent_count;
        incoming_size = make_rpc_frame(
            incoming,
            0,
            0,
            WLH_SERVICE_LINK,
            WLH_LINK_METHOD_HELLO,
            400,
            wlh_protocol_v1_HelloRequest_fields,
            &hello
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(rpc.request_id == 400 && rpc.kind == WLH_RPC_KIND_RESPONSE);
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(
            pb_decode(&stream, wlh_protocol_v1_HelloResponse_fields, &response)
        );
        new_session = response.session_id;
        CHECK(new_session != 0u && new_session != 42u);

        {
            static const uint8_t records[11] = {
                1, 0, 8, 0, 3, 0, 0, 0, 0x03, 0x0c, 0x00
            };
            sent_before = f.sent_count;
            hci_host_frame(&core, new_session, records, sizeof(records));
            wait_for_counter(&f.bt_hci_packets, packets_before + 1u);
            wait_for_sent(&f, sent_before + 1u);
        }
    }

    /* Backend-reported fatal errors latch ERROR with the given reason. */
    {
        wlh_protocol_v1_BluetoothStateChangedEvent event =
            wlh_protocol_v1_BluetoothStateChangedEvent_init_zero;
        sent_before = f.sent_count;
        CHECK(wlh_coproc_bluetooth_fatal_error(&core, 7u) == WLH_COPROC_OK);
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(rpc.method_id == WLH_BLUETOOTH_EVENT_STATE_CHANGED);
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(
            &stream, wlh_protocol_v1_BluetoothStateChangedEvent_fields, &event
        ));
        CHECK(
            event.state ==
                wlh_protocol_v1_BluetoothControllerState_BLUETOOTH_CONTROLLER_STATE_ERROR &&
            event.reason == 7u
        );
        CHECK(
            wlh_coproc_bluetooth_hci_send(
                &core, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
            ) == WLH_COPROC_INVALID_STATE
        );
    }

    CHECK(wlh_coproc_stop(&core) == WLH_COPROC_OK);
}

void test_bluetooth_adv_channel(void) {
    fixture_t f;
    wlh_coproc_t core;
    wlh_rpc_envelope_t rpc;
    const uint8_t *rpc_payload;
    size_t rpc_payload_size;
    pb_istream_t stream;
    unsigned sent_before;
    unsigned i;
    static const uint8_t event_packet[5] = {0x0e, 0x03, 0x01, 0x03, 0x0c};
    static const uint8_t adv_report[14] = {
        0x3e,
        0x0c,
        0x02,
        0x01,
        0x00,
        0x00,
        0x01,
        0x02,
        0x03,
        0x04,
        0x05,
        0x06,
        0x00,
        0xd0
    };

    memset(&f, 0, sizeof(f));
    f.core = &core;
    wlh_posix_osal_init(&f.posix);
    prepare_ready_bt_core(&f, &core, true);

    /* Hello grants a dedicated best-effort credit pool for the ADV
       channel. */
    {
        wlh_protocol_v1_HelloResponse response =
            wlh_protocol_v1_HelloResponse_init_zero;
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(
            pb_decode(&stream, wlh_protocol_v1_HelloResponse_fields, &response)
        );
        CHECK(response.initial_credits_count == 6u);
        CHECK(
            response.initial_credits[5].channel_id ==
                WLH_CHANNEL_BLUETOOTH_HCI_ADV &&
            response.initial_credits[5].units ==
                WLH_COPROC_BLUETOOTH_ADV_INITIAL_CREDIT &&
            response.initial_credits[5].unit_bytes == 1u
        );
        CHECK(
            response.channels_count == 2u &&
            response.channels[1].channel_id == WLH_CHANNEL_BLUETOOTH_HCI_ADV &&
            response.channels[1].max_frame_payload == WLH_COPROC_MAX_HCI_PACKET
        );
    }

    /* Advertising reports ride the ADV channel; other events stay on the
       reliable channel. */
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_bluetooth_hci_send(
            &core, WLH_H4_TYPE_EVENT, adv_report, sizeof(adv_report)
        ) == WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);
    check_sent_hci_record_on(
        &f,
        WLH_CHANNEL_BLUETOOTH_HCI_ADV,
        WLH_H4_TYPE_EVENT,
        adv_report,
        sizeof(adv_report)
    );

    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_bluetooth_hci_send(
            &core, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
        ) == WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);
    check_sent_hci_record(
        &f, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
    );

    /* Exhausting the ADV window sheds reports at the source: the call still
       succeeds, nothing is queued, and the backend is never backpressured. */
    for (i = 1u; i < WLH_COPROC_BLUETOOTH_ADV_INITIAL_CREDIT; ++i) {
        sent_before = f.sent_count;
        CHECK(
            wlh_coproc_bluetooth_hci_send(
                &core, WLH_H4_TYPE_EVENT, adv_report, sizeof(adv_report)
            ) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
    }
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_bluetooth_hci_send(
            &core, WLH_H4_TYPE_EVENT, adv_report, sizeof(adv_report)
        ) == WLH_COPROC_OK
    );
    {
        wlh_coproc_diagnostics_t diagnostics;
        wlh_coproc_get_diagnostics(&core, &diagnostics);
        CHECK(diagnostics.hci_adv_drops == 1u);
    }
    CHECK(f.sent_count == sent_before);
    CHECK(f.bt_tx_ready_calls == 0u);

    /* Reliable events keep flowing while the ADV window is exhausted. */
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_bluetooth_hci_send(
            &core, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
        ) == WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);
    check_sent_hci_record(
        &f, WLH_H4_TYPE_EVENT, event_packet, sizeof(event_packet)
    );

    /* An ADV credit update reopens the window without a tx-ready edge. */
    {
        wlh_protocol_v1_CreditUpdate update =
            wlh_protocol_v1_CreditUpdate_init_zero;
        uint8_t incoming[4096];
        size_t incoming_size;
        update.channel_id = WLH_CHANNEL_BLUETOOTH_HCI_ADV;
        update.units = 1u;
        sent_before = f.sent_count;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            0,
            WLH_SERVICE_LINK,
            WLH_LINK_METHOD_CREDIT_UPDATE,
            500,
            wlh_protocol_v1_CreditUpdate_fields,
            &update
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.bt_tx_ready_calls == 0u);

        sent_before = f.sent_count;
        CHECK(
            wlh_coproc_bluetooth_hci_send(
                &core, WLH_H4_TYPE_EVENT, adv_report, sizeof(adv_report)
            ) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        check_sent_hci_record_on(
            &f,
            WLH_CHANNEL_BLUETOOTH_HCI_ADV,
            WLH_H4_TYPE_EVENT,
            adv_report,
            sizeof(adv_report)
        );
    }

    CHECK(wlh_coproc_stop(&core) == WLH_COPROC_OK);
}
