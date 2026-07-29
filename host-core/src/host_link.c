#include "host_internal.h"
#include "wlh/log.h"

#include <limits.h>
#include <string.h>

#include "bluetooth.pb.h"
#include "link.pb.h"
#include "user_passthrough.pb.h"
#include "wifi.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>

wlh_host_result_t send_hello(wlh_host_t *host) {
    wlh_protocol_v1_HelloRequest *hello;
    wlh_rpc_envelope_t envelope;
    wlh_host_result_t result;

    hello = (wlh_protocol_v1_HelloRequest *)host->config.buffers.alloc(
        host->config.buffers.context, sizeof(*hello)
    );
    if (hello == NULL)
        return WLH_HOST_NO_MEMORY;
    memset(hello, 0, sizeof(*hello));

    hello->protocol_versions_count = 1u;
    hello->protocol_versions[0].major = 1u;
    hello->protocol_versions[0].min_minor = 0u;
    hello->protocol_versions[0].max_minor = 0u;
    memcpy(
        hello->implementation,
        "wl-hosted-host-core",
        sizeof("wl-hosted-host-core")
    );
    memcpy(hello->implementation_version, "0.1.0", sizeof("0.1.0"));
    hello->max_frame_size = host->config.max_frame_size;
    hello->alignment = 1u;
    hello->checksum_modes_count = 2u;
    hello->checksum_modes[0] = wlh_protocol_v1_ChecksumMode_CHECKSUM_MODE_SUM32;
    hello->checksum_modes[1] =
        wlh_protocol_v1_ChecksumMode_CHECKSUM_MODE_CRC32C;
    hello->max_rpc_payload = WLH_HOST_PROTOBUF_LIMIT;
    hello->services_count = 5u;
    // clang-format off
    hello->services[0] = (wlh_protocol_v1_ServiceVersionRange){
        WLH_SERVICE_LINK, 1u, 0u, 0u,
    };

    hello->services[1] = (wlh_protocol_v1_ServiceVersionRange){
        WLH_SERVICE_WIFI, 1u, 0u, 0u,
    };

    hello->services[2] = (wlh_protocol_v1_ServiceVersionRange){
        WLH_SERVICE_DIAGNOSTICS, 1u, 0u, 0u,
    };

    hello->services[3] = (wlh_protocol_v1_ServiceVersionRange){
        WLH_SERVICE_BLUETOOTH, 1u, 0u, 0u,
    };
    hello->services[4] = (wlh_protocol_v1_ServiceVersionRange){
        WLH_SERVICE_OTA, 1u, 0u, 0u,
    };

    hello->channels_count = 7u;
    hello->channels[0] = (wlh_protocol_v1_ChannelCapability){
        WLH_CHANNEL_LINK_CONTROL, WLH_HOST_PROTOBUF_LIMIT, 0u, 1u, 0u,
    };

    hello->channels[1] = (wlh_protocol_v1_ChannelCapability){
        WLH_CHANNEL_CONTROL_RPC, WLH_HOST_PROTOBUF_LIMIT, 0u, 1u, 0u,
    };

    hello->channels[2] = (wlh_protocol_v1_ChannelCapability){
        WLH_CHANNEL_ETHERNET_STA, 1600u, 0u, 1u, 0u,
    };

    hello->channels[3] = (wlh_protocol_v1_ChannelCapability){
        WLH_CHANNEL_ETHERNET_AP, 1600u, 0u, 1u, 0u,
    };

    hello->channels[4] = (wlh_protocol_v1_ChannelCapability){
        WLH_CHANNEL_BLUETOOTH_HCI, WLH_HOST_MAX_HCI_PACKET, 0u, 1u, 0u,
    };

    hello->channels[5] = (wlh_protocol_v1_ChannelCapability){
        WLH_CHANNEL_BLUETOOTH_HCI_ADV, WLH_HOST_MAX_HCI_PACKET, 0u, 1u, 0u,
    };
    hello->channels[6] = (wlh_protocol_v1_ChannelCapability){
        WLH_CHANNEL_OTA_STREAM, 4096u, 0u, 1u, 0u,
    };
    // clang-format on
    memset(&envelope, 0, sizeof(envelope));
    envelope.service_id = WLH_SERVICE_LINK;
    envelope.method_id = WLH_LINK_METHOD_HELLO;
    envelope.request_id = host->next_request_id++;
    if (envelope.request_id == 0u) {
        envelope.request_id = host->next_request_id++;
    }
    envelope.kind = WLH_RPC_KIND_REQUEST;
    set_state(host, WLH_HOST_STATE_NEGOTIATING);
    result = send_rpc_message(
        host, &envelope, wlh_protocol_v1_HelloRequest_fields, hello, true
    );
    host->config.buffers.free(host->config.buffers.context, (uint8_t *)hello);
    return result;
}

