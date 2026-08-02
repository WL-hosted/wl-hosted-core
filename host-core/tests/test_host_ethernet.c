#include "host_test_support.h"

#include "../src/host_internal.h"

/* Build an Ethernet TX job holding one raw record, bypassing the admission
 * path so the test can control exactly which jobs are queued. */
static wlh_host_data_job_t *make_ethernet_job(
    wlh_host_t *host, uint8_t channel, const uint8_t *payload, size_t size
) {
    wlh_host_data_job_t *job =
        (wlh_host_data_job_t *)host->config.buffers.alloc(
            host->config.buffers.context,
            sizeof(*job) + WLH_RAW_RECORD_HEADER_SIZE + size
        );
    size_t record_size = 0u;
    assert(job != NULL);
    if (job == NULL)
        return NULL;
    memset(job, 0, sizeof(*job));
    job->channel = channel;
    if (wlh_raw_record_encode(
            job->data,
            WLH_RAW_RECORD_HEADER_SIZE + size,
            &record_size,
            1u,
            0u,
            payload,
            size
        ) != WLH_WIRE_OK) {
        host->config.buffers.free(host->config.buffers.context, (uint8_t *)job);
        return NULL;
    }
    job->size = record_size;
    job->capacity = record_size;
    return job;
}

void test_ap_ethernet(void) {
    fixture_t fixture;
    uint8_t frame[4096];
    uint8_t raw[11] = {1u, 0u, 8u, 0u, 3u, 0u, 0u, 0u, 1u, 2u, 3u};
    uint8_t ethernet[60] = {0};
    wlh_frame_header_t header;
    wlh_rpc_envelope_t rpc;
    wlh_protocol_v1_CreditUpdate update =
        wlh_protocol_v1_CreditUpdate_init_zero;
    const uint8_t *payload;
    const uint8_t *rpc_payload;
    size_t frame_size = 0u;
    size_t payload_size = 0u;
    size_t rpc_payload_size = 0u;
    pb_istream_t stream;
    unsigned events_before;
    unsigned tx_before;

    fixture_init(&fixture);
    establish_ready(&fixture);
    events_before = fixture.events;
    tx_before = fixture.tx_count;
    wlh_frame_header_init(&header, WLH_CHANNEL_ETHERNET_AP);
    header.session_id = 42u;
    assert(
        wlh_frame_encode(
            frame, sizeof(frame), &frame_size, &header, raw, sizeof(raw)
        ) == WLH_WIRE_OK
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    while (fixture.events == events_before)
        wait_milliseconds(1u);
    fixture.now += 1u;
    wait_for_tx(&fixture, tx_before + 1u);
    assert(fixture.last_event_kind == WLH_HOST_EVENT_ETHERNET_AP_RX);
    assert(
        fixture.last_event_payload_size == 3u &&
        memcmp(fixture.last_event_payload, raw + 8u, 3u) == 0
    );
    assert(
        wlh_frame_decode(
            &header,
            &payload,
            &payload_size,
            fixture.tx,
            fixture.tx_size,
            sizeof(fixture.tx)
        ) == WLH_WIRE_OK
    );
    assert(header.channel == WLH_CHANNEL_LINK_CONTROL);
    assert(
        wlh_rpc_decode(
            &rpc,
            &rpc_payload,
            &rpc_payload_size,
            payload,
            payload_size,
            sizeof(fixture.tx)
        ) == WLH_WIRE_OK
    );
    assert(
        rpc.service_id == WLH_SERVICE_LINK &&
        rpc.method_id == WLH_LINK_METHOD_CREDIT_UPDATE &&
        rpc.kind == WLH_RPC_KIND_EVENT
    );
    stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
    assert(pb_decode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update));
    assert(update.channel_id == WLH_CHANNEL_ETHERNET_AP && update.units == 1u);

    /*
     * Dropping an event because the application executor is full must not
     * permanently consume the peer's transport credit.
     */
    tx_before = fixture.tx_count;
    events_before = fixture.events;
    fixture.reject_executor = true;
    wlh_frame_header_init(&header, WLH_CHANNEL_ETHERNET_AP);
    header.session_id = 42u;
    header.sequence = 1u;
    assert(
        wlh_frame_encode(
            frame, sizeof(frame), &frame_size, &header, raw, sizeof(raw)
        ) == WLH_WIRE_OK
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);
    while (fixture.host.ethernet_rx_pending_credit[1] == 0u)
        wait_milliseconds(1u);
    fixture.now += 1u;
    wait_for_tx(&fixture, tx_before + 1u);
    assert(fixture.events == events_before);
    fixture.reject_executor = false;
    assert(
        wlh_frame_decode(
            &header,
            &payload,
            &payload_size,
            fixture.tx,
            fixture.tx_size,
            sizeof(fixture.tx)
        ) == WLH_WIRE_OK
    );
    assert(header.channel == WLH_CHANNEL_LINK_CONTROL);
    assert(
        wlh_rpc_decode(
            &rpc,
            &rpc_payload,
            &rpc_payload_size,
            payload,
            payload_size,
            sizeof(fixture.tx)
        ) == WLH_WIRE_OK
    );
    assert(
        rpc.service_id == WLH_SERVICE_LINK &&
        rpc.method_id == WLH_LINK_METHOD_CREDIT_UPDATE
    );

    tx_before = fixture.tx_count;
    assert(
        wlh_host_ethernet_ap_send(&fixture.host, ethernet, sizeof(ethernet)) ==
        WLH_HOST_OK
    );
    wait_for_tx(&fixture, tx_before + 1u);
    assert(
        wlh_frame_decode(
            &header,
            &payload,
            &payload_size,
            fixture.tx,
            fixture.tx_size,
            sizeof(fixture.tx)
        ) == WLH_WIRE_OK
    );
    assert(header.channel == WLH_CHANNEL_ETHERNET_AP);
    assert(payload_size == sizeof(ethernet) + 8u);
    assert(wlh_host_stop(&fixture.host) == WLH_HOST_OK);
}

