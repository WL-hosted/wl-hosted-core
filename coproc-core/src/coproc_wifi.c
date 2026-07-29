#include "coproc_internal.h"
#include "wlh/log.h"

#include <limits.h>
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

static bool bss_ssid_valid(const wlh_coproc_bss_t *bss) {
    return bss->ssid_size <= WLH_COPROC_MAX_SSID_SIZE &&
           (bss->ssid_size == 0u || bss->ssid != NULL);
}

WLH_NOINLINE wlh_coproc_result_t handle_wifi(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    const uint8_t *payload,
    size_t payload_size
) {
    pb_istream_t stream;
    int status = -1;

    switch (request->method_id) {
    case WLH_WIFI_METHOD_INITIALIZE: {
        uint32_t operation_id;
        WLH_LOGI(
            "wlh_coproc",
            "wifi initialize request %lu",
            (unsigned long)request->request_id
        );
        wlh_protocol_v1_WifiInitializeRequest message =
            wlh_protocol_v1_WifiInitializeRequest_init_zero;
        stream = pb_istream_from_buffer(payload, payload_size);
        if (!pb_decode(
                &stream, wlh_protocol_v1_WifiInitializeRequest_fields, &message
            )) {
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        if (coproc->wifi_initialize_pending.active)
            return send_rpc(
                coproc,
                request->service_id,
                request->method_id,
                request->request_id,
                WLH_RPC_KIND_RESPONSE,
                WLH_STATUS_DOMAIN_WIFI,
                WLH_STATUS_BUSY,
                NULL,
                0u
            );
        operation_id = coproc->next_backend_operation_id++;
        if (operation_id == 0u)
            operation_id = coproc->next_backend_operation_id++;
        coproc->wifi_initialize_pending.active = true;
        coproc->wifi_initialize_pending.operation_id = operation_id;
        coproc->wifi_initialize_pending.session_id = coproc->session_id;
        coproc->wifi_initialize_pending.request_id = request->request_id;
        status = coproc->config.wifi.initialize != NULL
                     ? coproc->config.wifi.initialize(
                           coproc->config.wifi.context,
                           operation_id,
                           message.interface_flags
                       )
                     : -1;
        if (status != 0) {
            memset(
                &coproc->wifi_initialize_pending,
                0,
                sizeof(coproc->wifi_initialize_pending)
            );
            return send_status(coproc, request, status);
        }
        return WLH_COPROC_OK;
    }

    case WLH_WIFI_METHOD_SCAN_START: {
        wlh_protocol_v1_WifiScanRequest *message;
        uint32_t scan_id;
        message =
            (wlh_protocol_v1_WifiScanRequest *)coproc->config.buffers.alloc(
                coproc->config.buffers.context, sizeof(*message)
            );
        if (message == NULL)
            return WLH_COPROC_BACKEND_ERROR;
        memset(message, 0, sizeof(*message));
        stream = pb_istream_from_buffer(payload, payload_size);
        if (!pb_decode(
                &stream, wlh_protocol_v1_WifiScanRequest_fields, message
            )) {
            coproc->config.buffers.free(
                coproc->config.buffers.context, (uint8_t *)message
            );
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        scan_id = message->scan_id;
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)message
        );
        WLH_LOGI(
            "wlh_coproc",
            "wifi scan start request %lu scan_id=%lu",
            (unsigned long)request->request_id,
            (unsigned long)scan_id
        );
        status =
            coproc->config.wifi.scan != NULL
                ? coproc->config.wifi.scan(coproc->config.wifi.context, scan_id)
                : -1;
        break;
    }

    case WLH_WIFI_METHOD_CONNECT: {
        wlh_protocol_v1_WifiConnectRequest *message;
        wlh_coproc_wifi_connect_t *connect;
        message =
            (wlh_protocol_v1_WifiConnectRequest *)coproc->config.buffers.alloc(
                coproc->config.buffers.context, sizeof(*message)
            );
        if (message == NULL)
            return WLH_COPROC_BACKEND_ERROR;
        memset(message, 0, sizeof(*message));
        stream = pb_istream_from_buffer(payload, payload_size);
        if (!pb_decode(
                &stream, wlh_protocol_v1_WifiConnectRequest_fields, message
            )) {
            coproc->config.buffers.free(
                coproc->config.buffers.context, (uint8_t *)message
            );
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        connect = (wlh_coproc_wifi_connect_t *)coproc->config.buffers.alloc(
            coproc->config.buffers.context, sizeof(*connect)
        );
        if (connect == NULL) {
            coproc->config.buffers.free(
                coproc->config.buffers.context, (uint8_t *)message
            );
            return WLH_COPROC_BACKEND_ERROR;
        }
        memset(connect, 0, sizeof(*connect));
        connect->ssid_size = message->ssid.size;
        connect->credential_size = message->credential.size;
        memcpy(connect->ssid, message->ssid.bytes, connect->ssid_size);
        memcpy(
            connect->credential,
            message->credential.bytes,
            connect->credential_size
        );
        connect->security = (uint32_t)message->security;
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)message
        );
        WLH_LOGI(
            "wlh_coproc",
            "wifi connect request %lu ssid_size=%zu security=%lu",
            (unsigned long)request->request_id,
            connect->ssid_size,
            (unsigned long)connect->security
        );
        status = coproc->config.wifi.connect != NULL
                     ? coproc->config.wifi.connect(
                           coproc->config.wifi.context, connect
                       )
                     : -1;
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)connect
        );
        break;
    }

    case WLH_WIFI_METHOD_DISCONNECT:
        WLH_LOGI(
            "wlh_coproc",
            "wifi disconnect request %lu",
            (unsigned long)request->request_id
        );
        status =
            coproc->config.wifi.disconnect != NULL
                ? coproc->config.wifi.disconnect(coproc->config.wifi.context)
                : -1;
        break;

    case WLH_WIFI_METHOD_START_AP: {
        wlh_protocol_v1_WifiStartApRequest *message;
        wlh_coproc_wifi_ap_t *ap;
        message =
            (wlh_protocol_v1_WifiStartApRequest *)coproc->config.buffers.alloc(
                coproc->config.buffers.context, sizeof(*message)
            );
        if (message == NULL)
            return WLH_COPROC_BACKEND_ERROR;
        memset(message, 0, sizeof(*message));
        stream = pb_istream_from_buffer(payload, payload_size);
        if (!pb_decode(
                &stream, wlh_protocol_v1_WifiStartApRequest_fields, message
            )) {
            coproc->config.buffers.free(
                coproc->config.buffers.context, (uint8_t *)message
            );
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        ap = (wlh_coproc_wifi_ap_t *)coproc->config.buffers.alloc(
            coproc->config.buffers.context, sizeof(*ap)
        );
        if (ap == NULL) {
            coproc->config.buffers.free(
                coproc->config.buffers.context, (uint8_t *)message
            );
            return WLH_COPROC_BACKEND_ERROR;
        }
        memset(ap, 0, sizeof(*ap));
        ap->ssid_size = message->ssid.size;
        ap->credential_size = message->credential.size;
        memcpy(ap->ssid, message->ssid.bytes, ap->ssid_size);
        memcpy(ap->credential, message->credential.bytes, ap->credential_size);
        ap->security = (uint32_t)message->security;
        ap->channel = message->channel;
        ap->max_clients = message->max_clients;
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)message
        );
        WLH_LOGI(
            "wlh_coproc",
            "wifi start_ap request %lu channel=%lu max_clients=%lu",
            (unsigned long)request->request_id,
            (unsigned long)ap->channel,
            (unsigned long)ap->max_clients
        );
        status =
            coproc->config.wifi.start_ap != NULL
                ? coproc->config.wifi.start_ap(coproc->config.wifi.context, ap)
                : -1;
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)ap
        );
        break;
    }

    case WLH_WIFI_METHOD_STOP_AP:
        WLH_LOGI(
            "wlh_coproc",
            "wifi stop_ap request %lu",
            (unsigned long)request->request_id
        );
        status = coproc->config.wifi.stop_ap != NULL
                     ? coproc->config.wifi.stop_ap(coproc->config.wifi.context)
                     : -1;
        break;

    default:
        return send_rpc(
            coproc,
            request->service_id,
            request->method_id,
            request->request_id,
            WLH_RPC_KIND_RESPONSE,
            WLH_STATUS_DOMAIN_PROTOCOL,
            WLH_STATUS_NOT_SUPPORTED,
            NULL,
            0u
        );
    }

    return send_status(coproc, request, status);
}

