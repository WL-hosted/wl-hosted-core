#include "coproc_test_support.h"

#include "ota.pb.h"

/* Poll the sent-frame ring for a response/event on the OTA service whose
   request_id matches, tolerating interleaved progress events that make the
   last-sent frame unreliable. */
static bool wait_ota_rpc(
    fixture_t *f,
    uint16_t method,
    wlh_rpc_kind_t kind,
    uint32_t request_id,
    wlh_rpc_envelope_t *rpc,
    const uint8_t **payload,
    size_t *payload_size
) {
    unsigned attempt;
    for (attempt = 0; attempt < 1000u; ++attempt) {
        if (find_sent_rpc(
                f, WLH_SERVICE_OTA, method, kind, rpc, payload, payload_size
            ) &&
            rpc->request_id == request_id) {
            return true;
        }
        wait_milliseconds(1u);
    }
    CHECK(false);
    return false;
}

static void expect_ota_status(
    fixture_t *f, uint16_t method, uint32_t request_id, int16_t status_code
) {
    wlh_rpc_envelope_t rpc;
    const uint8_t *payload;
    size_t payload_size;
    if (!wait_ota_rpc(
            f,
            method,
            WLH_RPC_KIND_RESPONSE,
            request_id,
            &rpc,
            &payload,
            &payload_size
        )) {
        return;
    }
    CHECK(rpc.status_code == status_code);
    CHECK(
        rpc.status_domain == (status_code == WLH_STATUS_OK
                                  ? WLH_STATUS_DOMAIN_NONE
                                  : WLH_STATUS_DOMAIN_OTA)
    );
}

/* Wait until an OTA_STREAM credit update is observed in the ring. */
static void expect_ota_credit(fixture_t *f) {
    wlh_rpc_envelope_t rpc;
    const uint8_t *payload;
    size_t payload_size;
    unsigned attempt;
    for (attempt = 0; attempt < 1000u; ++attempt) {
        if (find_sent_rpc(
                f,
                WLH_SERVICE_LINK,
                WLH_LINK_METHOD_CREDIT_UPDATE,
                WLH_RPC_KIND_EVENT,
                &rpc,
                &payload,
                &payload_size
            )) {
            wlh_protocol_v1_CreditUpdate update =
                wlh_protocol_v1_CreditUpdate_init_zero;
            pb_istream_t stream = pb_istream_from_buffer(payload, payload_size);
            if (pb_decode(
                    &stream, wlh_protocol_v1_CreditUpdate_fields, &update
                ) &&
                update.channel_id == WLH_CHANNEL_OTA_STREAM &&
                update.units == 1u) {
                return;
            }
        }
        wait_milliseconds(1u);
    }
    CHECK(false);
}

static void query_ota(
    fixture_t *f,
    wlh_coproc_t *core,
    uint32_t request_id,
    wlh_protocol_v1_OtaQueryResponse *out
) {
    wlh_rpc_envelope_t rpc;
    const uint8_t *payload;
    size_t payload_size;
    pb_istream_t stream;
    wlh_protocol_v1_Empty empty = wlh_protocol_v1_Empty_init_zero;

    ota_send_request(
        core,
        WLH_OTA_METHOD_QUERY,
        request_id,
        wlh_protocol_v1_Empty_fields,
        &empty
    );
    if (!wait_ota_rpc(
            f,
            WLH_OTA_METHOD_QUERY,
            WLH_RPC_KIND_RESPONSE,
            request_id,
            &rpc,
            &payload,
            &payload_size
        )) {
        return;
    }
    CHECK(rpc.status_code == WLH_STATUS_OK);
    *out = (wlh_protocol_v1_OtaQueryResponse)
        wlh_protocol_v1_OtaQueryResponse_init_zero;
    stream = pb_istream_from_buffer(payload, payload_size);
    CHECK(pb_decode(&stream, wlh_protocol_v1_OtaQueryResponse_fields, out));
}

