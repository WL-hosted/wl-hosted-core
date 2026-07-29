#include "coproc_internal.h"
#include "wlh/log.h"

#include <limits.h>
#include <string.h>

#include "ota.pb.h"
#include <pb_decode.h>
#include <pb_encode.h>

#define OTA_STATE_UNSPECIFIED                                                  \
    ((uint32_t)wlh_protocol_v1_OtaState_OTA_STATE_UNSPECIFIED)
#define OTA_STATE_IDLE ((uint32_t)wlh_protocol_v1_OtaState_OTA_STATE_IDLE)
#define OTA_STATE_RECEIVING                                                    \
    ((uint32_t)wlh_protocol_v1_OtaState_OTA_STATE_RECEIVING)
#define OTA_STATE_VERIFYING                                                    \
    ((uint32_t)wlh_protocol_v1_OtaState_OTA_STATE_VERIFYING)
#define OTA_STATE_READY_TO_ACTIVATE                                            \
    ((uint32_t)wlh_protocol_v1_OtaState_OTA_STATE_READY_TO_ACTIVATE)
#define OTA_STATE_FAILED ((uint32_t)wlh_protocol_v1_OtaState_OTA_STATE_FAILED)

/* Emit a progress event no more than once per this many durable bytes, plus
 * one at every state transition. */
#define OTA_PROGRESS_INTERVAL_BYTES 65536u

bool ota_backend_present(const wlh_coproc_t *coproc) {
    return coproc->config.ota.write != NULL;
}

static bool ota_state_is_idle(uint32_t state) {
    return state == OTA_STATE_UNSPECIFIED || state == OTA_STATE_IDLE ||
           state == OTA_STATE_FAILED;
}

static void ota_emit_progress(wlh_coproc_t *coproc) {
    wlh_protocol_v1_OtaProgressEvent event =
        wlh_protocol_v1_OtaProgressEvent_init_zero;
    event.transfer_id = coproc->ota_transfer_id;
    event.state = (wlh_protocol_v1_OtaState)coproc->ota_state;
    event.bytes_received = coproc->ota_bytes_received;
    coproc->ota_progress_reported_bytes = coproc->ota_bytes_received;
    (void)send_event_message(
        coproc,
        WLH_SERVICE_OTA,
        WLH_OTA_EVENT_PROGRESS,
        wlh_protocol_v1_OtaProgressEvent_fields,
        &event
    );
}

static void ota_set_state(wlh_coproc_t *coproc, uint32_t state) {
    if (coproc->ota_state == state)
        return;
    coproc->ota_state = state;
    ota_emit_progress(coproc);
}

void ota_reset(wlh_coproc_t *coproc) {
    bool in_progress = coproc->ota_state == OTA_STATE_RECEIVING ||
                       coproc->ota_state == OTA_STATE_VERIFYING ||
                       coproc->ota_state == OTA_STATE_READY_TO_ACTIVATE;
    if (in_progress && ota_backend_present(coproc) &&
        coproc->config.ota.abort != NULL) {
        (void)coproc->config.ota.abort(
            coproc->config.ota.context, 0u, coproc->ota_transfer_id
        );
    }
    memset(&coproc->ota_pending, 0, sizeof(coproc->ota_pending));
    coproc->ota_state = OTA_STATE_IDLE;
    coproc->ota_transfer_id = 0u;
    coproc->ota_image_size = 0u;
    coproc->ota_bytes_accepted = 0u;
    coproc->ota_bytes_received = 0u;
    coproc->ota_progress_reported_bytes = 0u;
    memset(coproc->ota_target_version, 0, sizeof(coproc->ota_target_version));
}

static wlh_coproc_result_t send_ota_status(
    wlh_coproc_t *coproc,
    uint16_t method_id,
    uint32_t request_id,
    int16_t status_code
) {
    return send_rpc(
        coproc,
        WLH_SERVICE_OTA,
        method_id,
        request_id,
        WLH_RPC_KIND_RESPONSE,
        status_code == WLH_STATUS_OK ? WLH_STATUS_DOMAIN_NONE
                                     : WLH_STATUS_DOMAIN_OTA,
        status_code,
        NULL,
        0u
    );
}

