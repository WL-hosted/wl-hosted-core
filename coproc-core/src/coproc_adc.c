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

WLH_NOINLINE wlh_coproc_result_t handle_adc_request(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    const uint8_t *message,
    size_t message_size
) {
    wlh_protocol_v1_AdcReadRequest read =
        wlh_protocol_v1_AdcReadRequest_init_zero;
    wlh_protocol_v1_AdcReadResponse response =
        wlh_protocol_v1_AdcReadResponse_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(message, message_size);
    uint32_t millivolts = 0u;
    int status;

    if (request->method_id != WLH_ADC_METHOD_READ) {
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
    if (!pb_decode(&stream, wlh_protocol_v1_AdcReadRequest_fields, &read))
        return WLH_COPROC_PROTOCOL_ERROR;
    status = coproc->config.adc.read(
        coproc->config.adc.context, read.pin_id, &millivolts
    );
    if (status != WLH_COPROC_SERVICE_OK)
        return send_service_error(
            coproc, request, WLH_STATUS_DOMAIN_PERIPHERAL, status
        );
    response.pin_id = read.pin_id;
    response.millivolts = millivolts;
    return send_rpc_message(
        coproc,
        request->service_id,
        request->method_id,
        request->request_id,
        WLH_RPC_KIND_RESPONSE,
        WLH_STATUS_DOMAIN_NONE,
        WLH_STATUS_OK,
        wlh_protocol_v1_AdcReadResponse_fields,
        &response
    );
}