wlh_host_result_t handle_hello_response(
    wlh_host_t *host, const uint8_t *payload, size_t payload_size
) {
    wlh_protocol_v1_HelloResponse *hello;
    pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
    size_t index;
    hello = (wlh_protocol_v1_HelloResponse *)host->config.buffers.alloc(
        host->config.buffers.context, sizeof(*hello)
    );
    if (hello == NULL)
        return WLH_HOST_NO_MEMORY;
    memset(hello, 0, sizeof(*hello));
    if (!pb_decode(&stream, wlh_protocol_v1_HelloResponse_fields, hello) ||
        !hello->has_selected_protocol || hello->selected_protocol.major != 1u ||
        hello->session_id == 0u ||
        hello->max_frame_size < WLH_FRAME_HEADER_SIZE) {
        host->config.buffers.free(
            host->config.buffers.context, (uint8_t *)hello
        );
        set_state(host, WLH_HOST_STATE_FAILED);
        return WLH_HOST_PROTOCOL_ERROR;
    }
    host->session_id = hello->session_id;
    memcpy(
        host->peer_version,
        hello->implementation_version,
        sizeof(host->peer_version)
    );
    host->peer_version[sizeof(host->peer_version) - 1u] = '\0';
    host->diagnostics.session_id = hello->session_id;
    WLH_LOGI(
        "wlh_host", "negotiated session %lu", (unsigned long)hello->session_id
    );
    memset(host->tx_credit, 0, sizeof(host->tx_credit));
    for (index = 0; index < hello->initial_credits_count; ++index) {
        if (hello->initial_credits[index].channel_id < WLH_HOST_CHANNEL_COUNT) {
            host->tx_credit[hello->initial_credits[index].channel_id] =
                hello->initial_credits[index].units;
        }
    }
    {
        bool service_found = false;
        bool channel_found = false;
        uint32_t frame_limit = hello->max_frame_size;
        uint32_t record_limit = WLH_HOST_MAX_HCI_PACKET;
        for (index = 0; index < hello->services_count; ++index) {
            if (hello->services[index].service_id == WLH_SERVICE_BLUETOOTH)
                service_found = true;
        }
        for (index = 0; index < hello->channels_count; ++index) {
            if (hello->channels[index].channel_id ==
                WLH_CHANNEL_BLUETOOTH_HCI) {
                channel_found = true;
                if (hello->channels[index].max_frame_payload != 0u &&
                    hello->channels[index].max_frame_payload < record_limit)
                    record_limit = hello->channels[index].max_frame_payload;
            }
        }
        if (frame_limit > host->config.max_frame_size)
            frame_limit = host->config.max_frame_size;
        if (frame_limit >= WLH_FRAME_HEADER_SIZE + WLH_RAW_RECORD_HEADER_SIZE) {
            uint32_t payload_limit = frame_limit - WLH_FRAME_HEADER_SIZE -
                                     (uint32_t)WLH_RAW_RECORD_HEADER_SIZE;
            if (payload_limit < record_limit)
                record_limit = payload_limit;
        } else {
            channel_found = false;
        }
        host->bluetooth_supported = service_found && channel_found;
        host->bluetooth_max_record = record_limit;
        host->bluetooth_hci_stopped = false;
        host->bluetooth_state = WLH_BLUETOOTH_STATE_UNSPECIFIED;
    }
    {
        bool service_found = false;
        bool channel_found = false;
        uint32_t record_limit = 4096u;
        for (index = 0; index < hello->services_count; ++index)
            if (hello->services[index].service_id == WLH_SERVICE_OTA)
                service_found = true;
        for (index = 0; index < hello->channels_count; ++index) {
            if (hello->channels[index].channel_id == WLH_CHANNEL_OTA_STREAM) {
                channel_found = true;
                if (hello->channels[index].max_frame_payload != 0u)
                    record_limit = hello->channels[index].max_frame_payload;
            }
        }
        if (record_limit > host->config.max_frame_size - WLH_FRAME_HEADER_SIZE -
                               WLH_RAW_RECORD_HEADER_SIZE)
            record_limit = host->config.max_frame_size - WLH_FRAME_HEADER_SIZE -
                           WLH_RAW_RECORD_HEADER_SIZE;
        host->ota_supported =
            service_found && channel_found && record_limit >= 16u;
        host->ota_max_record = record_limit;
    }
    host->config.buffers.free(host->config.buffers.context, (uint8_t *)hello);
    host->diagnostics.last_peer_activity_ms = now_ms(host);
    set_state(host, WLH_HOST_STATE_READY);
    return WLH_HOST_OK;
}

