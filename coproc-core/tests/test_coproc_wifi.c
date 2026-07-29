#include "coproc_test_support.h"

void test_softap(void) {
    fixture_t f;
    wlh_coproc_t core;
    uint8_t incoming[4096];
    size_t incoming_size;
    wlh_rpc_envelope_t rpc;
    const uint8_t *rpc_payload;
    size_t rpc_payload_size;
    pb_istream_t stream;
    unsigned sent_before;

    memset(&f, 0, sizeof(f));
    f.core = &core;
    wlh_posix_osal_init(&f.posix);
    prepare_ready_core(&f, &core, true);

    /* START_AP dispatches to the backend op and is acked with OK. */
    sent_before = f.sent_count;
    {
        static const uint8_t ssid[] = "wlh-test-ap";
        static const uint8_t credential[] = "test-password";
        wlh_protocol_v1_WifiStartApRequest start =
            wlh_protocol_v1_WifiStartApRequest_init_zero;
        start.ssid.size = sizeof(ssid) - 1u;
        memcpy(start.ssid.bytes, ssid, sizeof(ssid) - 1u);
        start.credential.size = sizeof(credential) - 1u;
        memcpy(start.credential.bytes, credential, sizeof(credential) - 1u);
        start.security = wlh_protocol_v1_WifiSecurity_WIFI_SECURITY_WPA2_PSK;
        start.channel = 6;
        start.max_clients = 4;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            0,
            WLH_SERVICE_WIFI,
            WLH_WIFI_METHOD_START_AP,
            40,
            wlh_protocol_v1_WifiStartApRequest_fields,
            &start
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.start_ap_calls == 1u);
        CHECK(
            f.last_ap_request.ssid_size == sizeof(ssid) - 1u &&
            memcmp(f.last_ap_request.ssid, ssid, sizeof(ssid) - 1u) == 0
        );
        CHECK(
            f.last_ap_request.credential_size == sizeof(credential) - 1u &&
            memcmp(
                f.last_ap_request.credential,
                credential,
                sizeof(credential) - 1u
            ) == 0
        );
        CHECK(
            f.last_ap_request.security ==
                (uint32_t)wlh_protocol_v1_WifiSecurity_WIFI_SECURITY_WPA2_PSK &&
            f.last_ap_request.channel == 6u &&
            f.last_ap_request.max_clients == 4u
        );
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.request_id == 40 && rpc.kind == WLH_RPC_KIND_RESPONSE &&
            rpc.status_domain == WLH_STATUS_DOMAIN_NONE &&
            rpc.status_code == WLH_STATUS_OK
        );
    }

    /* STOP_AP dispatches to the backend op and is acked with OK. */
    sent_before = f.sent_count;
    {
        wlh_protocol_v1_Empty empty = wlh_protocol_v1_Empty_init_zero;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            1,
            WLH_SERVICE_WIFI,
            WLH_WIFI_METHOD_STOP_AP,
            41,
            wlh_protocol_v1_Empty_fields,
            &empty
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.stop_ap_calls == 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.request_id == 41 && rpc.kind == WLH_RPC_KIND_RESPONSE &&
            rpc.status_domain == WLH_STATUS_DOMAIN_NONE &&
            rpc.status_code == WLH_STATUS_OK
        );
    }

    /* Scan result ingress emits an encoded WIFI event. */
    sent_before = f.sent_count;
    {
        static const uint8_t ssid[] = "wlh-scan";
        static const uint8_t bssid[6] = {0x02, 0x00, 0x00, 0x00, 0x00, 0x01};
        wlh_coproc_bss_t bss;
        wlh_protocol_v1_WifiScanResultEvent event =
            wlh_protocol_v1_WifiScanResultEvent_init_zero;

        memset(&bss, 0, sizeof(bss));
        bss.ssid = ssid;
        bss.ssid_size = sizeof(ssid) - 1u;
        memcpy(bss.bssid, bssid, sizeof(bssid));
        bss.security =
            (uint32_t)wlh_protocol_v1_WifiSecurity_WIFI_SECURITY_WPA2_PSK;
        bss.channel = 6u;
        bss.rssi_dbm = -42;

        CHECK(wlh_coproc_wifi_scan_result(&core, 11u, &bss) == WLH_COPROC_OK);
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.service_id == WLH_SERVICE_WIFI &&
            rpc.method_id == WLH_WIFI_EVENT_SCAN_RESULT &&
            rpc.kind == WLH_RPC_KIND_EVENT
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(
            &stream, wlh_protocol_v1_WifiScanResultEvent_fields, &event
        ));
        CHECK(
            event.scan_id == 11u && event.networks_count == 1u &&
            event.networks[0].ssid.size == sizeof(ssid) - 1u &&
            memcmp(event.networks[0].ssid.bytes, ssid, sizeof(ssid) - 1u) ==
                0 &&
            event.networks[0].bssid.size == sizeof(bssid) &&
            memcmp(event.networks[0].bssid.bytes, bssid, sizeof(bssid)) == 0 &&
            event.networks[0].security ==
                wlh_protocol_v1_WifiSecurity_WIFI_SECURITY_WPA2_PSK &&
            event.networks[0].channel == 6u && event.networks[0].rssi_dbm == -42
        );
    }

    /* Connected ingress includes the STA interface MAC for Host networking. */
    sent_before = f.sent_count;
    {
        static const uint8_t ssid[] = "wlh-link";
        static const uint8_t bssid[6] = {0x02, 0, 0, 0, 0, 2};
        static const uint8_t interface_mac[6] = {
            0x24, 0x6f, 0x28, 0xaa, 0xbb, 0xcc
        };
        wlh_coproc_bss_t bss;
        wlh_protocol_v1_WifiConnectedEvent event =
            wlh_protocol_v1_WifiConnectedEvent_init_zero;

        memset(&bss, 0, sizeof(bss));
        bss.ssid = ssid;
        bss.ssid_size = sizeof(ssid) - 1u;
        memcpy(bss.bssid, bssid, sizeof(bssid));
        memcpy(bss.interface_mac, interface_mac, sizeof(bss.interface_mac));
        bss.security =
            (uint32_t)wlh_protocol_v1_WifiSecurity_WIFI_SECURITY_WPA2_PSK;
        bss.channel = 6u;
        bss.rssi_dbm = -40;

        CHECK(wlh_coproc_wifi_connected(&core, &bss) == WLH_COPROC_OK);
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.service_id == WLH_SERVICE_WIFI &&
            rpc.method_id == WLH_WIFI_EVENT_CONNECTED &&
            rpc.kind == WLH_RPC_KIND_EVENT
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(
            &stream, wlh_protocol_v1_WifiConnectedEvent_fields, &event
        ));
        CHECK(
            event.has_link && event.link.mac.size == sizeof(interface_mac) &&
            memcmp(
                event.link.mac.bytes, interface_mac, sizeof(interface_mac)
            ) == 0
        );
    }

    /* AP client joined ingress emits a WIFI event. */
    sent_before = f.sent_count;
    {
        static const uint8_t mac[6] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x01};
        wlh_protocol_v1_WifiApClientJoinedEvent joined =
            wlh_protocol_v1_WifiApClientJoinedEvent_init_zero;
        CHECK(
            wlh_coproc_wifi_ap_client_joined(&core, mac, -55, 7) ==
            WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.service_id == WLH_SERVICE_WIFI &&
            rpc.method_id == WLH_WIFI_EVENT_AP_CLIENT_JOINED &&
            rpc.kind == WLH_RPC_KIND_EVENT
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(
            &stream, wlh_protocol_v1_WifiApClientJoinedEvent_fields, &joined
        ));
        CHECK(
            joined.client.mac.size == 6u &&
            memcmp(joined.client.mac.bytes, mac, 6u) == 0 &&
            joined.client.rssi_dbm == -55 && joined.client.association_id == 7u
        );
    }

    /* AP client left ingress emits a WIFI event. */
    sent_before = f.sent_count;
    {
        static const uint8_t mac[6] = {0xde, 0xad, 0xbe, 0xef, 0x00, 0x01};
        wlh_protocol_v1_WifiApClientLeftEvent left =
            wlh_protocol_v1_WifiApClientLeftEvent_init_zero;
        CHECK(
            wlh_coproc_wifi_ap_client_left(&core, mac, 7, 3) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.service_id == WLH_SERVICE_WIFI &&
            rpc.method_id == WLH_WIFI_EVENT_AP_CLIENT_LEFT &&
            rpc.kind == WLH_RPC_KIND_EVENT
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(
            &stream, wlh_protocol_v1_WifiApClientLeftEvent_fields, &left
        ));
        CHECK(
            left.mac.size == 6u && memcmp(left.mac.bytes, mac, 6u) == 0 &&
            left.association_id == 7u && left.ieee80211_reason == 3u
        );
    }
    CHECK(wlh_coproc_stop(&core) == WLH_COPROC_OK);
}

