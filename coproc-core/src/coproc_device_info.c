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

WLH_NOINLINE wlh_coproc_result_t handle_device_info_request(
    wlh_coproc_t *coproc, const wlh_rpc_envelope_t *request
) {
    wlh_protocol_v1_DeviceInfoResponse *response;
    wlh_coproc_device_info_t info;
    wlh_coproc_result_t result;
    int status;

    memset(&info, 0, sizeof(info));
    status = coproc->config.device_info.get_info(
        coproc->config.device_info.context, &info
    );
    if (status != 0) {
        return send_rpc(
            coproc,
            request->service_id,
            request->method_id,
            request->request_id,
            WLH_RPC_KIND_RESPONSE,
            WLH_STATUS_DOMAIN_DEVICE_INFO,
            WLH_STATUS_INTERNAL,
            NULL,
            0u
        );
    }
    response =
        (wlh_protocol_v1_DeviceInfoResponse *)coproc->config.buffers.alloc(
            coproc->config.buffers.context, sizeof(*response)
        );
    if (response == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    memset(response, 0, sizeof(*response));

    /* The adapter contract is C strings; enforce termination and bounds
     * before handing the fields to nanopb. */
    info.vendor[sizeof(info.vendor) - 1u] = '\0';
    info.mcu_model[sizeof(info.mcu_model) - 1u] = '\0';
    info.board_profile[sizeof(info.board_profile) - 1u] = '\0';
    if (info.uid_size > sizeof(info.uid))
        info.uid_size = sizeof(info.uid);

    memcpy(response->vendor, info.vendor, sizeof(response->vendor) - 1u);
    memcpy(
        response->mcu_model, info.mcu_model, sizeof(response->mcu_model) - 1u
    );
    memcpy(
        response->board_profile,
        info.board_profile,
        sizeof(response->board_profile) - 1u
    );
    response->uid.size = info.uid_size;
    memcpy(response->uid.bytes, info.uid, info.uid_size);
    result = send_rpc_message(
        coproc,
        request->service_id,
        request->method_id,
        request->request_id,
        WLH_RPC_KIND_RESPONSE,
        WLH_STATUS_DOMAIN_NONE,
        WLH_STATUS_OK,
        wlh_protocol_v1_DeviceInfoResponse_fields,
        response
    );
    coproc->config.buffers.free(
        coproc->config.buffers.context, (uint8_t *)response
    );
    return result;
}