static wlh_coproc_result_t send_wifi_message(
    wlh_coproc_t *coproc,
    uint16_t method_id,
    const pb_msgdesc_t *fields,
    const void *message
) {
    return send_event_message(
        coproc, WLH_SERVICE_WIFI, method_id, fields, message
    );
}

wlh_coproc_result_t wlh_coproc_wifi_initialized(
    wlh_coproc_t *coproc, uint32_t operation_id, int backend_status
) {
    coproc_wifi_initialized_job_t *job;
    if (coproc == NULL || operation_id == 0u || !coproc->worker_started)
        return WLH_COPROC_INVALID_ARGUMENT;
    job = (coproc_wifi_initialized_job_t *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*job)
    );
    if (job == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    job->operation_id = operation_id;
    job->backend_status = backend_status;
    if (enqueue_job(
            coproc, COPROC_JOB_WIFI_INITIALIZED, job, WLH_OSAL_NO_WAIT
        ) != 0) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)job
        );
        return WLH_COPROC_BACKEND_ERROR;
    }
    return WLH_COPROC_OK;
}

wlh_coproc_result_t wlh_coproc_wifi_scan_result(
    wlh_coproc_t *coproc, uint32_t scan_id, const wlh_coproc_bss_t *bss
) {
    wlh_protocol_v1_WifiScanResultEvent *event;
    wlh_protocol_v1_WifiNetwork *network;
    wlh_coproc_result_t result;

    if (coproc == NULL || bss == NULL || !bss_ssid_valid(bss)) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }

    event = (wlh_protocol_v1_WifiScanResultEvent *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*event)
    );
    if (event == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    memset(event, 0, sizeof(*event));
    event->scan_id = scan_id;
    event->networks_count = 1u;
    network = &event->networks[0];

    network->ssid.size = bss->ssid_size;
    memcpy(network->ssid.bytes, bss->ssid, bss->ssid_size);

    network->bssid.size = 6u;
    memcpy(network->bssid.bytes, bss->bssid, 6u);

    network->channel = bss->channel;
    network->rssi_dbm = bss->rssi_dbm;
    network->security = (wlh_protocol_v1_WifiSecurity)bss->security;
    result = send_wifi_message(
        coproc,
        WLH_WIFI_EVENT_SCAN_RESULT,
        wlh_protocol_v1_WifiScanResultEvent_fields,
        event
    );
    coproc->config.buffers.free(
        coproc->config.buffers.context, (uint8_t *)event
    );
    return result;
}

