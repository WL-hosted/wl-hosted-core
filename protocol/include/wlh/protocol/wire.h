#ifndef WLH_PROTOCOL_WIRE_H
#define WLH_PROTOCOL_WIRE_H

#include <stddef.h>
#include <stdint.h>

#include "wlh/protocol/endian.h"
#include "wlh/protocol/ids.h"

#ifdef __cplusplus
extern "C" {
#endif

#define WLH_FRAME_MAGIC UINT16_C(0x4c57)
#define WLH_PROTOCOL_MAJOR UINT8_C(1)
#define WLH_FRAME_HEADER_SIZE ((size_t)24)
#define WLH_RPC_ENVELOPE_SIZE ((size_t)16)
#define WLH_FRAME_MAX_PAYLOAD UINT16_MAX
#define WLH_FRAME_MAX_SIZE                                                     \
    (WLH_FRAME_HEADER_SIZE + (size_t)WLH_FRAME_MAX_PAYLOAD)

#define WLH_FRAME_FLAG_CRC32C_PRESENT UINT8_C(0x01)
#define WLH_FRAME_FLAG_AGGREGATED UINT8_C(0x02)
#define WLH_FRAME_FLAGS_MASK UINT8_C(0x03)

#define WLH_RPC_FLAG_MORE UINT8_C(0x01)
#define WLH_RPC_FLAG_SENSITIVE UINT8_C(0x02)
#define WLH_RPC_FLAGS_MASK UINT8_C(0x03)

typedef enum wlh_wire_result {
    WLH_WIRE_OK = 0,
    WLH_WIRE_INVALID_ARGUMENT = -1,
    WLH_WIRE_BUFFER_TOO_SMALL = -2,
    WLH_WIRE_TRUNCATED = -3,
    WLH_WIRE_INVALID_MAGIC = -4,
    WLH_WIRE_UNSUPPORTED_VERSION = -5,
    WLH_WIRE_INVALID_HEADER_SIZE = -6,
    WLH_WIRE_INVALID_FLAGS = -7,
    WLH_WIRE_INVALID_LENGTH = -8,
    WLH_WIRE_CHECKSUM_MISMATCH = -9,
    WLH_WIRE_INVALID_RPC = -10
} wlh_wire_result_t;

typedef enum wlh_rpc_kind {
    WLH_RPC_KIND_REQUEST = 1,
    WLH_RPC_KIND_RESPONSE = 2,
    WLH_RPC_KIND_EVENT = 3
} wlh_rpc_kind_t;

typedef struct wlh_frame_header {
    uint16_t magic;
    uint8_t protocol_major;
    uint8_t header_size;

    uint8_t channel;
    uint8_t flags;
    uint16_t payload_size;

    uint32_t session_id;
    uint32_t sequence;

    uint32_t header_checksum;
    uint32_t payload_checksum;
} wlh_frame_header_t;

typedef struct wlh_rpc_envelope {
    uint16_t service_id;
    uint16_t method_id;

    uint32_t request_id;

    uint8_t kind;
    uint8_t flags;
    uint16_t payload_size;

    uint16_t status_domain;
    int16_t status_code;
} wlh_rpc_envelope_t;

void wlh_frame_header_init(wlh_frame_header_t *header, uint8_t channel);

uint32_t wlh_sum32(const uint8_t *data, size_t size);
uint32_t wlh_crc32c(const uint8_t *data, size_t size);

wlh_wire_result_t wlh_frame_header_validate(const wlh_frame_header_t *header);

wlh_wire_result_t wlh_frame_validate(
    const uint8_t *frame, size_t frame_size, size_t negotiated_max_frame_size
);

wlh_wire_result_t wlh_frame_encode(
    uint8_t *output,
    size_t output_capacity,
    size_t *output_size,
    const wlh_frame_header_t *header,
    const uint8_t *payload,
    size_t payload_size
);

wlh_wire_result_t wlh_frame_decode(
    wlh_frame_header_t *header,
    const uint8_t **payload,
    size_t *payload_size,
    const uint8_t *frame,
    size_t frame_size,
    size_t negotiated_max_frame_size
);

wlh_wire_result_t wlh_rpc_encode(
    uint8_t *output,
    size_t output_capacity,
    size_t *output_size,
    const wlh_rpc_envelope_t *envelope,
    const uint8_t *payload,
    size_t payload_size
);

wlh_wire_result_t wlh_rpc_envelope_validate(const wlh_rpc_envelope_t *envelope);

wlh_wire_result_t wlh_rpc_validate(
    const uint8_t *rpc, size_t rpc_size, size_t negotiated_max_rpc_payload
);

wlh_wire_result_t wlh_rpc_decode(
    wlh_rpc_envelope_t *envelope,
    const uint8_t **payload,
    size_t *payload_size,
    const uint8_t *rpc,
    size_t rpc_size,
    size_t negotiated_max_rpc_payload
);

#ifdef __cplusplus
}
#endif

#endif
