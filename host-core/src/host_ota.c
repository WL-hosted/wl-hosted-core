#include "host_internal.h"

#include <string.h>

#include "common.pb.h"
#include "ota.pb.h"
#include <pb_decode.h>

static wlh_host_result_t ota_check_supported(wlh_host_t *host) {
    bool supported;
    if (host == NULL || !host->worker_started)
        return WLH_HOST_INVALID_STATE;
    (void)host->config.osal.mutex_lock(
        host->config.osal.context, &host->state_mutex, WLH_OSAL_WAIT_FOREVER
    );
    supported = host->ota_supported;
    host->config.osal.mutex_unlock(
        host->config.osal.context, &host->state_mutex
    );
    return supported ? WLH_HOST_OK : WLH_HOST_NOT_SUPPORTED;
}

static wlh_host_result_t ota_request(
    wlh_host_t *host,
    uint16_t method,
    const pb_msgdesc_t *fields,
    const void *message,
    wlh_rpc_completion_fn completion,
    void *context
) {
    wlh_host_result_t result = ota_check_supported(host);
    if (result != WLH_HOST_OK)
        return result;
    return rpc_message_request(
        host, WLH_SERVICE_OTA, method, fields, message, completion, context
    );
}

typedef struct ota_begin_request {
    wlh_host_t *host;
    wlh_host_ota_begin_fn completion;
    void *context;
} ota_begin_request_t;

static void ota_begin_completion(
    void *context,
    wlh_host_result_t result,
    uint16_t domain,
    int16_t status,
    const uint8_t *payload,
    size_t payload_size
) {
    ota_begin_request_t *request = context;
    wlh_host_ota_begin_response_t response;
    const wlh_host_ota_begin_response_t *decoded = NULL;
    if (result == WLH_HOST_OK) {
        wlh_protocol_v1_OtaBeginResponse message =
            wlh_protocol_v1_OtaBeginResponse_init_zero;
        pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
        if (pb_decode(
                &stream, wlh_protocol_v1_OtaBeginResponse_fields, &message
            ) &&
            message.transfer_id != 0u && message.stream_chunk_size != 0u &&
            message.stream_alignment != 0u) {
            response.transfer_id = message.transfer_id;
            response.stream_chunk_size = message.stream_chunk_size;
            response.stream_alignment = message.stream_alignment;
            decoded = &response;
        } else {
            result = WLH_HOST_PROTOCOL_ERROR;
        }
    }
    request->completion(request->context, result, domain, status, decoded);
    request->host->config.buffers.free(
        request->host->config.buffers.context, (uint8_t *)request
    );
}

wlh_host_result_t wlh_host_ota_begin(
    wlh_host_t *host,
    const wlh_host_ota_begin_params_t *params,
    wlh_host_ota_begin_fn completion,
    void *context
) {
    wlh_protocol_v1_OtaBeginRequest *message;
    ota_begin_request_t *request;
    wlh_host_result_t result;
    if (host == NULL || params == NULL || completion == NULL ||
        params->image_size == 0u || params->signature_size > 512u ||
        (params->signature_size != 0u && params->signature == NULL) ||
        (params->target_version != NULL &&
         strlen(params->target_version) > 32u) ||
        (params->slot != NULL && strlen(params->slot) > 16u))
        return WLH_HOST_INVALID_ARGUMENT;
    result = ota_check_supported(host);
    if (result != WLH_HOST_OK)
        return result;
    request = (ota_begin_request_t *)host->config.buffers.alloc(
        host->config.buffers.context, sizeof(*request)
    );
    if (request == NULL)
        return WLH_HOST_NO_MEMORY;
    message = (wlh_protocol_v1_OtaBeginRequest *)host->config.buffers.alloc(
        host->config.buffers.context, sizeof(*message)
    );
    if (message == NULL) {
        host->config.buffers.free(
            host->config.buffers.context, (uint8_t *)request
        );
        return WLH_HOST_NO_MEMORY;
    }
    memset(message, 0, sizeof(*message));
    request->host = host;
    request->completion = completion;
    request->context = context;
    message->image_type =
        wlh_protocol_v1_OtaImageType_OTA_IMAGE_TYPE_COPROCESSOR_FIRMWARE;
    message->image_size = params->image_size;
    message->sha256.size = 32u;
    memcpy(message->sha256.bytes, params->sha256, 32u);
    if (params->target_version != NULL)
        memcpy(
            message->target_version,
            params->target_version,
            strlen(params->target_version) + 1u
        );
    if (params->slot != NULL)
        memcpy(message->slot, params->slot, strlen(params->slot) + 1u);
    message->signature.size = params->signature_size;
    if (params->signature_size != 0u)
        memcpy(
            message->signature.bytes, params->signature, params->signature_size
        );
    result = ota_request(
        host,
        WLH_OTA_METHOD_BEGIN,
        wlh_protocol_v1_OtaBeginRequest_fields,
        message,
        ota_begin_completion,
        request
    );
    host->config.buffers.free(host->config.buffers.context, (uint8_t *)message);
    if (result != WLH_HOST_OK)
        host->config.buffers.free(
            host->config.buffers.context, (uint8_t *)request
        );
    return result;
}