wlh_coproc_result_t wlh_coproc_wifi_scan_completed(
    wlh_coproc_t *coproc,
    uint32_t scan_id,
    uint32_t result_count,
    bool cancelled
) {
    wlh_protocol_v1_WifiScanCompletedEvent event =
        wlh_protocol_v1_WifiScanCompletedEvent_init_zero;
    event.scan_id = scan_id;
    event.result_count = result_count;
    event.cancelled = cancelled;
    return send_wifi_message(
        coproc,
        WLH_WIFI_EVENT_SCAN_COMPLETED,
        wlh_protocol_v1_WifiScanCompletedEvent_fields,
        &event
    );
}

wlh_coproc_result_t wlh_coproc_wifi_connected(
    wlh_coproc_t *coproc, const wlh_coproc_bss_t *bss
) {
    wlh_protocol_v1_WifiConnectedEvent event =
        wlh_protocol_v1_WifiConnectedEvent_init_zero;

    if (coproc == NULL || bss == NULL || !bss_ssid_valid(bss)) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }

    event.has_link = true;
    event.link.interface = wlh_protocol_v1_WifiInterface_WIFI_INTERFACE_STA;
    event.link.connected = true;

    event.link.ssid.size = bss->ssid_size;
    memcpy(event.link.ssid.bytes, bss->ssid, bss->ssid_size);

    event.link.bssid.size = 6u;
    memcpy(event.link.bssid.bytes, bss->bssid, 6u);

    event.link.mac.size = 6u;
    memcpy(event.link.mac.bytes, bss->interface_mac, 6u);

    event.link.channel = bss->channel;
    event.link.rssi_dbm = bss->rssi_dbm;
    event.link.security = (wlh_protocol_v1_WifiSecurity)bss->security;
    return send_wifi_message(
        coproc,
        WLH_WIFI_EVENT_CONNECTED,
        wlh_protocol_v1_WifiConnectedEvent_fields,
        &event
    );
}

