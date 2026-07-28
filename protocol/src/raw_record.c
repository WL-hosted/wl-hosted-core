#include "wlh/protocol/raw_record.h"

#include <string.h>

#include "wlh/protocol/endian.h"

wlh_wire_result_t wlh_raw_record_encode(
    uint8_t *output,
    size_t output_capacity,
    size_t *output_size,
    uint8_t record_type,
    uint8_t flags,
    const uint8_t *payload,
    size_t payload_size
) {
    size_t encoded_size;

    if (output == NULL || output_size == NULL ||
        (payload == NULL && payload_size != 0u)) {
        return WLH_WIRE_INVALID_ARGUMENT;
    }
    *output_size = 0;

    if (payload_size > UINT32_MAX ||
        payload_size > SIZE_MAX - WLH_RAW_RECORD_HEADER_SIZE) {
        return WLH_WIRE_INVALID_LENGTH;
    }
    encoded_size = WLH_RAW_RECORD_HEADER_SIZE + payload_size;
    if (output_capacity < encoded_size) {
        return WLH_WIRE_BUFFER_TOO_SMALL;
    }

    output[0] = record_type;
    output[1] = flags;
    (void)wlh_write_u16_le(
        output + 2, 2u, (uint16_t)WLH_RAW_RECORD_HEADER_SIZE
    );
    (void)wlh_write_u32_le(output + 4, 4u, (uint32_t)payload_size);
    if (payload_size != 0u) {
        memmove(output + WLH_RAW_RECORD_HEADER_SIZE, payload, payload_size);
    }
    *output_size = encoded_size;
    return WLH_WIRE_OK;
}

wlh_wire_result_t wlh_raw_record_iterator_init(
    wlh_raw_record_iterator_t *iterator, const uint8_t *data, size_t size
) {
    if (iterator == NULL || (data == NULL && size != 0u)) {
        return WLH_WIRE_INVALID_ARGUMENT;
    }
    iterator->data = data;
    iterator->size = size;
    iterator->offset = 0;
    return WLH_WIRE_OK;
}

wlh_wire_result_t wlh_raw_record_iterator_next(
    wlh_raw_record_iterator_t *iterator, wlh_raw_record_view_t *view
) {
    const uint8_t *record;
    size_t remaining;
    uint16_t header_size;
    uint32_t payload_size;

    if (iterator == NULL || view == NULL) {
        return WLH_WIRE_INVALID_ARGUMENT;
    }
    view->record_type = 0;
    view->flags = 0;
    view->payload = NULL;
    view->payload_size = 0;

    if (iterator->offset > iterator->size) {
        return WLH_WIRE_INVALID_ARGUMENT;
    }
    remaining = iterator->size - iterator->offset;
    if (remaining == 0u) {
        return WLH_WIRE_END;
    }
    if (remaining < WLH_RAW_RECORD_HEADER_SIZE) {
        return WLH_WIRE_TRUNCATED;
    }

    record = iterator->data + iterator->offset;
    (void)wlh_read_u16_le(record + 2, 2u, &header_size);
    (void)wlh_read_u32_le(record + 4, 4u, &payload_size);

    if ((size_t)header_size < WLH_RAW_RECORD_HEADER_SIZE) {
        return WLH_WIRE_INVALID_HEADER_SIZE;
    }
    if ((size_t)header_size > remaining) {
        return WLH_WIRE_TRUNCATED;
    }
    /* remaining - header_size cannot underflow after the check above, and
       comparing against it avoids any header_size + payload_size overflow. */
    if ((size_t)payload_size > remaining - (size_t)header_size) {
        return WLH_WIRE_TRUNCATED;
    }

    view->record_type = record[0];
    view->flags = record[1];
    view->payload = record + header_size;
    view->payload_size = payload_size;
    iterator->offset += (size_t)header_size + (size_t)payload_size;
    return WLH_WIRE_OK;
}
