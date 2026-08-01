#include "coproc_internal.h"
#include "wlh/log.h"

#include <limits.h>
#include <stdio.h>
#include <string.h>

#include "adc.pb.h"
#include "bluetooth.pb.h"
#include "device_info.pb.h"
#include "diagnostics.pb.h"
#include "io.pb.h"
#include "kv.pb.h"
#include "link.pb.h"
#include "user_passthrough.pb.h"
#include "wifi.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>

static WLH_NOINLINE wlh_coproc_result_t
send_hello_response(wlh_coproc_t *coproc, uint32_t request_id) {
    wlh_protocol_v1_HelloResponse *response;
    size_t i;
    size_t credit_index;
    size_t service_index;
    size_t channel_index;
    const char *implementation_version;
    uint32_t selected_session;
    wlh_coproc_result_t result;

    response = (wlh_protocol_v1_HelloResponse *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*response)
    );
    if (response == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    memset(response, 0, sizeof(*response));

    selected_session = coproc->next_session_id++;
    if (selected_session == 0u) {
        selected_session = coproc->next_session_id++;
    }

    implementation_version = coproc->config.implementation_version[0] != '\0'
                                 ? coproc->config.implementation_version
                                 : "0.1.0";

    response->has_selected_protocol = true;
    response->selected_protocol.major = 1u;
    response->session_id = selected_session;
    response->boot_id = selected_session;
    memcpy(
        response->implementation, "wlh-coproc-core", sizeof("wlh-coproc-core")
    );
    (void)snprintf(
        response->implementation_version,
        sizeof(response->implementation_version),
        "%s",
        implementation_version
    );
    response->max_frame_size = coproc->config.max_frame_size;
    response->alignment = 1u;
    response->checksum_mode = wlh_protocol_v1_ChecksumMode_CHECKSUM_MODE_SUM32;

    for (i = 0; i < 4u; ++i) {
        response->initial_credits[i].channel_id = (uint32_t)i;
        response->initial_credits[i].units = coproc->config.initial_credit;
        response->initial_credits[i].unit_bytes = 1u;
        coproc->tx_credit[i] = coproc->config.initial_credit;
    }
    credit_index = 4u;
    service_index = 0u;
    channel_index = 0u;

    coproc->tx_credit[WLH_CHANNEL_BLUETOOTH_HCI] = 0u;
    coproc->tx_credit[WLH_CHANNEL_BLUETOOTH_HCI_ADV] = 0u;
    if (bluetooth_backend_present(coproc)) {
        response->services[service_index].service_id = WLH_SERVICE_BLUETOOTH;
        response->services[service_index].major = 1u;
        ++service_index;
        response->channels[channel_index].channel_id =
            WLH_CHANNEL_BLUETOOTH_HCI;
        response->channels[channel_index].max_frame_payload =
            WLH_COPROC_MAX_HCI_PACKET;
        response->channels[channel_index].alignment = 1u;
        ++channel_index;
        response->initial_credits[credit_index].channel_id =
            WLH_CHANNEL_BLUETOOTH_HCI;
        response->initial_credits[credit_index].units =
            WLH_COPROC_BLUETOOTH_INITIAL_CREDIT;
        response->initial_credits[credit_index].unit_bytes = 1u;
        ++credit_index;
        coproc->tx_credit[WLH_CHANNEL_BLUETOOTH_HCI] =
            WLH_COPROC_BLUETOOTH_INITIAL_CREDIT;
        if (coproc->bluetooth_adv_channel) {
            response->channels[channel_index].channel_id =
                WLH_CHANNEL_BLUETOOTH_HCI_ADV;
            response->channels[channel_index].max_frame_payload =
                WLH_COPROC_MAX_HCI_PACKET;
            response->channels[channel_index].alignment = 1u;
            ++channel_index;
            response->initial_credits[credit_index].channel_id =
                WLH_CHANNEL_BLUETOOTH_HCI_ADV;
            response->initial_credits[credit_index].units =
                WLH_COPROC_BLUETOOTH_ADV_INITIAL_CREDIT;
            response->initial_credits[credit_index].unit_bytes = 1u;
            ++credit_index;
            coproc->tx_credit[WLH_CHANNEL_BLUETOOTH_HCI_ADV] =
                WLH_COPROC_BLUETOOTH_ADV_INITIAL_CREDIT;
        }
    }

    coproc->tx_credit[WLH_CHANNEL_OTA_STREAM] = 0u;
    if (ota_backend_present(coproc)) {
        response->services[service_index].service_id = WLH_SERVICE_OTA;
        response->services[service_index].major = 1u;
        ++service_index;
        response->channels[channel_index].channel_id = WLH_CHANNEL_OTA_STREAM;
        response->channels[channel_index].max_frame_payload =
            WLH_COPROC_OTA_STREAM_HEADER_SIZE + WLH_COPROC_OTA_CHUNK_SIZE;
        response->channels[channel_index].alignment = WLH_COPROC_OTA_ALIGNMENT;
        ++channel_index;
        response->initial_credits[credit_index].channel_id =
            WLH_CHANNEL_OTA_STREAM;
        response->initial_credits[credit_index].units =
            WLH_COPROC_OTA_INITIAL_CREDIT;
        response->initial_credits[credit_index].unit_bytes = 1u;
        ++credit_index;
        coproc->tx_credit[WLH_CHANNEL_OTA_STREAM] =
            WLH_COPROC_OTA_INITIAL_CREDIT;
    }

    response->services_count = (pb_size_t)service_index;
    response->channels_count = (pb_size_t)channel_index;
    response->initial_credits_count = (pb_size_t)credit_index;

    /* Negotiation frames use session 0. The selected session takes effect only
       after the complete HelloResponse has been sent. */
    coproc->session_id = 0u;
    result = send_rpc_message(
        coproc,
        WLH_SERVICE_LINK,
        WLH_LINK_METHOD_HELLO,
        request_id,
        WLH_RPC_KIND_RESPONSE,
        WLH_STATUS_DOMAIN_NONE,
        WLH_STATUS_OK,
        wlh_protocol_v1_HelloResponse_fields,
        response
    );
    coproc->config.buffers.free(
        coproc->config.buffers.context, (uint8_t *)response
    );
    if (result == WLH_COPROC_OK) {
        coproc->session_id = selected_session;
        WLH_LOGI(
            "wlh_coproc",
            "negotiated session %lu",
            (unsigned long)selected_session
        );
        set_state(coproc, WLH_COPROC_STATE_READY);
    }
    return result;
}