wlh_host_result_t wlh_host_ota_finalize(
    wlh_host_t *host,
    uint32_t transfer_id,
    uint64_t bytes_sent,
    wlh_rpc_completion_fn completion,
    void *context
) {
    wlh_protocol_v1_OtaFinalizeRequest message =
        wlh_protocol_v1_OtaFinalizeRequest_init_zero;
    if (transfer_id == 0u)
        return WLH_HOST_INVALID_ARGUMENT;
    message.transfer_id = transfer_id;
    message.bytes_sent = bytes_sent;
    return ota_request(
        host,
        WLH_OTA_METHOD_FINALIZE,
        wlh_protocol_v1_OtaFinalizeRequest_fields,
        &message,
        completion,
        context
    );
}

wlh_host_result_t wlh_host_ota_abort(
    wlh_host_t *host,
    uint32_t transfer_id,
    wlh_rpc_completion_fn completion,
    void *context
) {
    wlh_protocol_v1_OtaAbortRequest message =
        wlh_protocol_v1_OtaAbortRequest_init_zero;
    if (transfer_id == 0u)
        return WLH_HOST_INVALID_ARGUMENT;
    message.transfer_id = transfer_id;
    return ota_request(
        host,
        WLH_OTA_METHOD_ABORT,
        wlh_protocol_v1_OtaAbortRequest_fields,
        &message,
        completion,
        context
    );
}

wlh_host_result_t wlh_host_ota_activate(
    wlh_host_t *host,
    uint32_t transfer_id,
    bool reboot,
    wlh_rpc_completion_fn completion,
    void *context
) {
    wlh_protocol_v1_OtaActivateRequest message =
        wlh_protocol_v1_OtaActivateRequest_init_zero;
    if (transfer_id == 0u)
        return WLH_HOST_INVALID_ARGUMENT;
    message.transfer_id = transfer_id;
    message.reboot = reboot;
    return ota_request(
        host,
        WLH_OTA_METHOD_ACTIVATE,
        wlh_protocol_v1_OtaActivateRequest_fields,
        &message,
        completion,
        context
    );
}

typedef struct ota_query_request {
    wlh_host_t *host;
    wlh_host_ota_query_fn completion;
    void *context;
} ota_query_request_t;

static void ota_query_completion(
    void *context,
    wlh_host_result_t result,
    uint16_t domain,
    int16_t status,
    const uint8_t *payload,
    size_t payload_size
) {
    ota_query_request_t *request = context;
    wlh_host_ota_query_response_t response;
    const wlh_host_ota_query_response_t *decoded = NULL;
    if (result == WLH_HOST_OK) {
        wlh_protocol_v1_OtaQueryResponse message =
            wlh_protocol_v1_OtaQueryResponse_init_zero;
        pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
        if (pb_decode(
                &stream, wlh_protocol_v1_OtaQueryResponse_fields, &message
            ) &&
            (uint32_t)message.state <=
                wlh_protocol_v1_OtaState_OTA_STATE_FAILED) {
            memset(&response, 0, sizeof(response));
            response.state = (uint32_t)message.state;
            response.transfer_id = message.transfer_id;
            response.image_size = message.image_size;
            response.bytes_received = message.bytes_received;
            memcpy(
                response.target_version,
                message.target_version,
                sizeof(response.target_version)
            );
            decoded = &response;
        } else
            result = WLH_HOST_PROTOCOL_ERROR;
    }
    request->completion(request->context, result, domain, status, decoded);
    request->host->config.buffers.free(
        request->host->config.buffers.context, (uint8_t *)request
    );
}

wlh_host_result_t wlh_host_ota_query(
    wlh_host_t *host, wlh_host_ota_query_fn completion, void *context
) {
    wlh_protocol_v1_Empty message = wlh_protocol_v1_Empty_init_zero;
    ota_query_request_t *request;
    wlh_host_result_t result;
    if (host == NULL || completion == NULL)
        return WLH_HOST_INVALID_ARGUMENT;
    result = ota_check_supported(host);
    if (result != WLH_HOST_OK)
        return result;
    request = (ota_query_request_t *)host->config.buffers.alloc(
        host->config.buffers.context, sizeof(*request)
    );
    if (request == NULL)
        return WLH_HOST_NO_MEMORY;
    request->host = host;
    request->completion = completion;
    request->context = context;
    result = ota_request(
        host,
        WLH_OTA_METHOD_QUERY,
        wlh_protocol_v1_Empty_fields,
        &message,
        ota_query_completion,
        request
    );
    if (result != WLH_HOST_OK)
        host->config.buffers.free(
            host->config.buffers.context, (uint8_t *)request
        );
    return result;
}

static void put_le16(uint8_t *out, uint16_t value) {
    out[0] = (uint8_t)value;
    out[1] = (uint8_t)(value >> 8);
}
static void put_le32(uint8_t *out, uint32_t value) {
    unsigned i;
    for (i = 0; i < 4u; ++i)
        out[i] = (uint8_t)(value >> (i * 8u));
}
static void put_le64(uint8_t *out, uint64_t value) {
    unsigned i;
    for (i = 0; i < 8u; ++i)
        out[i] = (uint8_t)(value >> (i * 8u));
}

