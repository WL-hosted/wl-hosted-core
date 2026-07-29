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

typedef struct wlh_adc_read_request {
    wlh_host_t *host;
    wlh_host_adc_read_fn completion;
    void *context;
} wlh_adc_read_request_t;

static void adc_read_completion(
    void *context,
    wlh_host_result_t result,
    uint16_t status_domain,
    int16_t status_code,
    const uint8_t *payload,
    size_t payload_size
) {
    wlh_adc_read_request_t *request = context;
    wlh_host_t *host = request->host;
    wlh_host_adc_sample_t sample;
    const wlh_host_adc_sample_t *decoded = NULL;

    if (result == WLH_HOST_OK) {
        wlh_protocol_v1_AdcReadResponse message =
            wlh_protocol_v1_AdcReadResponse_init_zero;
        pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
        if (pb_decode(
                &stream, wlh_protocol_v1_AdcReadResponse_fields, &message
            )) {
            sample.pin_id = message.pin_id;
            sample.millivolts = message.millivolts;
            decoded = &sample;
        } else {
            result = WLH_HOST_PROTOCOL_ERROR;
        }
    }
    request->completion(
        request->context, result, status_domain, status_code, decoded
    );
    host->config.buffers.free(host->config.buffers.context, (uint8_t *)request);
}

wlh_host_result_t wlh_host_adc_read(
    wlh_host_t *host,
    uint32_t pin_id,
    wlh_host_adc_read_fn completion,
    void *context
) {
    wlh_protocol_v1_AdcReadRequest message =
        wlh_protocol_v1_AdcReadRequest_init_zero;
    wlh_adc_read_request_t *request;
    wlh_host_result_t result;

    if (host == NULL || completion == NULL)
        return WLH_HOST_INVALID_ARGUMENT;
    request = (wlh_adc_read_request_t *)host->config.buffers.alloc(
        host->config.buffers.context, sizeof(*request)
    );
    if (request == NULL)
        return WLH_HOST_NO_MEMORY;
    request->host = host;
    request->completion = completion;
    request->context = context;
    message.pin_id = pin_id;

    result = rpc_message_request(
        host,
        WLH_SERVICE_ADC,
        WLH_ADC_METHOD_READ,
        wlh_protocol_v1_AdcReadRequest_fields,
        &message,
        adc_read_completion,
        request
    );
    if (result != WLH_HOST_OK)
        host->config.buffers.free(
            host->config.buffers.context, (uint8_t *)request
        );
    return result;
}