WLH_NOINLINE wlh_coproc_result_t handle_hello_request(
    wlh_coproc_t *coproc,
    const wlh_frame_header_t *frame_header,
    const wlh_rpc_envelope_t *request,
    const uint8_t *message,
    size_t message_size
) {
    wlh_protocol_v1_HelloRequest *hello;
    pb_istream_t stream;
    size_t i;
    bool supports_v1 = false;
    wlh_coproc_result_t result;

    hello = (wlh_protocol_v1_HelloRequest *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*hello)
    );
    if (hello == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    memset(hello, 0, sizeof(*hello));
    stream = pb_istream_from_buffer(message, message_size);
    if (frame_header->session_id != 0u ||
        !pb_decode(&stream, wlh_protocol_v1_HelloRequest_fields, hello)) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)hello
        );
        return WLH_COPROC_PROTOCOL_ERROR;
    }
    for (i = 0; i < hello->protocol_versions_count; ++i) {
        if (hello->protocol_versions[i].major == 1u)
            supports_v1 = true;
    }
    coproc->bluetooth_adv_channel = false;
    for (i = 0; i < hello->channels_count; ++i) {
        if (hello->channels[i].channel_id == WLH_CHANNEL_BLUETOOTH_HCI_ADV)
            coproc->bluetooth_adv_channel = true;
    }
    if (!supports_v1 || hello->max_frame_size < WLH_FRAME_HEADER_SIZE) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)hello
        );
        return send_rpc(
            coproc,
            WLH_SERVICE_LINK,
            WLH_LINK_METHOD_HELLO,
            request->request_id,
            WLH_RPC_KIND_RESPONSE,
            WLH_STATUS_DOMAIN_PROTOCOL,
            WLH_STATUS_VERSION_MISMATCH,
            NULL,
            0u
        );
    }
    coproc->config.buffers.free(
        coproc->config.buffers.context, (uint8_t *)hello
    );
    memset(coproc->tx_sequence, 0, sizeof(coproc->tx_sequence));
    memset(coproc->rx_sequence_valid, 0, sizeof(coproc->rx_sequence_valid));
    memset(
        &coproc->wifi_initialize_pending,
        0,
        sizeof(coproc->wifi_initialize_pending)
    );
    memset(&coproc->bluetooth_pending, 0, sizeof(coproc->bluetooth_pending));
    coproc->bluetooth_state = BT_STATE_UNSPECIFIED;
    coproc->bluetooth_tx_inflight = 0u;
    coproc->bluetooth_adv_tx_inflight = 0u;
    coproc->bluetooth_hci_stopped = false;
    ota_reset(coproc);
    reset_ethernet_rx_completions(coproc);
    result = send_hello_response(coproc, request->request_id);
    return result;
}

WLH_NOINLINE wlh_coproc_result_t
coproc_emit_due_heartbeat(wlh_coproc_t *coproc) {
    wlh_protocol_v1_Heartbeat *heartbeat;
    uint64_t now;

    if (coproc == NULL) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }

    now = now_ms(coproc);
    if (coproc->state == WLH_COPROC_STATE_READY &&
        now - coproc->last_heartbeat_ms >=
            coproc->config.heartbeat_interval_ms) {
        heartbeat = (wlh_protocol_v1_Heartbeat *)coproc->config.buffers.alloc(
            coproc->config.buffers.context, sizeof(*heartbeat)
        );
        if (heartbeat != NULL) {
            memset(heartbeat, 0, sizeof(*heartbeat));
            heartbeat->session_id = coproc->session_id;
            heartbeat->state = wlh_protocol_v1_LinkState_LINK_STATE_HEALTHY;
            heartbeat->uptime_ms = now - coproc->started_ms;
            heartbeat->monotonic_ms = now;
            (void)send_rpc_message(
                coproc,
                WLH_SERVICE_LINK,
                WLH_LINK_METHOD_HEARTBEAT,
                0u,
                WLH_RPC_KIND_EVENT,
                0u,
                0,
                wlh_protocol_v1_Heartbeat_fields,
                heartbeat
            );
            coproc->config.buffers.free(
                coproc->config.buffers.context, (uint8_t *)heartbeat
            );
        }
        coproc->last_heartbeat_ms = now;
    }
    return WLH_COPROC_OK;
}

uint32_t coproc_next_wait_ms(const wlh_coproc_t *coproc) {
    uint64_t current;
    uint64_t deadline;
    uint64_t remaining;

    if (coproc->state != WLH_COPROC_STATE_READY)
        return WLH_OSAL_WAIT_FOREVER;
    current = now_ms(coproc);
    deadline = coproc->last_heartbeat_ms + coproc->config.heartbeat_interval_ms;
    if (deadline <= current)
        return WLH_OSAL_NO_WAIT;
    remaining = deadline - current;
    if (remaining >= WLH_OSAL_WAIT_FOREVER)
        return WLH_OSAL_WAIT_FOREVER - 1u;
    return (uint32_t)remaining;
}
