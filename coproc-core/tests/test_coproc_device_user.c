#include "coproc_test_support.h"

void test_device_info_and_user_passthrough(void) {
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
    memcpy(f.device_info.vendor, "espressif", sizeof("espressif"));
    memcpy(f.device_info.mcu_model, "ESP32-S3", sizeof("ESP32-S3"));
    f.device_info.uid_size = 6u;
    memcpy(f.device_info.uid, "\x01\x02\x03\x04\x05\x06", 6u);
    memcpy(
        f.device_info.board_profile,
        "espressif.esp32s3.coreboard.usb-wifi",
        sizeof("espressif.esp32s3.coreboard.usb-wifi")
    );
    prepare_ready_core(&f, &core, true);

    /* GET_INFO returns the configured fields. */
    sent_before = f.sent_count;
    {
        wlh_protocol_v1_Empty empty = wlh_protocol_v1_Empty_init_zero;
        wlh_protocol_v1_DeviceInfoResponse info =
            wlh_protocol_v1_DeviceInfoResponse_init_zero;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            0,
            WLH_SERVICE_DEVICE_INFO,
            WLH_DEVICE_INFO_METHOD_GET_INFO,
            20,
            wlh_protocol_v1_Empty_fields,
            &empty
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.device_info_queries == 1);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.request_id == 20 && rpc.kind == WLH_RPC_KIND_RESPONSE &&
            rpc.status_code == WLH_STATUS_OK
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(
            pb_decode(&stream, wlh_protocol_v1_DeviceInfoResponse_fields, &info)
        );
        CHECK(strcmp(info.vendor, "espressif") == 0);
        CHECK(strcmp(info.mcu_model, "ESP32-S3") == 0);
        CHECK(
            info.uid.size == 6u && info.uid.bytes[0] == 1u &&
            info.uid.bytes[5] == 6u
        );
        CHECK(
            strcmp(
                info.board_profile, "espressif.esp32s3.coreboard.usb-wifi"
            ) == 0
        );
    }

    /* GET_INFO propagates a backend failure as DEVICE_INFO domain error. */
    sent_before = f.sent_count;
    f.device_info_status = -1;
    {
        wlh_protocol_v1_Empty empty = wlh_protocol_v1_Empty_init_zero;
        incoming_size = make_rpc_frame(
            incoming,
            42,
            1,
            WLH_SERVICE_DEVICE_INFO,
            WLH_DEVICE_INFO_METHOD_GET_INFO,
            21,
            wlh_protocol_v1_Empty_fields,
            &empty
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.status_domain == WLH_STATUS_DOMAIN_DEVICE_INFO &&
            rpc.status_code == WLH_STATUS_INTERNAL
        );
    }
    f.device_info_status = 0;

    /* SEND dispatches to the adapter and is acked with OK. */
    sent_before = f.sent_count;
    {
        static const uint8_t payload[] = "hello";
        wlh_protocol_v1_UserMessageSendRequest send_request =
            wlh_protocol_v1_UserMessageSendRequest_init_zero;
        send_request.endpoint_id = 7;
        send_request.message_type = 3;
        send_request.flags = 1;
        send_request.payload.size = sizeof(payload) - 1u;
        memcpy(send_request.payload.bytes, payload, sizeof(payload) - 1u);
        incoming_size = make_rpc_frame(
            incoming,
            42,
            2,
            WLH_SERVICE_USER_PASSTHROUGH,
            WLH_USER_PASSTHROUGH_METHOD_SEND,
            22,
            wlh_protocol_v1_UserMessageSendRequest_fields,
            &send_request
        );
        CHECK(
            wlh_coproc_on_frame(&core, incoming, incoming_size) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        CHECK(f.user_messages == 1u);
        CHECK(
            f.last_user_message.endpoint_id == 7 &&
            f.last_user_message.message_type == 3 &&
            f.last_user_message.flags == 1 &&
            f.last_user_message.request_id == 22
        );
        CHECK(
            f.last_user_message.payload_size == sizeof(payload) - 1u &&
            memcmp(
                f.last_user_message.payload, payload, sizeof(payload) - 1u
            ) == 0
        );
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.request_id == 22 && rpc.kind == WLH_RPC_KIND_RESPONSE &&
            rpc.status_code == WLH_STATUS_OK
        );
    }

    /* wlh_coproc_user_message_result emits a RESULT event. */
    sent_before = f.sent_count;
    {
        static const uint8_t result_payload[] = "done";
        wlh_protocol_v1_UserMessageResultEvent event =
            wlh_protocol_v1_UserMessageResultEvent_init_zero;
        CHECK(
            wlh_coproc_user_message_result(
                &core, 7, 3, 22, -5, result_payload, sizeof(result_payload) - 1u
            ) == WLH_COPROC_OK
        );
        wait_for_sent(&f, sent_before + 1u);
        decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
        CHECK(
            rpc.service_id == WLH_SERVICE_USER_PASSTHROUGH &&
            rpc.method_id == WLH_USER_PASSTHROUGH_EVENT_RESULT &&
            rpc.kind == WLH_RPC_KIND_EVENT
        );
        stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
        CHECK(pb_decode(
            &stream, wlh_protocol_v1_UserMessageResultEvent_fields, &event
        ));
        CHECK(
            event.endpoint_id == 7 && event.message_type == 3 &&
            event.correlation_id == 22 && event.result == -5 &&
            event.payload.size == sizeof(result_payload) - 1u &&
            memcmp(
                event.payload.bytes, result_payload, sizeof(result_payload) - 1u
            ) == 0
        );
    }
    CHECK(wlh_coproc_stop(&core) == WLH_COPROC_OK);
}