wlh_coproc_result_t wlh_coproc_wifi_disconnected(
    wlh_coproc_t *coproc, uint32_t reason, bool locally_initiated
) {
    wlh_protocol_v1_WifiDisconnectedEvent event =
        wlh_protocol_v1_WifiDisconnectedEvent_init_zero;
    event.interface = wlh_protocol_v1_WifiInterface_WIFI_INTERFACE_STA;
    event.reason = (wlh_protocol_v1_WifiDisconnectReason)reason;
    event.locally_initiated = locally_initiated;
    return send_wifi_message(
        coproc,
        WLH_WIFI_EVENT_DISCONNECTED,
        wlh_protocol_v1_WifiDisconnectedEvent_fields,
        &event
    );
}

wlh_coproc_result_t wlh_coproc_wifi_ap_started(
    wlh_coproc_t *coproc, const wlh_coproc_bss_t *ap
) {
    wlh_protocol_v1_WifiConnectedEvent event =
        wlh_protocol_v1_WifiConnectedEvent_init_zero;

    if (coproc == NULL || ap == NULL || !bss_ssid_valid(ap))
        return WLH_COPROC_INVALID_ARGUMENT;
    event.has_link = true;
    event.link.interface = wlh_protocol_v1_WifiInterface_WIFI_INTERFACE_AP;
    event.link.connected = true;
    event.link.ssid.size = ap->ssid_size;
    memcpy(event.link.ssid.bytes, ap->ssid, ap->ssid_size);
    event.link.bssid.size = 6u;
    memcpy(event.link.bssid.bytes, ap->bssid, 6u);
    event.link.mac.size = 6u;
    memcpy(event.link.mac.bytes, ap->interface_mac, 6u);
    event.link.channel = ap->channel;
    event.link.security = (wlh_protocol_v1_WifiSecurity)ap->security;
    return send_wifi_message(
        coproc,
        WLH_WIFI_EVENT_CONNECTED,
        wlh_protocol_v1_WifiConnectedEvent_fields,
        &event
    );
}

wlh_coproc_result_t wlh_coproc_wifi_ap_stopped(
    wlh_coproc_t *coproc, uint32_t reason, bool locally_initiated
) {
    wlh_protocol_v1_WifiDisconnectedEvent event =
        wlh_protocol_v1_WifiDisconnectedEvent_init_zero;
    if (coproc == NULL)
        return WLH_COPROC_INVALID_ARGUMENT;
    event.interface = wlh_protocol_v1_WifiInterface_WIFI_INTERFACE_AP;
    event.reason = (wlh_protocol_v1_WifiDisconnectReason)reason;
    event.locally_initiated = locally_initiated;
    return send_wifi_message(
        coproc,
        WLH_WIFI_EVENT_DISCONNECTED,
        wlh_protocol_v1_WifiDisconnectedEvent_fields,
        &event
    );
}

wlh_coproc_result_t wlh_coproc_wifi_ap_client_joined(
    wlh_coproc_t *coproc,
    const uint8_t mac[6],
    int32_t rssi_dbm,
    uint32_t association_id
) {
    wlh_protocol_v1_WifiApClientJoinedEvent event =
        wlh_protocol_v1_WifiApClientJoinedEvent_init_zero;

    if (coproc == NULL || mac == NULL) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }

    event.has_client = true;
    event.client.mac.size = 6u;
    memcpy(event.client.mac.bytes, mac, 6u);
    event.client.rssi_dbm = rssi_dbm;
    event.client.association_id = association_id;
    return send_wifi_message(
        coproc,
        WLH_WIFI_EVENT_AP_CLIENT_JOINED,
        wlh_protocol_v1_WifiApClientJoinedEvent_fields,
        &event
    );
}

wlh_coproc_result_t wlh_coproc_wifi_ap_client_left(
    wlh_coproc_t *coproc,
    const uint8_t mac[6],
    uint32_t association_id,
    uint32_t ieee80211_reason
) {
    wlh_protocol_v1_WifiApClientLeftEvent event =
        wlh_protocol_v1_WifiApClientLeftEvent_init_zero;

    if (coproc == NULL || mac == NULL) {
        return WLH_COPROC_INVALID_ARGUMENT;
    }

    event.mac.size = 6u;
    memcpy(event.mac.bytes, mac, 6u);
    event.association_id = association_id;
    event.ieee80211_reason = ieee80211_reason;
    return send_wifi_message(
        coproc,
        WLH_WIFI_EVENT_AP_CLIENT_LEFT,
        wlh_protocol_v1_WifiApClientLeftEvent_fields,
        &event
    );
}