static void fill_begin_request(
    wlh_protocol_v1_OtaBeginRequest *request, uint64_t image_size
) {
    unsigned i;
    *request = (wlh_protocol_v1_OtaBeginRequest)
        wlh_protocol_v1_OtaBeginRequest_init_zero;
    request->image_type =
        wlh_protocol_v1_OtaImageType_OTA_IMAGE_TYPE_COPROCESSOR_FIRMWARE;
    request->image_size = image_size;
    request->sha256.size = 32u;
    for (i = 0; i < 32u; ++i)
        request->sha256.bytes[i] = (uint8_t)(i + 1u);
    snprintf(request->target_version, sizeof(request->target_version), "1.2.3");
    snprintf(request->slot, sizeof(request->slot), "ota_1");
}

/* Drive a BEGIN to completion and confirm the transfer enters RECEIVING with
   the advertised chunk geometry, draining the RECEIVING progress event so the
   caller can make deterministic assertions afterwards. */
static uint32_t begin_transfer(
    fixture_t *f, wlh_coproc_t *core, uint32_t request_id, uint64_t image_size
) {
    wlh_protocol_v1_OtaBeginRequest request;
    wlh_protocol_v1_OtaBeginResponse response =
        wlh_protocol_v1_OtaBeginResponse_init_zero;
    wlh_rpc_envelope_t rpc;
    const uint8_t *payload;
    size_t payload_size;
    pb_istream_t stream;
    unsigned attempt;

    fill_begin_request(&request, image_size);
    ota_send_request(
        core,
        WLH_OTA_METHOD_BEGIN,
        request_id,
        wlh_protocol_v1_OtaBeginRequest_fields,
        &request
    );
    if (!wait_ota_rpc(
            f,
            WLH_OTA_METHOD_BEGIN,
            WLH_RPC_KIND_RESPONSE,
            request_id,
            &rpc,
            &payload,
            &payload_size
        )) {
        return 0u;
    }
    CHECK(rpc.status_code == WLH_STATUS_OK);
    stream = pb_istream_from_buffer(payload, payload_size);
    CHECK(
        pb_decode(&stream, wlh_protocol_v1_OtaBeginResponse_fields, &response)
    );
    CHECK(response.stream_chunk_size == WLH_COPROC_OTA_CHUNK_SIZE);
    CHECK(response.stream_alignment == WLH_COPROC_OTA_ALIGNMENT);

    /* Drain the RECEIVING progress event so later no-credit assertions are not
       fooled by a late progress frame. */
    for (attempt = 0; attempt < 1000u; ++attempt) {
        if (find_sent_rpc(
                f,
                WLH_SERVICE_OTA,
                WLH_OTA_EVENT_PROGRESS,
                WLH_RPC_KIND_EVENT,
                &rpc,
                &payload,
                &payload_size
            )) {
            break;
        }
        wait_milliseconds(1u);
    }
    CHECK(attempt < 1000u);
    return response.transfer_id;
}

void test_ota_hello_advertisement(void) {
    fixture_t f;
    wlh_coproc_t core;
    wlh_rpc_envelope_t rpc;
    const uint8_t *rpc_payload;
    size_t rpc_payload_size;
    pb_istream_t stream;
    wlh_protocol_v1_HelloResponse response =
        wlh_protocol_v1_HelloResponse_init_zero;

    memset(&f, 0, sizeof(f));
    f.core = &core;
    wlh_posix_osal_init(&f.posix);
    prepare_ready_ota_core(&f, &core);

    decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
    stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
    CHECK(pb_decode(&stream, wlh_protocol_v1_HelloResponse_fields, &response));

    CHECK(
        response.services_count == 1u &&
        response.services[0].service_id == WLH_SERVICE_OTA &&
        response.services[0].major == 1u
    );
    CHECK(
        response.channels_count == 1u &&
        response.channels[0].channel_id == WLH_CHANNEL_OTA_STREAM &&
        response.channels[0].max_frame_payload ==
            WLH_COPROC_OTA_STREAM_HEADER_SIZE + WLH_COPROC_OTA_CHUNK_SIZE &&
        response.channels[0].alignment == WLH_COPROC_OTA_ALIGNMENT
    );
    /* Base channels 0-3 occupy indices 0-3; OTA lands at the cursor. */
    CHECK(response.initial_credits_count == 5u);
    CHECK(
        response.initial_credits[4].channel_id == WLH_CHANNEL_OTA_STREAM &&
        response.initial_credits[4].units == WLH_COPROC_OTA_INITIAL_CREDIT &&
        response.initial_credits[4].unit_bytes == 1u
    );

    CHECK(wlh_coproc_stop(&core) == WLH_COPROC_OK);
}

