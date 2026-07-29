#include "coproc_test_support.h"

void test_hello_wifi_and_ethernet(void) {
    fixture_t f;
    wlh_coproc_t core;
    wlh_coproc_config_t config;
    uint8_t incoming[4096];
    size_t incoming_size;
    wlh_protocol_v1_HelloRequest hello = wlh_protocol_v1_HelloRequest_init_zero;
    wlh_frame_header_t frame_header;
    wlh_rpc_envelope_t rpc;
    const uint8_t *frame_payload, *rpc_payload;
    size_t frame_payload_size, rpc_payload_size;
    wlh_protocol_v1_HelloResponse response =
        wlh_protocol_v1_HelloResponse_init_zero;
    pb_istream_t stream;

    memset(&f, 0, sizeof(f));
    memset(&config, 0, sizeof(config));
    wlh_posix_osal_init(&f.posix);
    f.core = &core;

    config.port.context = &f;
    config.port.submit_tx = submit_frame;
    config.port.ethernet_rx = ethernet_rx;
    config.port.ethernet_ap_rx = ethernet_ap_rx;
    config.buffers = (wlh_coproc_buffer_ops_t){&f, buffer_alloc, buffer_free};
    config.osal = wlh_posix_osal_ops(&f.posix);

    config.wifi.context = &f;
    config.wifi.initialize = wifi_init;

    config.max_frame_size = 4096;
    config.heartbeat_interval_ms = 1000;
    config.initial_credit = 8;
    config.initial_session_id = 42;
    config.core_queue_depth = 8u;

    CHECK(wlh_coproc_init(&core, &config) == WLH_COPROC_OK);
    CHECK(wlh_coproc_start(&core) == WLH_COPROC_OK);
    wait_for_state(&core, WLH_COPROC_STATE_WAITING_FOR_HELLO);

    hello.protocol_versions_count = 1;
    hello.protocol_versions[0].major = 1;
    hello.max_frame_size = 4096;
    incoming_size = make_rpc_frame(
        incoming,
        0,
        0,
        WLH_SERVICE_LINK,
        WLH_LINK_METHOD_HELLO,
        7,
        wlh_protocol_v1_HelloRequest_fields,
        &hello
    );
    CHECK(wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK);
    wait_for_state(&core, WLH_COPROC_STATE_READY);
    wait_for_sent(&f, 1u);
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
    CHECK(frame_header.session_id == 0);
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
    CHECK(rpc.request_id == 7 && rpc.kind == WLH_RPC_KIND_RESPONSE);
    stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
    CHECK(pb_decode(&stream, wlh_protocol_v1_HelloResponse_fields, &response));
    CHECK(response.session_id == 42 && response.initial_credits_count == 4);

    {
        unsigned sent_before = f.sent_count;
        wlh_protocol_v1_WifiInitializeRequest init =
            wlh_protocol_v1_WifiInitializeRequest_init_zero;
        init.interface_flags = 1u;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            0,
            WLH_SERVICE_WIFI,
            WLH_WIFI_METHOD_INITIALIZE,
            8,
            wlh_protocol_v1_WifiInitializeRequest_fields,
            &init
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        {
            unsigned attempt;
            for (attempt = 0; attempt < 1000u && f.initialized != 1; ++attempt)
                wait_milliseconds(1u);
        }
        CHECK(f.initialized == 1);
    }

    {
        unsigned sent_before = f.sent_count;
        wlh_protocol_v1_DiagnosticsPingRequest ping =
            wlh_protocol_v1_DiagnosticsPingRequest_init_zero;
        wlh_protocol_v1_DiagnosticsPingResponse pong =
            wlh_protocol_v1_DiagnosticsPingResponse_init_zero;
        ping.cookie = 0x1234u;
        ping.host_time_us = 99u;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            1,
            WLH_SERVICE_DIAGNOSTICS,
            WLH_DIAGNOSTICS_METHOD_PING,
            9,
            wlh_protocol_v1_DiagnosticsPingRequest_fields,
            &ping
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
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
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(
            &stream, wlh_protocol_v1_DiagnosticsPingResponse_fields, &pong
        ));
        CHECK(
            pong.cookie == ping.cookie && pong.host_time_us == ping.host_time_us
        );
    }

    {
        uint8_t raw[11] = {1, 0, 8, 0, 3, 0, 0, 0, 1, 2, 3};
        wlh_frame_header_t header;
        wlh_protocol_v1_CreditUpdate update =
            wlh_protocol_v1_CreditUpdate_init_zero;
        size_t size = 0;
        unsigned sent_before = f.sent_count;
        wlh_frame_header_init(&header, WLH_CHANNEL_ETHERNET_STA);
        header.session_id = 42;
        CHECK(
            wlh_frame_encode(
                incoming, sizeof(incoming), &size, &header, raw, sizeof(raw)
            ) == WLH_WIRE_OK
        );
        CHECK(wlh_coproc_on_frame(&core, incoming, size) == WLH_COPROC_OK);
        {
            unsigned attempt;
            for (attempt = 0; attempt < 1000u && f.ethernet_size != 3u;
                 ++attempt)
                wait_milliseconds(1u);
        }
        CHECK(f.ethernet_size == 3);
        wait_for_sent(&f, sent_before + 1u);
        CHECK(
            wlh_frame_decode(
                &header,
                &frame_payload,
                &frame_payload_size,
                f.sent,
                f.sent_size,
                4096
            ) == WLH_WIRE_OK
        );
        CHECK(header.channel == WLH_CHANNEL_LINK_CONTROL);
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
        CHECK(
            update.channel_id == WLH_CHANNEL_ETHERNET_STA && update.units == 1u
        );
    }
    {
        uint8_t raw[11] = {1, 0, 8, 0, 3, 0, 0, 0, 4, 5, 6};
        wlh_frame_header_t header;
        size_t size = 0;
        unsigned sent_before = f.sent_count;
        wlh_frame_header_init(&header, WLH_CHANNEL_ETHERNET_AP);
        header.session_id = 42;
        CHECK(
            wlh_frame_encode(
                incoming, sizeof(incoming), &size, &header, raw, sizeof(raw)
            ) == WLH_WIRE_OK
        );
        CHECK(wlh_coproc_on_frame(&core, incoming, size) == WLH_COPROC_OK);
        {
            unsigned attempt;
            for (attempt = 0; attempt < 1000u && f.ethernet_ap_size != 3u;
                 ++attempt)
                wait_milliseconds(1u);
        }
        CHECK(f.ethernet_ap_size == 3u);
        CHECK(
            wlh_coproc_ethernet_ap_send(&core, raw + 8u, 3u) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 2u);
        CHECK(
            wlh_frame_decode(
                &header,
                &frame_payload,
                &frame_payload_size,
                f.sent,
                f.sent_size,
                4096
            ) == WLH_WIRE_OK
        );
        CHECK(header.channel == WLH_CHANNEL_ETHERNET_AP);
        check_raw_record(frame_payload, frame_payload_size, raw + 8u, 3u);
    }
    {
        /* The STA send path builds the same 8-byte raw record. Both are
           checked byte for byte: the header lives in a flexible array member,
           so a sizeof(*job)-sized memset leaves bytes 1 and 3 poisoned and the
           peer drops every frame, leaking a credit each time. */
        uint8_t payload[3] = {7, 8, 9};
        wlh_frame_header_t header;
        unsigned sent_before = f.sent_count;
        CHECK(
            wlh_coproc_ethernet_sta_send(&core, payload, sizeof(payload)) ==
            WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(
            wlh_frame_decode(
                &header,
                &frame_payload,
                &frame_payload_size,
                f.sent,
                f.sent_size,
                4096
            ) == WLH_WIRE_OK
        );
        CHECK(header.channel == WLH_CHANNEL_ETHERNET_STA);
        check_raw_record(
            frame_payload, frame_payload_size, payload, sizeof(payload)
        );
    }
    CHECK(wlh_coproc_stop(&core) == WLH_COPROC_OK);
}
