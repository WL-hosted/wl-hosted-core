#include "coproc_internal.h"

#include <string.h>

#include "eth.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>

/* The wired Ethernet backend is present when the adapter supplies either the
 * service ops or the data-channel receive callback; a partial adapter still
 * gets a coherent negotiation (GET_INFO answers NOT_SUPPORTED without the
 * ops, and frames addressed to a missing receive callback are credited and
 * skipped, exactly like a Wi-Fi interface without one). */
bool eth_backend_present(const wlh_coproc_t *coproc) {
    return coproc->config.eth.get_info != NULL ||
           coproc->config.port.ethernet_eth_rx != NULL;
}

static wlh_coproc_result_t send_eth_status(
    wlh_coproc_t *coproc, uint32_t request_id, int16_t status_code
) {
    return send_rpc(
        coproc,
        WLH_SERVICE_ETH,
        WLH_ETH_METHOD_GET_INFO,
        request_id,
        WLH_RPC_KIND_RESPONSE,
        status_code == WLH_STATUS_OK ? WLH_STATUS_DOMAIN_NONE
                                     : WLH_STATUS_DOMAIN_ETH,
        status_code,
        NULL,
        0u
    );
}

WLH_NOINLINE wlh_coproc_result_t handle_eth(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    const uint8_t *payload,
    size_t payload_size
) {
    pb_istream_t stream;
    uint32_t operation_id;
    int status;

    switch (request->method_id) {
    case WLH_ETH_METHOD_GET_INFO: {
        wlh_protocol_v1_EthGetInfoRequest message =
            wlh_protocol_v1_EthGetInfoRequest_init_zero;
        stream = pb_istream_from_buffer(payload, payload_size);
        if (!pb_decode(
                &stream, wlh_protocol_v1_EthGetInfoRequest_fields, &message
            )) {
            return WLH_COPROC_PROTOCOL_ERROR;
        }
        if (coproc->config.eth.get_info == NULL) {
            return send_eth_status(
                coproc, request->request_id, WLH_STATUS_NOT_SUPPORTED
            );
        }
        if (coproc->eth_pending.active) {
            return send_eth_status(
                coproc, request->request_id, WLH_STATUS_BUSY
            );
        }
        operation_id = coproc->next_backend_operation_id++;
        if (operation_id == 0u)
            operation_id = coproc->next_backend_operation_id++;
        coproc->eth_pending.active = true;
        coproc->eth_pending.operation_id = operation_id;
        coproc->eth_pending.session_id = coproc->session_id;
        coproc->eth_pending.request_id = request->request_id;
        status = coproc->config.eth.get_info(
            coproc->config.eth.context, operation_id
        );
        if (status != 0) {
            memset(&coproc->eth_pending, 0, sizeof(coproc->eth_pending));
            return send_eth_status(
                coproc, request->request_id, WLH_STATUS_INTERNAL
            );
        }
        return WLH_COPROC_OK;
    }

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
}

WLH_NOINLINE void eth_info_completed(
    wlh_coproc_t *coproc, const coproc_eth_info_job_t *completed
) {
    uint32_t request_id;
    wlh_protocol_v1_EthGetInfoResponse response =
        wlh_protocol_v1_EthGetInfoResponse_init_zero;

    /* A completion that does not match the outstanding submission is stale
     * (superseded session or already answered); dropping it is safe because
     * every accepted submission produces exactly one answer. */
    if (!coproc->eth_pending.active ||
        completed->operation_id != coproc->eth_pending.operation_id ||
        coproc->eth_pending.session_id != coproc->session_id) {
        return;
    }
    request_id = coproc->eth_pending.request_id;
    memset(&coproc->eth_pending, 0, sizeof(coproc->eth_pending));
    if (completed->backend_status != 0) {
        (void)send_eth_status(coproc, request_id, WLH_STATUS_INTERNAL);
        return;
    }
    response.has_info = true;
    response.info.link_state =
        (wlh_protocol_v1_EthLinkState)completed->info.link_state;
    response.info.mac_address.size =
        (pb_size_t)sizeof(completed->info.mac_address);
    memcpy(
        response.info.mac_address.bytes,
        completed->info.mac_address,
        sizeof(completed->info.mac_address)
    );
    response.info.speed = (wlh_protocol_v1_EthSpeed)completed->info.speed;
    response.info.duplex = (wlh_protocol_v1_EthDuplex)completed->info.duplex;
    (void)send_rpc_message(
        coproc,
        WLH_SERVICE_ETH,
        WLH_ETH_METHOD_GET_INFO,
        request_id,
        WLH_RPC_KIND_RESPONSE,
        WLH_STATUS_DOMAIN_NONE,
        WLH_STATUS_OK,
        wlh_protocol_v1_EthGetInfoResponse_fields,
        &response
    );
}

wlh_coproc_result_t wlh_coproc_eth_info_ready(
    wlh_coproc_t *coproc,
    uint32_t operation_id,
    int backend_status,
    const wlh_coproc_eth_info_t *info
) {
    coproc_eth_info_job_t *job;
    if (coproc == NULL || operation_id == 0u || !coproc->worker_started ||
        (info == NULL && backend_status == 0))
        return WLH_COPROC_INVALID_ARGUMENT;
    job = (coproc_eth_info_job_t *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*job)
    );
    if (job == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    memset(job, 0, sizeof(*job));
    job->operation_id = operation_id;
    job->backend_status = backend_status;
    if (info != NULL)
        job->info = *info;
    if (enqueue_job(coproc, COPROC_JOB_ETH_INFO, job, WLH_OSAL_NO_WAIT) != 0) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)job
        );
        return WLH_COPROC_BACKEND_ERROR;
    }
    return WLH_COPROC_OK;
}

wlh_coproc_result_t wlh_coproc_eth_link_state_changed(
    wlh_coproc_t *coproc,
    wlh_coproc_eth_link_state_t link_state,
    wlh_coproc_eth_speed_t speed,
    wlh_coproc_eth_duplex_t duplex
) {
    wlh_protocol_v1_EthLinkStateChangedEvent event =
        wlh_protocol_v1_EthLinkStateChangedEvent_init_zero;
    if (coproc == NULL || !coproc->worker_started ||
        (link_state != WLH_COPROC_ETH_LINK_STATE_DOWN &&
         link_state != WLH_COPROC_ETH_LINK_STATE_UP) ||
        speed > WLH_COPROC_ETH_SPEED_1000M ||
        duplex > WLH_COPROC_ETH_DUPLEX_FULL)
        return WLH_COPROC_INVALID_ARGUMENT;
    event.link_state = (wlh_protocol_v1_EthLinkState)link_state;
    event.speed = (wlh_protocol_v1_EthSpeed)speed;
    event.duplex = (wlh_protocol_v1_EthDuplex)duplex;
    return send_event_message(
        coproc,
        WLH_SERVICE_ETH,
        WLH_ETH_EVENT_LINK_STATE_CHANGED,
        wlh_protocol_v1_EthLinkStateChangedEvent_fields,
        &event
    );
}