static uint32_t ota_begin_operation(
    wlh_coproc_t *coproc, const wlh_rpc_envelope_t *request
) {
    uint32_t operation_id = coproc->next_backend_operation_id++;
    if (operation_id == 0u)
        operation_id = coproc->next_backend_operation_id++;
    coproc->ota_pending.active = true;
    coproc->ota_pending.operation_id = operation_id;
    coproc->ota_pending.session_id = coproc->session_id;
    coproc->ota_pending.request_id = request->request_id;
    coproc->ota_pending.method_id = request->method_id;
    return operation_id;
}

static wlh_coproc_result_t ota_submit_result(
    wlh_coproc_t *coproc, const wlh_rpc_envelope_t *request, int status
) {
    if (status != 0) {
        memset(&coproc->ota_pending, 0, sizeof(coproc->ota_pending));
        return send_ota_status(
            coproc, request->method_id, request->request_id, WLH_STATUS_INTERNAL
        );
    }
    return WLH_COPROC_OK;
}

static uint32_t ota_next_transfer_id(wlh_coproc_t *coproc) {
    uint32_t transfer_id = coproc->next_ota_transfer_id++;
    if (transfer_id == 0u)
        transfer_id = coproc->next_ota_transfer_id++;
    return transfer_id;
}

static wlh_coproc_result_t handle_ota_begin(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    const uint8_t *payload,
    size_t payload_size
) {
    wlh_protocol_v1_OtaBeginRequest *message;
    wlh_coproc_ota_begin_params_t params;
    pb_istream_t stream;
    uint32_t operation_id;
    uint32_t transfer_id;
    int status;

    if (coproc->ota_pending.active) {
        return send_ota_status(
            coproc, request->method_id, request->request_id, WLH_STATUS_BUSY
        );
    }
    if (!ota_state_is_idle(coproc->ota_state)) {
        /* A transfer already occupies the single slot; the host must ABORT it
           before starting another. */
        return send_ota_status(
            coproc, request->method_id, request->request_id, WLH_STATUS_BUSY
        );
    }

    /* OtaBeginRequest embeds a 512-byte signature, too large for the core's
       bounded stack frame; decode it into a heap scratch buffer. */
    message = (wlh_protocol_v1_OtaBeginRequest *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*message)
    );
    if (message == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    memset(message, 0, sizeof(*message));
    stream = pb_istream_from_buffer(payload, payload_size);
    if (!pb_decode(&stream, wlh_protocol_v1_OtaBeginRequest_fields, message)) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)message
        );
        return WLH_COPROC_PROTOCOL_ERROR;
    }
    if (message->image_size == 0u || message->sha256.size != 32u) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)message
        );
        return send_ota_status(
            coproc,
            request->method_id,
            request->request_id,
            WLH_STATUS_INVALID_ARGUMENT
        );
    }

    transfer_id = ota_next_transfer_id(coproc);
    coproc->ota_transfer_id = transfer_id;
    coproc->ota_image_size = message->image_size;
    coproc->ota_bytes_accepted = 0u;
    coproc->ota_bytes_received = 0u;
    coproc->ota_progress_reported_bytes = 0u;
    memset(coproc->ota_target_version, 0, sizeof(coproc->ota_target_version));
    memcpy(
        coproc->ota_target_version,
        message->target_version,
        sizeof(coproc->ota_target_version) - 1u
    );

    memset(&params, 0, sizeof(params));
    params.image_type = (uint32_t)message->image_type;
    params.image_size = message->image_size;
    memcpy(params.sha256, message->sha256.bytes, sizeof(params.sha256));
    memcpy(
        params.target_version,
        message->target_version,
        sizeof(params.target_version) - 1u
    );
    memcpy(params.slot, message->slot, sizeof(params.slot) - 1u);
    params.signature = message->signature.bytes;
    params.signature_size = message->signature.size;

    operation_id = ota_begin_operation(coproc, request);
    status = coproc->config.ota.begin(
        coproc->config.ota.context, operation_id, transfer_id, &params
    );
    coproc->config.buffers.free(
        coproc->config.buffers.context, (uint8_t *)message
    );
    return ota_submit_result(coproc, request, status);
}