/* Bring up a core exposing both the Bluetooth and OTA backends with the ADV
   channel negotiated, to verify the Hello cursor packs OTA after Bluetooth. */
static void prepare_ready_bt_ota_core(fixture_t *f, wlh_coproc_t *core) {
    wlh_coproc_config_t config;
    uint8_t incoming[4096];
    size_t incoming_size;
    wlh_protocol_v1_HelloRequest hello = wlh_protocol_v1_HelloRequest_init_zero;

    f->core = core;
    f->ota_auto_complete = true;
    memset(&config, 0, sizeof(config));
    config.port.context = f;
    config.port.submit_tx = submit_frame;
    config.port.ethernet_sta_rx = ethernet_sta_rx;
    config.buffers = (wlh_coproc_buffer_ops_t){f, buffer_alloc, buffer_free};
    config.osal = wlh_posix_osal_ops(&f->posix);
    config.bluetooth.context = f;
    config.bluetooth.initialize = bt_initialize;
    config.bluetooth.enable = bt_enable;
    config.bluetooth.disable = bt_disable;
    config.bluetooth.deinitialize = bt_deinitialize;
    config.bluetooth.get_info = bt_get_info;
    config.bluetooth.hci_send = bt_hci_send;
    config.bluetooth.hci_tx_ready = bt_hci_tx_ready;
    config.ota.context = f;
    config.ota.begin = ota_begin;
    config.ota.write = ota_write;
    config.ota.finalize = ota_finalize;
    config.ota.abort = ota_abort;
    config.ota.activate = ota_activate;
    config.max_frame_size = 4096;
    config.heartbeat_interval_ms = 60000;
    config.initial_credit = 64;
    config.initial_session_id = 42;
    config.core_queue_depth = 8u;

    CHECK(wlh_coproc_init(core, &config) == WLH_COPROC_OK);
    CHECK(wlh_coproc_start(core) == WLH_COPROC_OK);
    wait_for_state(core, WLH_COPROC_STATE_WAITING_FOR_HELLO);

    hello.protocol_versions_count = 1;
    hello.protocol_versions[0].major = 1;
    hello.max_frame_size = 4096;
    hello.channels_count = 2u;
    hello.channels[0].channel_id = WLH_CHANNEL_BLUETOOTH_HCI;
    hello.channels[0].max_frame_payload = 4096u;
    hello.channels[1].channel_id = WLH_CHANNEL_BLUETOOTH_HCI_ADV;
    hello.channels[1].max_frame_payload = 4096u;
    incoming_size = make_rpc_frame(
        incoming,
        0,
        0,
        WLH_SERVICE_LINK,
        WLH_LINK_METHOD_HELLO,
        7,
        wlh_protocol_v1_HelloRequest_fields,
        &hello
    );
    CHECK(wlh_coproc_on_frame(core, incoming, incoming_size) == WLH_COPROC_OK);
    wait_for_state(core, WLH_COPROC_STATE_READY);
    wait_for_sent(f, 1u);
}

