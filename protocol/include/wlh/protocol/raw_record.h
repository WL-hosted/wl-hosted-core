#ifndef WLH_PROTOCOL_RAW_RECORD_H
#define WLH_PROTOCOL_RAW_RECORD_H

#include <stddef.h>
#include <stdint.h>

#include "wlh/protocol/wire.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WLH_RAW_RECORD_HEADER_SIZE ((size_t)8)

typedef struct wlh_raw_record_view {
    uint8_t record_type;
    uint8_t flags;
    const uint8_t *payload;
    size_t payload_size;
} wlh_raw_record_view_t;

typedef struct wlh_raw_record_iterator {
    const uint8_t *data;
    size_t size;
    size_t offset;
} wlh_raw_record_iterator_t;

/* Encodes one raw record (8-byte header plus payload) into output. The
   output capacity is validated before any byte is written. Aggregate
   multiple records by advancing output by *output_size between calls. */
wlh_wire_result_t wlh_raw_record_encode(
    uint8_t *output,
    size_t output_capacity,
    size_t *output_size,
    uint8_t record_type,
    uint8_t flags,
    const uint8_t *payload,
    size_t payload_size
);

wlh_wire_result_t wlh_raw_record_iterator_init(
    wlh_raw_record_iterator_t *iterator, const uint8_t *data, size_t size
);

/* Returns WLH_WIRE_OK and fills *view for each record, WLH_WIRE_END after
   the final record, and a negative result when the input is malformed.
   Records with a header_size larger than 8 are accepted and their extra
   header bytes skipped, per the v1 forward-compatibility rule. */
wlh_wire_result_t wlh_raw_record_iterator_next(
    wlh_raw_record_iterator_t *iterator, wlh_raw_record_view_t *view
);

#ifdef __cplusplus
}
#endif

#endif