void test_oversized_ssid_rejected(void) {
    static uint8_t ssid[200];
    wlh_coproc_t core;
    wlh_coproc_bss_t bss;
    failing_allocator_t allocator;

    memset(ssid, 'A', sizeof(ssid));
    memset(&core, 0, sizeof(core));
    memset(&bss, 0, sizeof(bss));
    memset(&allocator, 0, sizeof(allocator));
    core.config.buffers = (wlh_coproc_buffer_ops_t){&allocator,
                                                    failing_buffer_alloc,
                                                    failing_buffer_free};
    bss.ssid = ssid;
    bss.ssid_size = sizeof(ssid);

    CHECK(
        wlh_coproc_wifi_scan_result(&core, 1u, &bss) ==
        WLH_COPROC_INVALID_ARGUMENT
    );
    CHECK(
        wlh_coproc_wifi_connected(&core, &bss) == WLH_COPROC_INVALID_ARGUMENT
    );
    CHECK(
        wlh_coproc_wifi_ap_started(&core, &bss) == WLH_COPROC_INVALID_ARGUMENT
    );
    /* Rejected before any allocation. */
    CHECK(allocator.attempts == 0u && allocator.outstanding == 0u);

    /* A non-NULL ssid is required whenever ssid_size is non-zero. */
    bss.ssid = NULL;
    bss.ssid_size = 8u;
    CHECK(
        wlh_coproc_wifi_scan_result(&core, 1u, &bss) ==
        WLH_COPROC_INVALID_ARGUMENT
    );
    CHECK(
        wlh_coproc_wifi_connected(&core, &bss) == WLH_COPROC_INVALID_ARGUMENT
    );
    CHECK(
        wlh_coproc_wifi_ap_started(&core, &bss) == WLH_COPROC_INVALID_ARGUMENT
    );

    /* The exact schema bound is still accepted: it reaches the allocator. */
    bss.ssid = ssid;
    bss.ssid_size = 32u;
    allocator.fail_at = 1u;
    CHECK(
        wlh_coproc_wifi_scan_result(&core, 1u, &bss) == WLH_COPROC_BACKEND_ERROR
    );
    CHECK(allocator.attempts == 1u && allocator.outstanding == 0u);
}

void test_large_message_allocation_failures(void) {
    static const uint8_t ssid[] = "allocation-test";
    wlh_coproc_t core;
    wlh_coproc_bss_t bss;
    failing_allocator_t allocator;

    memset(&core, 0, sizeof(core));
    memset(&bss, 0, sizeof(bss));
    memset(&allocator, 0, sizeof(allocator));
    core.config.buffers = (wlh_coproc_buffer_ops_t){&allocator,
                                                    failing_buffer_alloc,
                                                    failing_buffer_free};
    bss.ssid = ssid;
    bss.ssid_size = sizeof(ssid) - 1u;

    allocator.fail_at = 1u;
    CHECK(
        wlh_coproc_wifi_scan_result(&core, 1u, &bss) == WLH_COPROC_BACKEND_ERROR
    );
    CHECK(allocator.outstanding == 0u);

    allocator.attempts = 0u;
    allocator.fail_at = 2u;
    CHECK(
        wlh_coproc_wifi_scan_result(&core, 1u, &bss) == WLH_COPROC_BACKEND_ERROR
    );
    CHECK(allocator.outstanding == 0u);
}