void test_ota_hello_with_bluetooth(void) {
    fixture_t f;
    wlh_coproc_t core;
    wlh_rpc_envelope_t rpc;
    const uint8_t *rpc_payload;
    size_t rpc_payload_size;
    pb_istream_t stream;
    wlh_protocol_v1_HelloResponse response =
        wlh_protocol_v1_HelloResponse_init_zero;

    memset(&f, 0, sizeof(f));
    f.core = &core;
    wlh_posix_osal_init(&f.posix);
    prepare_ready_bt_ota_core(&f, &core);

    decode_last_sent(&f, &rpc, &rpc_payload, &rpc_payload_size);
    stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
    CHECK(pb_decode(&stream, wlh_protocol_v1_HelloResponse_fields, &response));

    /* Bluetooth occupies the leading cursor slots; OTA follows without a gap.
     */
    CHECK(
        response.services_count == 2u &&
        response.services[0].service_id == WLH_SERVICE_BLUETOOTH &&
        response.services[1].service_id == WLH_SERVICE_OTA &&
        response.services[1].major == 1u
    );
    CHECK(
        response.channels_count == 3u &&
        response.channels[0].channel_id == WLH_CHANNEL_BLUETOOTH_HCI &&
        response.channels[1].channel_id == WLH_CHANNEL_BLUETOOTH_HCI_ADV &&
        response.channels[2].channel_id == WLH_CHANNEL_OTA_STREAM &&
        response.channels[2].max_frame_payload ==
            WLH_COPROC_OTA_STREAM_HEADER_SIZE + WLH_COPROC_OTA_CHUNK_SIZE
    );
    /* Indices 0-3 base, 4 HCI, 5 ADV, 6 OTA. */
    CHECK(response.initial_credits_count == 7u);
    CHECK(
        response.initial_credits[6].channel_id == WLH_CHANNEL_OTA_STREAM &&
        response.initial_credits[6].units == WLH_COPROC_OTA_INITIAL_CREDIT
    );

    CHECK(wlh_coproc_stop(&core) == WLH_COPROC_OK);
}