/*
 * The peer charges one credit unit per raw record, and the coprocessor
 * aggregates several Ethernet frames into one wire frame. Returning a single
 * unit for an aggregated frame leaks records-1 credits every time, which
 * drains the channel monotonically and stalls the data path. Verify the
 * returned unit count tracks the record count rather than the frame count.
 */
void test_ethernet_aggregated_credit_accounting(void) {
    fixture_t fixture;
    uint8_t frame[4096];
    uint8_t payload_buffer[256];
    /* Three raw records in one frame payload; 4-byte body each. */
    static const uint8_t bodies[3][4] = {
        {0xA1u, 0xA2u, 0xA3u, 0xA4u},
        {0xB1u, 0xB2u, 0xB3u, 0xB4u},
        {0xC1u, 0xC2u, 0xC3u, 0xC4u}
    };
    wlh_frame_header_t header;
    wlh_rpc_envelope_t rpc;
    wlh_protocol_v1_CreditUpdate update =
        wlh_protocol_v1_CreditUpdate_init_zero;
    const uint8_t *payload;
    const uint8_t *rpc_payload;
    size_t frame_size = 0u;
    size_t payload_size = 0u;
    size_t rpc_payload_size = 0u;
    size_t aggregate_size = 0u;
    unsigned index;
    unsigned events_before;
    unsigned tx_before;
    pb_istream_t stream;

    fixture_init(&fixture);
    establish_ready(&fixture);

    for (index = 0u; index < 3u; ++index) {
        size_t record_size = 0u;
        assert(
            wlh_raw_record_encode(
                payload_buffer + aggregate_size,
                sizeof(payload_buffer) - aggregate_size,
                &record_size,
                1u,
                0u,
                bodies[index],
                sizeof(bodies[index])
            ) == WLH_WIRE_OK
        );
        aggregate_size += record_size;
    }

    events_before = fixture.events;
    tx_before = fixture.tx_count;
    wlh_frame_header_init(&header, WLH_CHANNEL_ETHERNET_AP);
    header.session_id = 42u;
    assert(
        wlh_frame_encode(
            frame,
            sizeof(frame),
            &frame_size,
            &header,
            payload_buffer,
            aggregate_size
        ) == WLH_WIRE_OK
    );
    assert(wlh_host_on_frame(&fixture.host, frame, frame_size) == WLH_HOST_OK);

    /* All three records must be delivered upward. */
    while (fixture.events < events_before + 3u)
        wait_milliseconds(1u);
    assert(fixture.events == events_before + 3u);

    fixture.now += 1u;
    wait_for_tx(&fixture, tx_before + 1u);
    assert(
        wlh_frame_decode(
            &header,
            &payload,
            &payload_size,
            fixture.tx,
            fixture.tx_size,
            sizeof(fixture.tx)
        ) == WLH_WIRE_OK
    );
    assert(header.channel == WLH_CHANNEL_LINK_CONTROL);
    assert(
        wlh_rpc_decode(
            &rpc,
            &rpc_payload,
            &rpc_payload_size,
            payload,
            payload_size,
            sizeof(fixture.tx)
        ) == WLH_WIRE_OK
    );
    assert(
        rpc.service_id == WLH_SERVICE_LINK &&
        rpc.method_id == WLH_LINK_METHOD_CREDIT_UPDATE
    );
    stream = pb_istream_from_buffer(rpc_payload, rpc_payload_size);
    assert(pb_decode(&stream, wlh_protocol_v1_CreditUpdate_fields, &update));
    assert(update.channel_id == WLH_CHANNEL_ETHERNET_AP);
    assert(update.units == 3u);

    assert(wlh_host_stop(&fixture.host) == WLH_HOST_OK);
}