wlh_host_result_t wlh_host_ota_stream_send(
    wlh_host_t *host,
    uint32_t transfer_id,
    uint64_t offset,
    const uint8_t *data,
    size_t size
) {
    wlh_host_data_job_t *job;
    size_t record_size;
    wlh_host_result_t result = WLH_HOST_OK;
    if (host == NULL || data == NULL || size == 0u || size > UINT16_MAX ||
        transfer_id == 0u || !host->worker_started)
        return WLH_HOST_INVALID_ARGUMENT;
    (void)host->config.osal.mutex_lock(
        host->config.osal.context, &host->state_mutex, WLH_OSAL_WAIT_FOREVER
    );
    if (!host->ota_supported)
        result = WLH_HOST_NOT_SUPPORTED;
    else if (size + 16u > host->ota_max_record)
        result = WLH_HOST_INVALID_ARGUMENT;
    else if (host->tx_credit[WLH_CHANNEL_OTA_STREAM] <= host->ota_tx_inflight)
        result = WLH_HOST_NO_CREDIT;
    else
        host->ota_tx_inflight++;
    host->config.osal.mutex_unlock(
        host->config.osal.context, &host->state_mutex
    );
    if (result != WLH_HOST_OK)
        return result;
    job = (wlh_host_data_job_t *)host->config.buffers.alloc(
        host->config.buffers.context,
        sizeof(*job) + WLH_RAW_RECORD_HEADER_SIZE + 16u + size
    );
    if (job == NULL) {
        result = WLH_HOST_NO_MEMORY;
        goto release;
    }
    job->channel = WLH_CHANNEL_OTA_STREAM;
    put_le32(job->data + WLH_RAW_RECORD_HEADER_SIZE, transfer_id);
    put_le64(job->data + WLH_RAW_RECORD_HEADER_SIZE + 4u, offset);
    put_le16(job->data + WLH_RAW_RECORD_HEADER_SIZE + 12u, (uint16_t)size);
    put_le16(job->data + WLH_RAW_RECORD_HEADER_SIZE + 14u, 0u);
    memcpy(job->data + WLH_RAW_RECORD_HEADER_SIZE + 16u, data, size);
    if (wlh_raw_record_encode(
            job->data,
            WLH_RAW_RECORD_HEADER_SIZE + 16u + size,
            &record_size,
            1u,
            0u,
            job->data + WLH_RAW_RECORD_HEADER_SIZE,
            16u + size
        ) != WLH_WIRE_OK) {
        host->config.buffers.free(host->config.buffers.context, (uint8_t *)job);
        result = WLH_HOST_INVALID_ARGUMENT;
        goto release;
    }
    job->size = record_size;
    if (enqueue_job(host, WLH_HOST_JOB_OTA_TX, job, WLH_OSAL_NO_WAIT) != 0) {
        host->config.buffers.free(host->config.buffers.context, (uint8_t *)job);
        result = WLH_HOST_PENDING_FULL;
        goto release;
    }
    return WLH_HOST_OK;
release:
    (void)host->config.osal.mutex_lock(
        host->config.osal.context, &host->state_mutex, WLH_OSAL_WAIT_FOREVER
    );
    if (host->ota_tx_inflight > 0u)
        host->ota_tx_inflight--;
    host->config.osal.mutex_unlock(
        host->config.osal.context, &host->state_mutex
    );
    return result;
}

wlh_host_result_t process_ota_frame(
    wlh_host_t *host, const uint8_t *payload, size_t payload_size
) {
    wlh_raw_record_iterator_t it;
    wlh_raw_record_view_t record;
    if (wlh_raw_record_iterator_init(&it, payload, payload_size) != WLH_WIRE_OK)
        return WLH_HOST_PROTOCOL_ERROR;
    while (wlh_raw_record_iterator_next(&it, &record) == WLH_WIRE_OK) {
        const uint8_t *p = record.payload;
        if (record.record_type != 1u || record.payload_size < 16u)
            return WLH_HOST_PROTOCOL_ERROR;
        if (host->config.ota_stream_rx != NULL &&
            host->config.ota_stream_rx(
                host->config.ota_context,
                (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
                    ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24),
                (uint64_t)p[4] | ((uint64_t)p[5] << 8) |
                    ((uint64_t)p[6] << 16) | ((uint64_t)p[7] << 24) |
                    ((uint64_t)p[8] << 32) | ((uint64_t)p[9] << 40) |
                    ((uint64_t)p[10] << 48) | ((uint64_t)p[11] << 56),
                p + 16u,
                record.payload_size - 16u,
                (uint16_t)p[14] | ((uint16_t)p[15] << 8)
            ) != WLH_HOST_OK)
            return WLH_HOST_NO_MEMORY;
    }
    return WLH_HOST_OK;
}

const char *wlh_host_get_peer_version(const wlh_host_t *host) {
    return host == NULL ? NULL : host->peer_version;
}