void test_ota_full_flow(void) {
    fixture_t f;
    wlh_coproc_t core;
    uint32_t transfer_id;
    unsigned sent_before;
    const uint64_t image_size = 2u * WLH_COPROC_OTA_CHUNK_SIZE + 32u;
    static uint8_t chunk[WLH_COPROC_OTA_CHUNK_SIZE];
    unsigned i;
    wlh_protocol_v1_OtaFinalizeRequest finalize =
        wlh_protocol_v1_OtaFinalizeRequest_init_zero;
    wlh_protocol_v1_OtaActivateRequest activate =
        wlh_protocol_v1_OtaActivateRequest_init_zero;

    for (i = 0; i < sizeof(chunk); ++i)
        chunk[i] = (uint8_t)(i & 0xffu);

    memset(&f, 0, sizeof(f));
    f.core = &core;
    wlh_posix_osal_init(&f.posix);
    prepare_ready_ota_core(&f, &core);

    transfer_id = begin_transfer(&f, &core, 1000u, image_size);
    CHECK(transfer_id == 1u);
    CHECK(f.ota_begins == 1u);
    CHECK(f.ota_last_params.image_size == image_size);
    CHECK(
        f.ota_last_params.sha256[0] == 1u && f.ota_last_params.sha256[31] == 32u
    );

    /* Chunk 0: accepted but no credit returned until the durable write. */
    sent_before = f.sent_count;
    ota_stream_send(
        &core, 42u, transfer_id, 0u, chunk, WLH_COPROC_OTA_CHUNK_SIZE
    );
    wait_for_counter(&f.ota_writes, 1u);
    CHECK(f.ota_last_offset == 0u);
    CHECK(f.ota_last_data_size == WLH_COPROC_OTA_CHUNK_SIZE);
    CHECK(f.sent_count == sent_before);

    /* write_complete returns exactly one credit and no progress yet. */
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_ota_write_complete(
            &core, transfer_id, WLH_COPROC_OTA_CHUNK_SIZE, 0
        ) == WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);
    {
        wlh_rpc_envelope_t rpc;
        const uint8_t *payload;
        size_t payload_size;
        wlh_protocol_v1_CreditUpdate update =
            wlh_protocol_v1_CreditUpdate_init_zero;
        pb_istream_t stream;
        decode_last_sent(&f, &rpc, &payload, &payload_size);
        CHECK(
            rpc.service_id == WLH_SERVICE_LINK &&
            rpc.method_id == WLH_LINK_METHOD_CREDIT_UPDATE &&
            rpc.kind == WLH_RPC_KIND_EVENT
        );
        stream = pb_istream_from_buffer(payload, payload_size);
        CHECK(pb_decode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update));
        CHECK(
            update.channel_id == WLH_CHANNEL_OTA_STREAM && update.units == 1u
        );
    }

    /* Chunk 1. */
    sent_before = f.sent_count;
    ota_stream_send(
        &core,
        42u,
        transfer_id,
        WLH_COPROC_OTA_CHUNK_SIZE,
        chunk,
        WLH_COPROC_OTA_CHUNK_SIZE
    );
    wait_for_counter(&f.ota_writes, 2u);
    CHECK(f.sent_count == sent_before);
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_ota_write_complete(
            &core, transfer_id, 2u * WLH_COPROC_OTA_CHUNK_SIZE, 0
        ) == WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 1u);

    /* Final chunk completes the image and triggers a completion progress. */
    sent_before = f.sent_count;
    ota_stream_send(
        &core, 42u, transfer_id, 2u * WLH_COPROC_OTA_CHUNK_SIZE, chunk, 32u
    );
    wait_for_counter(&f.ota_writes, 3u);
    CHECK(f.ota_last_offset == 2u * WLH_COPROC_OTA_CHUNK_SIZE);
    CHECK(f.ota_last_data_size == 32u);
    CHECK(f.sent_count == sent_before);
    sent_before = f.sent_count;
    CHECK(
        wlh_coproc_ota_write_complete(&core, transfer_id, image_size, 0) ==
        WLH_COPROC_OK
    );
    wait_for_sent(&f, sent_before + 2u);
    {
        wlh_rpc_envelope_t rpc;
        const uint8_t *payload;
        size_t payload_size;
        wlh_protocol_v1_OtaProgressEvent event =
            wlh_protocol_v1_OtaProgressEvent_init_zero;
        pb_istream_t stream;
        unsigned attempt;
        for (attempt = 0; attempt < 1000u; ++attempt) {
            if (find_sent_rpc(
                    &f,
                    WLH_SERVICE_OTA,
                    WLH_OTA_EVENT_PROGRESS,
                    WLH_RPC_KIND_EVENT,
                    &rpc,
                    &payload,
                    &payload_size
                )) {
                stream = pb_istream_from_buffer(payload, payload_size);
                CHECK(pb_decode(
                    &stream, wlh_protocol_v1_OtaProgressEvent_fields, &event
                ));
                if (event.bytes_received == image_size)
                    break;
            }
            wait_milliseconds(1u);
        }
        CHECK(event.bytes_received == image_size);
    }

    /* FINALIZE moves to READY_TO_ACTIVATE. */
    finalize.transfer_id = transfer_id;
    finalize.bytes_sent = image_size;
    ota_send_request(
        &core,
        WLH_OTA_METHOD_FINALIZE,
        1001u,
        wlh_protocol_v1_OtaFinalizeRequest_fields,
        &finalize
    );
    expect_ota_status(&f, WLH_OTA_METHOD_FINALIZE, 1001u, WLH_STATUS_OK);
    CHECK(f.ota_finalizes == 1u && f.ota_last_bytes_sent == image_size);

    /* ACTIVATE with reboot reaches the backend and acknowledges. */
    activate.transfer_id = transfer_id;
    activate.reboot = true;
    ota_send_request(
        &core,
        WLH_OTA_METHOD_ACTIVATE,
        1002u,
        wlh_protocol_v1_OtaActivateRequest_fields,
        &activate
    );
    expect_ota_status(&f, WLH_OTA_METHOD_ACTIVATE, 1002u, WLH_STATUS_OK);
    CHECK(f.ota_activates == 1u && f.ota_last_reboot);

    CHECK(wlh_coproc_stop(&core) == WLH_COPROC_OK);
}