static void handle_credit_update(
    wlh_host_t *host, const uint8_t *payload, size_t payload_size
) {
    wlh_protocol_v1_CreditUpdate credit =
        wlh_protocol_v1_CreditUpdate_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
    if (pb_decode(&stream, wlh_protocol_v1_CreditUpdate_fields, &credit) &&
        credit.channel_id < WLH_HOST_CHANNEL_COUNT) {
        uint32_t old = host->tx_credit[credit.channel_id];
        host->tx_credit[credit.channel_id] =
            UINT32_MAX - old < credit.units ? UINT32_MAX : old + credit.units;
        if (credit.channel_id == WLH_CHANNEL_BLUETOOTH_HCI &&
            host->config.bluetooth_hci_tx_ready != NULL) {
            uint32_t inflight = host->bluetooth_tx_inflight;
            uint32_t updated = host->tx_credit[credit.channel_id];
            bool was_usable = old > inflight;
            bool now_usable = updated > inflight;
            if (!was_usable && now_usable)
                host->config.bluetooth_hci_tx_ready(
                    host->config.bluetooth_context
                );
        }
        if (credit.channel_id == WLH_CHANNEL_OTA_STREAM &&
            host->config.ota_stream_tx_ready != NULL) {
            uint32_t inflight = host->ota_tx_inflight;
            bool was_usable = old > inflight;
            bool now_usable = host->tx_credit[credit.channel_id] > inflight;
            if (!was_usable && now_usable)
                host->config.ota_stream_tx_ready(host->config.ota_context);
        }
        if (host->state == WLH_HOST_STATE_CONGESTED)
            set_state(host, WLH_HOST_STATE_READY);
    }
}

static void handle_heartbeat(
    wlh_host_t *host, const uint8_t *payload, size_t payload_size
) {
    wlh_protocol_v1_Heartbeat *heartbeat;
    pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
    heartbeat = (wlh_protocol_v1_Heartbeat *)host->config.buffers.alloc(
        host->config.buffers.context, sizeof(*heartbeat)
    );
    if (heartbeat == NULL)
        return;
    memset(heartbeat, 0, sizeof(*heartbeat));
    if (pb_decode(&stream, wlh_protocol_v1_Heartbeat_fields, heartbeat) &&
        heartbeat->session_id == host->session_id) {
        host->diagnostics.last_peer_activity_ms = now_ms(host);
    }
    host->config.buffers.free(
        host->config.buffers.context, (uint8_t *)heartbeat
    );
}