static wlh_coproc_result_t handle_ota_finalize(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    const uint8_t *payload,
    size_t payload_size
) {
    wlh_protocol_v1_OtaFinalizeRequest message =
        wlh_protocol_v1_OtaFinalizeRequest_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
    uint32_t operation_id;
    int status;

    if (!pb_decode(
            &stream, wlh_protocol_v1_OtaFinalizeRequest_fields, &message
        )) {
        return WLH_COPROC_PROTOCOL_ERROR;
    }
    if (coproc->ota_pending.active) {
        return send_ota_status(
            coproc, request->method_id, request->request_id, WLH_STATUS_BUSY
        );
    }
    if (coproc->ota_state != OTA_STATE_RECEIVING) {
        return send_ota_status(
            coproc,
            request->method_id,
            request->request_id,
            WLH_STATUS_NOT_READY
        );
    }
    if (message.transfer_id != coproc->ota_transfer_id) {
        return send_ota_status(
            coproc,
            request->method_id,
            request->request_id,
            WLH_STATUS_NOT_FOUND
        );
    }
    operation_id = ota_begin_operation(coproc, request);
    ota_set_state(coproc, OTA_STATE_VERIFYING);
    status = coproc->config.ota.finalize(
        coproc->config.ota.context,
        operation_id,
        coproc->ota_transfer_id,
        message.bytes_sent
    );
    return ota_submit_result(coproc, request, status);
}

static wlh_coproc_result_t handle_ota_abort(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    const uint8_t *payload,
    size_t payload_size
) {
    wlh_protocol_v1_OtaAbortRequest message =
        wlh_protocol_v1_OtaAbortRequest_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
    uint32_t operation_id;
    int status;

    if (!pb_decode(&stream, wlh_protocol_v1_OtaAbortRequest_fields, &message)) {
        return WLH_COPROC_PROTOCOL_ERROR;
    }
    if (coproc->ota_pending.active) {
        return send_ota_status(
            coproc, request->method_id, request->request_id, WLH_STATUS_BUSY
        );
    }
    if (ota_state_is_idle(coproc->ota_state)) {
        /* Nothing to abort; ABORT is idempotent. */
        return send_ota_status(
            coproc, request->method_id, request->request_id, WLH_STATUS_OK
        );
    }
    if (message.transfer_id != coproc->ota_transfer_id) {
        return send_ota_status(
            coproc,
            request->method_id,
            request->request_id,
            WLH_STATUS_NOT_FOUND
        );
    }
    if (coproc->config.ota.abort == NULL) {
        ota_set_state(coproc, OTA_STATE_IDLE);
        coproc->ota_transfer_id = 0u;
        return send_ota_status(
            coproc, request->method_id, request->request_id, WLH_STATUS_OK
        );
    }
    operation_id = ota_begin_operation(coproc, request);
    status = coproc->config.ota.abort(
        coproc->config.ota.context, operation_id, coproc->ota_transfer_id
    );
    return ota_submit_result(coproc, request, status);
}

static wlh_coproc_result_t handle_ota_activate(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    const uint8_t *payload,
    size_t payload_size
) {
    wlh_protocol_v1_OtaActivateRequest message =
        wlh_protocol_v1_OtaActivateRequest_init_zero;
    pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
    uint32_t operation_id;
    int status;

    if (!pb_decode(
            &stream, wlh_protocol_v1_OtaActivateRequest_fields, &message
        )) {
        return WLH_COPROC_PROTOCOL_ERROR;
    }
    if (coproc->ota_pending.active) {
        return send_ota_status(
            coproc, request->method_id, request->request_id, WLH_STATUS_BUSY
        );
    }
    if (coproc->ota_state != OTA_STATE_READY_TO_ACTIVATE) {
        return send_ota_status(
            coproc,
            request->method_id,
            request->request_id,
            WLH_STATUS_NOT_READY
        );
    }
    if (message.transfer_id != coproc->ota_transfer_id) {
        return send_ota_status(
            coproc,
            request->method_id,
            request->request_id,
            WLH_STATUS_NOT_FOUND
        );
    }
    operation_id = ota_begin_operation(coproc, request);
    status = coproc->config.ota.activate(
        coproc->config.ota.context,
        operation_id,
        coproc->ota_transfer_id,
        message.reboot
    );
    return ota_submit_result(coproc, request, status);
}