/*
 * With ethernet_tx_aggregation_limit >= 2 the worker coalesces adjacent
 * same-channel Ethernet jobs into one wire frame, charging one credit unit
 * per raw record (never per frame). With the limit left at its default the
 * worker must keep emitting one record per wire frame.
 */
void test_ethernet_tx_aggregation(void) {
    fixture_t fixture;
    wlh_host_job_t job;
    wlh_host_data_job_t *first_job;
    wlh_host_data_job_t *second_job;
    wlh_raw_record_iterator_t iterator;
    wlh_raw_record_view_t record;
    wlh_frame_header_t header;
    const uint8_t *payload;
    size_t payload_size;
    unsigned tx_before;
    static const uint8_t first[] = {0xaa, 0xbb, 0xcc};
    static const uint8_t second[] = {0x11, 0x22};

    fixture_init(&fixture);
    establish_ready(&fixture);
    fixture.host.config.ethernet_tx_aggregation_limit = 3u;

    first_job = make_ethernet_job(
        &fixture.host, WLH_CHANNEL_ETHERNET_AP, first, sizeof(first)
    );
    second_job = make_ethernet_job(
        &fixture.host, WLH_CHANNEL_ETHERNET_AP, second, sizeof(second)
    );
    assert(first_job != NULL && second_job != NULL);
    if (first_job != NULL && second_job != NULL) {
        /* Hold the worker's mutex while placing two adjacent Ethernet jobs on
           its queue. This deterministically exercises the worker-side
           coalescer without depending on thread scheduling. */
        assert(
            fixture.host.config.osal.mutex_lock(
                fixture.host.config.osal.context,
                &fixture.host.state_mutex,
                WLH_OSAL_WAIT_FOREVER
            ) == 0
        );
        job.kind = WLH_HOST_JOB_ETHERNET_TX;
        job.payload = first_job;
        assert(
            fixture.host.config.osal.queue_send(
                fixture.host.config.osal.context,
                &fixture.host.core_queue,
                &job,
                WLH_OSAL_NO_WAIT
            ) == 0
        );
        job.payload = second_job;
        assert(
            fixture.host.config.osal.queue_send(
                fixture.host.config.osal.context,
                &fixture.host.core_queue,
                &job,
                WLH_OSAL_NO_WAIT
            ) == 0
        );
        /* The admission slots the merge path releases for the second job. */
        assert(
            fixture.host.config.osal.semaphore_take(
                fixture.host.config.osal.context,
                &fixture.host.ethernet_tx_slots,
                WLH_OSAL_NO_WAIT
            ) == 0
        );
        assert(
            fixture.host.config.osal.semaphore_take(
                fixture.host.config.osal.context,
                &fixture.host.ethernet_tx_slots,
                WLH_OSAL_NO_WAIT
            ) == 0
        );
        fixture.host.config.osal.mutex_unlock(
            fixture.host.config.osal.context, &fixture.host.state_mutex
        );

        tx_before = fixture.tx_count;
        wait_for_tx(&fixture, tx_before + 1u);
        /* Two records must leave as a single wire frame. */
        assert(fixture.tx_count == tx_before + 1u);
        assert(
            wlh_frame_decode(
                &header,
                &payload,
                &payload_size,
                fixture.tx,
                fixture.tx_size,
                sizeof(fixture.tx)
            ) == WLH_WIRE_OK
        );
        assert(header.channel == WLH_CHANNEL_ETHERNET_AP);
        assert(
            wlh_raw_record_iterator_init(&iterator, payload, payload_size) ==
            WLH_WIRE_OK
        );
        assert(wlh_raw_record_iterator_next(&iterator, &record) == WLH_WIRE_OK);
        assert(record.payload_size == sizeof(first));
        assert(memcmp(record.payload, first, sizeof(first)) == 0);
        assert(wlh_raw_record_iterator_next(&iterator, &record) == WLH_WIRE_OK);
        assert(record.payload_size == sizeof(second));
        assert(memcmp(record.payload, second, sizeof(second)) == 0);
        assert(
            wlh_raw_record_iterator_next(&iterator, &record) == WLH_WIRE_END
        );
        /* Credit is charged one unit per record, not per wire frame. */
        assert(fixture.host.tx_credit[WLH_CHANNEL_ETHERNET_AP] == 0u);
    }

    /* Default limit: aggregation disabled, one record per wire frame. */
    fixture.host.config.ethernet_tx_aggregation_limit = 1u;
    first_job = make_ethernet_job(
        &fixture.host, WLH_CHANNEL_ETHERNET_STA, first, sizeof(first)
    );
    second_job = make_ethernet_job(
        &fixture.host, WLH_CHANNEL_ETHERNET_STA, second, sizeof(second)
    );
    assert(first_job != NULL && second_job != NULL);
    if (first_job != NULL && second_job != NULL) {
        assert(
            fixture.host.config.osal.mutex_lock(
                fixture.host.config.osal.context,
                &fixture.host.state_mutex,
                WLH_OSAL_WAIT_FOREVER
            ) == 0
        );
        job.kind = WLH_HOST_JOB_ETHERNET_TX;
        job.payload = first_job;
        assert(
            fixture.host.config.osal.queue_send(
                fixture.host.config.osal.context,
                &fixture.host.core_queue,
                &job,
                WLH_OSAL_NO_WAIT
            ) == 0
        );
        job.payload = second_job;
        assert(
            fixture.host.config.osal.queue_send(
                fixture.host.config.osal.context,
                &fixture.host.core_queue,
                &job,
                WLH_OSAL_NO_WAIT
            ) == 0
        );
        fixture.host.config.osal.mutex_unlock(
            fixture.host.config.osal.context, &fixture.host.state_mutex
        );

        tx_before = fixture.tx_count;
        wait_for_tx(&fixture, tx_before + 2u);
        assert(fixture.tx_count == tx_before + 2u);
        assert(fixture.host.tx_credit[WLH_CHANNEL_ETHERNET_STA] == 0u);
    }

    assert(wlh_host_stop(&fixture.host) == WLH_HOST_OK);
}