static void handle_session_changed(
    wlh_host_t *host, const uint8_t *payload, size_t payload_size
) {
    wlh_protocol_v1_SessionChangedEvent *changed;
    pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
    uint32_t new_session_id;
    changed = (wlh_protocol_v1_SessionChangedEvent *)host->config.buffers.alloc(
        host->config.buffers.context, sizeof(*changed)
    );
    if (changed == NULL)
        return;
    memset(changed, 0, sizeof(*changed));
    if (!pb_decode(
            &stream, wlh_protocol_v1_SessionChangedEvent_fields, changed
        ) ||
        changed->new_session_id == 0u ||
        changed->new_session_id == host->session_id) {
        host->config.buffers.free(
            host->config.buffers.context, (uint8_t *)changed
        );
        return;
    }
    new_session_id = changed->new_session_id;
    (void)new_session_id;
    host->config.buffers.free(host->config.buffers.context, (uint8_t *)changed);
    host->diagnostics.peer_resets++;
    WLH_LOGW(
        "wlh_host",
        "peer requested session change %lu -> %lu",
        (unsigned long)host->session_id,
        (unsigned long)new_session_id
    );
    cancel_pending(host, WLH_HOST_SESSION_CHANGED);
    host->session_id = 0u;
    memset(host->tx_credit, 0, sizeof(host->tx_credit));
    memset(host->rx_sequence_valid, 0, sizeof(host->rx_sequence_valid));
    host->bluetooth_supported = false;
    host->bluetooth_hci_stopped = false;
    host->bluetooth_tx_inflight = 0u;
    host->bluetooth_state = WLH_BLUETOOTH_STATE_UNSPECIFIED;
    host->ota_supported = false;
    host->ota_tx_inflight = 0u;
    host->peer_version[0] = '\0';
    (void)send_hello(host);
}

void handle_link_event(
    wlh_host_t *host,
    const wlh_rpc_envelope_t *envelope,
    const uint8_t *payload,
    size_t payload_size
) {
    if (envelope->method_id == WLH_LINK_METHOD_CREDIT_UPDATE) {
        handle_credit_update(host, payload, payload_size);
    } else if (envelope->method_id == WLH_LINK_METHOD_HEARTBEAT) {
        handle_heartbeat(host, payload, payload_size);
    } else if (envelope->method_id == WLH_LINK_EVENT_SESSION_CHANGED) {
        handle_session_changed(host, payload, payload_size);
    }
}

static void transport_start_complete(void *context, int status) {
    wlh_host_t *host = context;
    (void)enqueue_job(
        host,
        status == 0 ? WLH_HOST_JOB_TRANSPORT_STARTED
                    : WLH_HOST_JOB_TRANSPORT_START_FAILED,
        NULL,
        WLH_OSAL_WAIT_FOREVER
    );
}

void transport_stop_complete(void *context, int status) {
    wlh_host_t *host = context;
    (void)enqueue_job(
        host,
        status == 0 ? WLH_HOST_JOB_TRANSPORT_STOPPED
                    : WLH_HOST_JOB_TRANSPORT_STOP_FAILED,
        NULL,
        WLH_OSAL_WAIT_FOREVER
    );
}

void request_transport_start(wlh_host_t *host) {
    set_state(host, WLH_HOST_STATE_TRANSPORT_STARTING);
    if (host->config.transport.start(
            host->config.transport.context, transport_start_complete, host
        ) != 0)
        set_state(host, WLH_HOST_STATE_FAILED);
}

void finish_shutdown(wlh_host_t *host) {
    host->session_id = 0u;
    set_state(host, WLH_HOST_STATE_UNINITIALIZED);
    host->worker_stopping = true;
}

void process_transport_lost(wlh_host_t *host) {
    if (host->state == WLH_HOST_STATE_STOPPING ||
        host->state == WLH_HOST_STATE_UNINITIALIZED)
        return;
    host->diagnostics.transport_resets++;
    cancel_pending(host, WLH_HOST_SESSION_CHANGED);
    host->session_id = 0u;
    memset(host->tx_credit, 0, sizeof(host->tx_credit));
    memset(host->tx_sequence, 0, sizeof(host->tx_sequence));
    memset(host->rx_sequence_valid, 0, sizeof(host->rx_sequence_valid));
    host->bluetooth_supported = false;
    host->bluetooth_hci_stopped = false;
    host->bluetooth_tx_inflight = 0u;
    host->bluetooth_state = WLH_BLUETOOTH_STATE_UNSPECIFIED;
    host->ota_supported = false;
    host->ota_tx_inflight = 0u;
    host->peer_version[0] = '\0';
    set_state(host, WLH_HOST_STATE_RECOVERING);
    if (host->config.transport.stop(
            host->config.transport.context, transport_stop_complete, host
        ) != 0)
        set_state(host, WLH_HOST_STATE_FAILED);
}

void wlh_host_transport_lost(wlh_host_t *host) {
    if (host != NULL && host->worker_started)
        (void)enqueue_job(
            host, WLH_HOST_JOB_TRANSPORT_LOST, NULL, WLH_OSAL_NO_WAIT
        );
}