static wlh_coproc_result_t handle_ota_query(
    wlh_coproc_t *coproc, const wlh_rpc_envelope_t *request
) {
    wlh_protocol_v1_OtaQueryResponse response =
        wlh_protocol_v1_OtaQueryResponse_init_zero;
    uint32_t state = coproc->ota_state;
    if (state == OTA_STATE_UNSPECIFIED)
        state = OTA_STATE_IDLE;
    response.state = (wlh_protocol_v1_OtaState)state;
    response.transfer_id = coproc->ota_transfer_id;
    response.image_size = coproc->ota_image_size;
    response.bytes_received = coproc->ota_bytes_received;
    memcpy(
        response.target_version,
        coproc->ota_target_version,
        sizeof(response.target_version) - 1u
    );
    return send_rpc_message(
        coproc,
        WLH_SERVICE_OTA,
        WLH_OTA_METHOD_QUERY,
        request->request_id,
        WLH_RPC_KIND_RESPONSE,
        WLH_STATUS_DOMAIN_NONE,
        WLH_STATUS_OK,
        wlh_protocol_v1_OtaQueryResponse_fields,
        &response
    );
}

WLH_NOINLINE wlh_coproc_result_t handle_ota(
    wlh_coproc_t *coproc,
    const wlh_rpc_envelope_t *request,
    const uint8_t *payload,
    size_t payload_size
) {
    switch (request->method_id) {
    case WLH_OTA_METHOD_BEGIN:
        return handle_ota_begin(coproc, request, payload, payload_size);
    case WLH_OTA_METHOD_FINALIZE:
        return handle_ota_finalize(coproc, request, payload, payload_size);
    case WLH_OTA_METHOD_ABORT:
        return handle_ota_abort(coproc, request, payload, payload_size);
    case WLH_OTA_METHOD_ACTIVATE:
        return handle_ota_activate(coproc, request, payload, payload_size);
    case WLH_OTA_METHOD_QUERY:
        return handle_ota_query(coproc, request);
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

static uint64_t ota_read_u64le(const uint8_t *p) {
    return (uint64_t)p[0] | ((uint64_t)p[1] << 8) | ((uint64_t)p[2] << 16) |
           ((uint64_t)p[3] << 24) | ((uint64_t)p[4] << 32) |
           ((uint64_t)p[5] << 40) | ((uint64_t)p[6] << 48) |
           ((uint64_t)p[7] << 56);
}

static uint32_t ota_read_u32le(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t ota_read_u16le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void ota_fail_transfer(wlh_coproc_t *coproc) {
    ota_set_state(coproc, OTA_STATE_FAILED);
}

WLH_NOINLINE wlh_coproc_result_t process_ota_frame(
    wlh_coproc_t *coproc, const uint8_t *payload, size_t payload_size
) {
    wlh_raw_record_iterator_t iterator;
    wlh_raw_record_view_t record;
    const uint8_t *header;
    uint32_t transfer_id;
    uint64_t offset;
    uint16_t data_size;
    const uint8_t *data;
    size_t data_len;
    bool last_chunk;
    int status;

    if (!ota_backend_present(coproc))
        return WLH_COPROC_PROTOCOL_ERROR;

    /* Every OTA_STREAM frame carries exactly one firmware chunk, so one frame
       spends and returns exactly one flow-control credit. A malformed or
       out-of-sequence chunk fails the transfer but still returns the credit so
       the host is never stranded in CONGESTED. */
    if (payload_size == 0u ||
        wlh_raw_record_iterator_init(&iterator, payload, payload_size) !=
            WLH_WIRE_OK ||
        wlh_raw_record_iterator_next(&iterator, &record) != WLH_WIRE_OK) {
        goto reject;
    }
    if (record.record_type != WLH_COPROC_OTA_RECORD_TYPE ||
        record.payload_size < WLH_COPROC_OTA_STREAM_HEADER_SIZE) {
        goto reject;
    }
    {
        wlh_raw_record_view_t extra;
        if (wlh_raw_record_iterator_next(&iterator, &extra) != WLH_WIRE_END) {
            goto reject;
        }
    }

    header = record.payload;
    transfer_id = ota_read_u32le(header);
    offset = ota_read_u64le(header + 4);
    data_size = ota_read_u16le(header + 12);
    data = record.payload + WLH_COPROC_OTA_STREAM_HEADER_SIZE;
    data_len = record.payload_size - WLH_COPROC_OTA_STREAM_HEADER_SIZE;

    if (coproc->ota_state != OTA_STATE_RECEIVING ||
        transfer_id != coproc->ota_transfer_id ||
        (size_t)data_size != data_len || data_len == 0u ||
        data_len > WLH_COPROC_OTA_CHUNK_SIZE ||
        offset != coproc->ota_bytes_accepted ||
        offset > coproc->ota_image_size ||
        data_len > coproc->ota_image_size - offset) {
        goto reject;
    }
    last_chunk = offset + data_len == coproc->ota_image_size;
    if ((offset % WLH_COPROC_OTA_ALIGNMENT) != 0u ||
        (!last_chunk && (data_len % WLH_COPROC_OTA_ALIGNMENT) != 0u)) {
        goto reject;
    }

    status = coproc->config.ota.write(
        coproc->config.ota.context,
        coproc->ota_transfer_id,
        offset,
        data,
        data_len
    );
    if (status != 0) {
        ota_fail_transfer(coproc);
        (void)send_credit_update(coproc, WLH_CHANNEL_OTA_STREAM);
        return WLH_COPROC_BACKEND_ERROR;
    }
    coproc->ota_bytes_accepted += data_len;
    /* Credit returns from wlh_coproc_ota_write_complete() once the chunk is
       durable, pacing the host to the flash write rate. */
    return WLH_COPROC_OK;

reject:
    WLH_LOGW("wlh_coproc", "invalid OTA chunk (%zu bytes)", payload_size);
    if (coproc->ota_state == OTA_STATE_RECEIVING ||
        coproc->ota_state == OTA_STATE_VERIFYING)
        ota_fail_transfer(coproc);
    (void)send_credit_update(coproc, WLH_CHANNEL_OTA_STREAM);
    return WLH_COPROC_PROTOCOL_ERROR;
}

WLH_NOINLINE void ota_operation_completed(
    wlh_coproc_t *coproc, const coproc_ota_complete_job_t *completed
) {
    uint16_t method_id;
    uint32_t request_id;

    if (!coproc->ota_pending.active ||
        completed->operation_id != coproc->ota_pending.operation_id ||
        coproc->ota_pending.session_id != coproc->session_id) {
        return;
    }
    method_id = coproc->ota_pending.method_id;
    request_id = coproc->ota_pending.request_id;
    memset(&coproc->ota_pending, 0, sizeof(coproc->ota_pending));

    if (completed->backend_status != 0) {
        if (method_id != WLH_OTA_METHOD_ABORT)
            ota_set_state(coproc, OTA_STATE_FAILED);
        (void)send_ota_status(
            coproc, method_id, request_id, WLH_STATUS_INTERNAL
        );
        return;
    }

    switch (method_id) {
    case WLH_OTA_METHOD_BEGIN: {
        wlh_protocol_v1_OtaBeginResponse response =
            wlh_protocol_v1_OtaBeginResponse_init_zero;
        ota_set_state(coproc, OTA_STATE_RECEIVING);
        response.transfer_id = coproc->ota_transfer_id;
        response.stream_chunk_size = WLH_COPROC_OTA_CHUNK_SIZE;
        response.stream_alignment = WLH_COPROC_OTA_ALIGNMENT;
        (void)send_rpc_message(
            coproc,
            WLH_SERVICE_OTA,
            WLH_OTA_METHOD_BEGIN,
            request_id,
            WLH_RPC_KIND_RESPONSE,
            WLH_STATUS_DOMAIN_NONE,
            WLH_STATUS_OK,
            wlh_protocol_v1_OtaBeginResponse_fields,
            &response
        );
        return;
    }
    case WLH_OTA_METHOD_FINALIZE:
        ota_set_state(coproc, OTA_STATE_READY_TO_ACTIVATE);
        (void)send_ota_status(
            coproc, WLH_OTA_METHOD_FINALIZE, request_id, WLH_STATUS_OK
        );
        return;
    case WLH_OTA_METHOD_ABORT:
        (void)send_ota_status(
            coproc, WLH_OTA_METHOD_ABORT, request_id, WLH_STATUS_OK
        );
        ota_set_state(coproc, OTA_STATE_IDLE);
        coproc->ota_transfer_id = 0u;
        coproc->ota_bytes_accepted = 0u;
        coproc->ota_bytes_received = 0u;
        return;
    case WLH_OTA_METHOD_ACTIVATE:
        (void)send_ota_status(
            coproc, WLH_OTA_METHOD_ACTIVATE, request_id, WLH_STATUS_OK
        );
        return;
    default:
        return;
    }
}

WLH_NOINLINE void ota_write_completed(
    wlh_coproc_t *coproc, const coproc_ota_write_job_t *completed
) {
    /* Each accepted chunk consumed one credit; return it here so the window
       tracks durable writes. */
    (void)send_credit_update(coproc, WLH_CHANNEL_OTA_STREAM);
    if (completed->transfer_id != coproc->ota_transfer_id ||
        coproc->ota_state != OTA_STATE_RECEIVING) {
        return;
    }
    if (completed->backend_status != 0) {
        ota_fail_transfer(coproc);
        return;
    }
    if (completed->bytes_received > coproc->ota_bytes_received)
        coproc->ota_bytes_received = completed->bytes_received;
    if (coproc->ota_bytes_received == coproc->ota_image_size ||
        coproc->ota_bytes_received - coproc->ota_progress_reported_bytes >=
            OTA_PROGRESS_INTERVAL_BYTES) {
        ota_emit_progress(coproc);
    }
}

static wlh_coproc_result_t ota_enqueue_complete(
    wlh_coproc_t *coproc, uint32_t operation_id, int backend_status
) {
    coproc_ota_complete_job_t *job;
    if (coproc == NULL || operation_id == 0u || !coproc->worker_started)
        return WLH_COPROC_INVALID_ARGUMENT;
    job = (coproc_ota_complete_job_t *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*job)
    );
    if (job == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    job->operation_id = operation_id;
    job->backend_status = backend_status;
    if (enqueue_job(coproc, COPROC_JOB_OTA_COMPLETE, job, WLH_OSAL_NO_WAIT) !=
        0) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)job
        );
        return WLH_COPROC_BACKEND_ERROR;
    }
    return WLH_COPROC_OK;
}

