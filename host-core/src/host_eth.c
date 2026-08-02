#include "host_internal.h"

#include <string.h>

#include "eth.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>

typedef struct wlh_eth_info_request {
    wlh_host_t *host;
    wlh_host_eth_info_fn completion;
    void *context;
} wlh_eth_info_request_t;

static void eth_info_completion(
    void *context,
    wlh_host_result_t result,
    uint16_t status_domain,
    int16_t status_code,
    const uint8_t *payload,
    size_t payload_size
) {
    wlh_eth_info_request_t *request = context;
    wlh_host_t *host = request->host;
    wlh_host_eth_info_t info;
    const wlh_host_eth_info_t *decoded = NULL;

    if (result == WLH_HOST_OK) {
        wlh_protocol_v1_EthGetInfoResponse message =
            wlh_protocol_v1_EthGetInfoResponse_init_zero;
        pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
        if (pb_decode(
                &stream, wlh_protocol_v1_EthGetInfoResponse_fields, &message
            ) &&
            message.has_info &&
            (uint32_t)message.info.link_state >= WLH_HOST_ETH_LINK_STATE_DOWN &&
            (uint32_t)message.info.link_state <= WLH_HOST_ETH_LINK_STATE_UP &&
            message.info.mac_address.size == 6u &&
            (uint32_t)message.info.speed <= WLH_HOST_ETH_SPEED_1000M &&
            (uint32_t)message.info.duplex <= WLH_HOST_ETH_DUPLEX_FULL) {
            memset(&info, 0, sizeof(info));
            info.link_state =
                (wlh_host_eth_link_state_t)message.info.link_state;
            memcpy(info.mac_address, message.info.mac_address.bytes, 6u);
            info.speed = (wlh_host_eth_speed_t)message.info.speed;
            info.duplex = (wlh_host_eth_duplex_t)message.info.duplex;
            decoded = &info;
        } else {
            result = WLH_HOST_PROTOCOL_ERROR;
        }
    }
    request->completion(
        request->context, result, status_domain, status_code, decoded
    );
    host->config.buffers.free(host->config.buffers.context, (uint8_t *)request);
}

wlh_host_result_t wlh_host_eth_get_info(
    wlh_host_t *host, wlh_host_eth_info_fn completion, void *context
) {
    wlh_protocol_v1_EthGetInfoRequest message =
        wlh_protocol_v1_EthGetInfoRequest_init_zero;
    wlh_eth_info_request_t *request;
    wlh_host_result_t result;

    if (host == NULL || completion == NULL || !host->worker_started)
        return WLH_HOST_INVALID_ARGUMENT;
    request = (wlh_eth_info_request_t *)host->config.buffers.alloc(
        host->config.buffers.context, sizeof(*request)
    );
    if (request == NULL)
        return WLH_HOST_NO_MEMORY;
    request->host = host;
    request->completion = completion;
    request->context = context;

    result = rpc_message_request(
        host,
        WLH_SERVICE_ETH,
        WLH_ETH_METHOD_GET_INFO,
        wlh_protocol_v1_EthGetInfoRequest_fields,
        &message,
        eth_info_completion,
        request
    );
    if (result != WLH_HOST_OK)
        host->config.buffers.free(
            host->config.buffers.context, (uint8_t *)request
        );
    return result;
}
