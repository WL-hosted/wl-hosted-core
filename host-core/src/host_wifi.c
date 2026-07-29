#include "host_internal.h"

#include <string.h>

#include "adc.pb.h"
#include "bluetooth.pb.h"
#include "common.pb.h"
#include "device_info.pb.h"
#include "io.pb.h"
#include "kv.pb.h"
#include "user_passthrough.pb.h"
#include "wifi.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>

static wlh_host_result_t wifi_request(
    wlh_host_t *host,
    uint16_t method,
    const pb_msgdesc_t *fields,
    const void *message,
    wlh_rpc_completion_fn completion,
    void *context
) {
    return rpc_message_request(
        host, WLH_SERVICE_WIFI, method, fields, message, completion, context
    );
}

wlh_host_result_t wlh_host_wifi_initialize(
    wlh_host_t *host, wlh_rpc_completion_fn completion, void *context
) {
    wlh_protocol_v1_WifiInitializeRequest request =
        wlh_protocol_v1_WifiInitializeRequest_init_zero;
    request.interface_flags = 1u;
    return wifi_request(
        host,
        WLH_WIFI_METHOD_INITIALIZE,
        wlh_protocol_v1_WifiInitializeRequest_fields,
        &request,
        completion,
        context
    );
}

wlh_host_result_t wlh_host_wifi_scan(
    wlh_host_t *host,
    const wlh_wifi_scan_params_t *params,
    wlh_rpc_completion_fn completion,
    void *context
) {
    wlh_protocol_v1_WifiScanRequest request =
        wlh_protocol_v1_WifiScanRequest_init_zero;
    if (params == NULL || params->scan_id == 0u ||
        params->ssid_size > WLH_HOST_MAX_SSID_SIZE ||
        (params->ssid_size != 0u && params->ssid == NULL))
        return WLH_HOST_INVALID_ARGUMENT;
    request.scan_id = params->scan_id;
    request.interface = wlh_protocol_v1_WifiInterface_WIFI_INTERFACE_STA;
    request.ssid.size = params->ssid_size;
    if (params->ssid_size != 0u)
        memcpy(request.ssid.bytes, params->ssid, params->ssid_size);
    request.include_hidden = params->include_hidden;
    request.max_results = params->max_results;
    return wifi_request(
        host,
        WLH_WIFI_METHOD_SCAN_START,
        wlh_protocol_v1_WifiScanRequest_fields,
        &request,
        completion,
        context
    );
}

wlh_host_result_t wlh_host_wifi_connect(
    wlh_host_t *host,
    const wlh_wifi_connect_params_t *params,
    wlh_rpc_completion_fn completion,
    void *context
) {
    wlh_protocol_v1_WifiConnectRequest request =
        wlh_protocol_v1_WifiConnectRequest_init_zero;
    if (params == NULL || params->ssid_size == 0u ||
        params->ssid_size > WLH_HOST_MAX_SSID_SIZE ||
        params->credential_size > WLH_HOST_MAX_CREDENTIAL_SIZE ||
        params->ssid == NULL ||
        (params->credential_size != 0u && params->credential == NULL))
        return WLH_HOST_INVALID_ARGUMENT;
    request.ssid.size = params->ssid_size;
    memcpy(request.ssid.bytes, params->ssid, params->ssid_size);
    request.credential.size = params->credential_size;
    if (params->credential_size != 0u)
        memcpy(
            request.credential.bytes,
            params->credential,
            params->credential_size
        );
    request.security = (wlh_protocol_v1_WifiSecurity)params->security;
    request.timeout_ms = params->timeout_ms;
    return wifi_request(
        host,
        WLH_WIFI_METHOD_CONNECT,
        wlh_protocol_v1_WifiConnectRequest_fields,
        &request,
        completion,
        context
    );
}

wlh_host_result_t wlh_host_wifi_disconnect(
    wlh_host_t *host, wlh_rpc_completion_fn completion, void *context
) {
    wlh_protocol_v1_WifiDisconnectRequest request =
        wlh_protocol_v1_WifiDisconnectRequest_init_zero;
    request.interface = wlh_protocol_v1_WifiInterface_WIFI_INTERFACE_STA;
    return wifi_request(
        host,
        WLH_WIFI_METHOD_DISCONNECT,
        wlh_protocol_v1_WifiDisconnectRequest_fields,
        &request,
        completion,
        context
    );
}

wlh_host_result_t wlh_host_wifi_start_ap(
    wlh_host_t *host,
    const wlh_wifi_start_ap_params_t *params,
    wlh_rpc_completion_fn completion,
    void *context
) {
    wlh_protocol_v1_WifiStartApRequest request =
        wlh_protocol_v1_WifiStartApRequest_init_zero;
    if (params == NULL || params->ssid_size == 0u ||
        params->ssid_size > WLH_HOST_MAX_SSID_SIZE ||
        params->credential_size > WLH_HOST_MAX_CREDENTIAL_SIZE ||
        params->ssid == NULL ||
        (params->credential_size != 0u && params->credential == NULL))
        return WLH_HOST_INVALID_ARGUMENT;
    request.ssid.size = params->ssid_size;
    memcpy(request.ssid.bytes, params->ssid, params->ssid_size);
    request.credential.size = params->credential_size;
    if (params->credential_size != 0u)
        memcpy(
            request.credential.bytes,
            params->credential,
            params->credential_size
        );
    request.security = (wlh_protocol_v1_WifiSecurity)params->security;
    request.channel = params->channel;
    request.max_clients = params->max_clients;
    return wifi_request(
        host,
        WLH_WIFI_METHOD_START_AP,
        wlh_protocol_v1_WifiStartApRequest_fields,
        &request,
        completion,
        context
    );
}

wlh_host_result_t wlh_host_wifi_stop_ap(
    wlh_host_t *host, wlh_rpc_completion_fn completion, void *context
) {
    wlh_protocol_v1_Empty request = wlh_protocol_v1_Empty_init_zero;
    return wifi_request(
        host,
        WLH_WIFI_METHOD_STOP_AP,
        wlh_protocol_v1_Empty_fields,
        &request,
        completion,
        context
    );
}