wlh_coproc_result_t wlh_coproc_ota_begin_complete(
    wlh_coproc_t *coproc, uint32_t operation_id, int backend_status
) {
    return ota_enqueue_complete(coproc, operation_id, backend_status);
}

wlh_coproc_result_t wlh_coproc_ota_finalize_complete(
    wlh_coproc_t *coproc, uint32_t operation_id, int backend_status
) {
    return ota_enqueue_complete(coproc, operation_id, backend_status);
}

wlh_coproc_result_t wlh_coproc_ota_abort_complete(
    wlh_coproc_t *coproc, uint32_t operation_id, int backend_status
) {
    return ota_enqueue_complete(coproc, operation_id, backend_status);
}

wlh_coproc_result_t wlh_coproc_ota_activate_complete(
    wlh_coproc_t *coproc, uint32_t operation_id, int backend_status
) {
    return ota_enqueue_complete(coproc, operation_id, backend_status);
}

wlh_coproc_result_t wlh_coproc_ota_write_complete(
    wlh_coproc_t *coproc,
    uint32_t transfer_id,
    uint64_t bytes_received,
    int backend_status
) {
    coproc_ota_write_job_t *job;
    if (coproc == NULL || !coproc->worker_started)
        return WLH_COPROC_INVALID_ARGUMENT;
    job = (coproc_ota_write_job_t *)coproc->config.buffers.alloc(
        coproc->config.buffers.context, sizeof(*job)
    );
    if (job == NULL)
        return WLH_COPROC_BACKEND_ERROR;
    job->transfer_id = transfer_id;
    job->bytes_received = bytes_received;
    job->backend_status = backend_status;
    if (enqueue_job(
            coproc, COPROC_JOB_OTA_WRITE_COMPLETE, job, WLH_OSAL_NO_WAIT
        ) != 0) {
        coproc->config.buffers.free(
            coproc->config.buffers.context, (uint8_t *)job
        );
        return WLH_COPROC_BACKEND_ERROR;
    }
    return WLH_COPROC_OK;
}