void test_ota_errors_idle(void) {
    fixture_t f;
    wlh_coproc_t core;
    wlh_protocol_v1_OtaQueryResponse query =
        wlh_protocol_v1_OtaQueryResponse_init_zero;
    wlh_protocol_v1_OtaBeginRequest begin;
    wlh_protocol_v1_OtaFinalizeRequest finalize =
        wlh_protocol_v1_OtaFinalizeRequest_init_zero;
    wlh_protocol_v1_OtaActivateRequest activate =
        wlh_protocol_v1_OtaActivateRequest_init_zero;
    wlh_protocol_v1_OtaAbortRequest abort =
        wlh_protocol_v1_OtaAbortRequest_init_zero;

    memset(&f, 0, sizeof(f));
    f.core = &core;
    wlh_posix_osal_init(&f.posix);
    prepare_ready_ota_core(&f, &core);

    /* QUERY on an untouched core reports IDLE. */
    query_ota(&f, &core, 2000u, &query);
    CHECK(query.state == wlh_protocol_v1_OtaState_OTA_STATE_IDLE);
    CHECK(query.transfer_id == 0u && query.bytes_received == 0u);

    /* A zero-length image is rejected before reaching the backend. */
    fill_begin_request(&begin, 0u);
    ota_send_request(
        &core,
        WLH_OTA_METHOD_BEGIN,
        2001u,
        wlh_protocol_v1_OtaBeginRequest_fields,
        &begin
    );
    expect_ota_status(
        &f, WLH_OTA_METHOD_BEGIN, 2001u, WLH_STATUS_INVALID_ARGUMENT
    );
    CHECK(f.ota_begins == 0u);

    /* FINALIZE / ACTIVATE are not ready without a transfer in flight. */
    finalize.transfer_id = 1u;
    ota_send_request(
        &core,
        WLH_OTA_METHOD_FINALIZE,
        2002u,
        wlh_protocol_v1_OtaFinalizeRequest_fields,
        &finalize
    );
    expect_ota_status(&f, WLH_OTA_METHOD_FINALIZE, 2002u, WLH_STATUS_NOT_READY);

    activate.transfer_id = 1u;
    ota_send_request(
        &core,
        WLH_OTA_METHOD_ACTIVATE,
        2003u,
        wlh_protocol_v1_OtaActivateRequest_fields,
        &activate
    );
    expect_ota_status(&f, WLH_OTA_METHOD_ACTIVATE, 2003u, WLH_STATUS_NOT_READY);

    /* ABORT with nothing in flight is an idempotent OK. */
    abort.transfer_id = 1u;
    ota_send_request(
        &core,
        WLH_OTA_METHOD_ABORT,
        2004u,
        wlh_protocol_v1_OtaAbortRequest_fields,
        &abort
    );
    expect_ota_status(&f, WLH_OTA_METHOD_ABORT, 2004u, WLH_STATUS_OK);
    CHECK(f.ota_aborts == 0u);

    CHECK(wlh_coproc_stop(&core) == WLH_COPROC_OK);
}

