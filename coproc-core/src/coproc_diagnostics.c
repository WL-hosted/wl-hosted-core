#include "coproc_internal.h"

#include "diagnostics.pb.h"
#include <pb_decode.h>

wlh_coproc_result_t handle_diagnostics_request(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    const uint8_t *message,
    size_t message_size
) {
    uint8_t response_data[64];
    size_t response_size = 0u;
    wlh_protocol_v1_DiagnosticsPingRequest ping =
        wlh_protocol_v1_DiagnosticsPingRequest_init_zero;
    wlh_protocol_v1_DiagnosticsPingResponse response =
        wlh_protocol_v1_DiagnosticsPingResponse_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(message, message_size);

    if (request->method_id != WLH_DIAGNOSTICS_METHOD_PING)
        return WLH_COPROC_NOT_SUPPORTED;
    if (!pb_decode(
            &stream, wlh_protocol_v1_DiagnosticsPingRequest_fields, &ping
        ))
        return WLH_COPROC_PROTOCOL_ERROR;

    response.cookie = ping.cookie;
    response.host_time_us = ping.host_time_us;
    response.coprocessor_uptime_us =
        (now_ms(coproc) - coproc->started_ms) * 1000u;
    if (!encode_message(
            response_data,
            sizeof(response_data),
            &response_size,
            wlh_protocol_v1_DiagnosticsPingResponse_fields,
            &response
        ))
        return WLH_COPROC_PROTOCOL_ERROR;

    return send_rpc(
        coproc,
        request->service_id,
        request->method_id,
        request->request_id,
        WLH_RPC_KIND_RESPONSE,
        WLH_STATUS_DOMAIN_NONE,
        WLH_STATUS_OK,
        response_data,
        response_size
    );
}