void test_ota_transfer_errors(void) {
    fixture_t f;
    wlh_coproc_t core;
    uint32_t transfer_id;
    unsigned sent_before;
    const uint64_t image_size = 2u * WLH_COPROC_OTA_CHUNK_SIZE;
    static uint8_t chunk[WLH_COPROC_OTA_CHUNK_SIZE];
    unsigned i;
    wlh_protocol_v1_OtaBeginRequest begin;
    wlh_protocol_v1_OtaFinalizeRequest finalize =
        wlh_protocol_v1_OtaFinalizeRequest_init_zero;
    wlh_protocol_v1_OtaAbortRequest abort =
        wlh_protocol_v1_OtaAbortRequest_init_zero;
    wlh_protocol_v1_OtaQueryResponse query =
        wlh_protocol_v1_OtaQueryResponse_init_zero;

    for (i = 0; i < sizeof(chunk); ++i)
        chunk[i] = (uint8_t)(i & 0xffu);

    memset(&f, 0, sizeof(f));
    f.core = &core;
    wlh_posix_osal_init(&f.posix);
    prepare_ready_ota_core(&f, &core);

    transfer_id = begin_transfer(&f, &core, 3000u, image_size);
    CHECK(transfer_id == 1u);

    /* A second BEGIN while a transfer occupies the slot is refused. */
    fill_begin_request(&begin, image_size);
    ota_send_request(
        &core,
        WLH_OTA_METHOD_BEGIN,
        3001u,
        wlh_protocol_v1_OtaBeginRequest_fields,
        &begin
    );
    expect_ota_status(&f, WLH_OTA_METHOD_BEGIN, 3001u, WLH_STATUS_BUSY);
    CHECK(f.ota_begins == 1u);

    /* FINALIZE for an unknown transfer id is NOT_FOUND. */
    finalize.transfer_id = transfer_id + 99u;
    finalize.bytes_sent = image_size;
    ota_send_request(
        &core,
        WLH_OTA_METHOD_FINALIZE,
        3002u,
        wlh_protocol_v1_OtaFinalizeRequest_fields,
        &finalize
    );
    expect_ota_status(&f, WLH_OTA_METHOD_FINALIZE, 3002u, WLH_STATUS_NOT_FOUND);

    /* ABORT of the live transfer reaches the backend and returns to IDLE. */
    abort.transfer_id = transfer_id;
    ota_send_request(
        &core,
        WLH_OTA_METHOD_ABORT,
        3003u,
        wlh_protocol_v1_OtaAbortRequest_fields,
        &abort
    );
    expect_ota_status(&f, WLH_OTA_METHOD_ABORT, 3003u, WLH_STATUS_OK);
    CHECK(f.ota_aborts == 1u);
    query_ota(&f, &core, 3004u, &query);
    CHECK(query.state == wlh_protocol_v1_OtaState_OTA_STATE_IDLE);
    CHECK(query.transfer_id == 0u);

    /* Start a fresh transfer, then feed an out-of-order chunk. */
    transfer_id = begin_transfer(&f, &core, 3005u, image_size);
    CHECK(transfer_id == 2u);

    ota_stream_send(
        &core,
        42u,
        transfer_id,
        WLH_COPROC_OTA_ALIGNMENT,
        chunk,
        WLH_COPROC_OTA_CHUNK_SIZE
    );
    expect_ota_credit(&f);
    query_ota(&f, &core, 3006u, &query);
    CHECK(query.state == wlh_protocol_v1_OtaState_OTA_STATE_FAILED);
    CHECK(f.ota_writes == 0u);

    /* A chunk arriving after failure is still credited so the host is never
       stranded in CONGESTED; exactly one credit frame and no progress. */
    sent_before = f.sent_count;
    ota_stream_send(
        &core, 42u, transfer_id, 0u, chunk, WLH_COPROC_OTA_CHUNK_SIZE
    );
    wait_for_sent(&f, sent_before + 1u);
    {
        wlh_rpc_envelope_t rpc;
        const uint8_t *payload;
        size_t payload_size;
        wlh_protocol_v1_CreditUpdate update =
            wlh_protocol_v1_CreditUpdate_init_zero;
        pb_istream_t stream;
        decode_last_sent(&f, &rpc, &payload, &payload_size);
        CHECK(
            rpc.service_id == WLH_SERVICE_LINK &&
            rpc.method_id == WLH_LINK_METHOD_CREDIT_UPDATE
        );
        stream = pb_istream_from_buffer(payload, payload_size);
        CHECK(pb_decode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update));
        CHECK(
            update.channel_id == WLH_CHANNEL_OTA_STREAM && update.units == 1u
        );
    }
    CHECK(f.ota_writes == 0u);

    CHECK(wlh_coproc_stop(&core) == WLH_COPROC_OK);
}
