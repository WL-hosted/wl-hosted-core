#include "wlh/protocol/wire.h"

#include <string.h>

static uint16_t read_u16_le_unchecked(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static int16_t read_i16_le_unchecked(const uint8_t *p) {
    return (int16_t)read_u16_le_unchecked(p);
}

static uint32_t read_u32_le_unchecked(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static void write_u16_le_unchecked(uint8_t *p, uint16_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
}

static void write_i16_le_unchecked(uint8_t *p, int16_t value) {
    write_u16_le_unchecked(p, (uint16_t)value);
}

static void write_u32_le_unchecked(uint8_t *p, uint32_t value) {
    p[0] = (uint8_t)value;
    p[1] = (uint8_t)(value >> 8);
    p[2] = (uint8_t)(value >> 16);
    p[3] = (uint8_t)(value >> 24);
}

static uint32_t frame_checksum(
    const uint8_t *data, size_t size, uint8_t flags
) {
    if ((flags & WLH_FRAME_FLAG_CRC32C_PRESENT) != 0u) {
        return wlh_crc32c(data, size);
    }
    return wlh_sum32(data, size);
}

wlh_wire_result_t wlh_frame_header_validate(const wlh_frame_header_t *header) {
    if (header == NULL) {
        return WLH_WIRE_INVALID_ARGUMENT;
    }

    if (header->magic != WLH_FRAME_MAGIC) {
        return WLH_WIRE_INVALID_MAGIC;
    }
    if (header->protocol_major != WLH_PROTOCOL_MAJOR) {
        return WLH_WIRE_UNSUPPORTED_VERSION;
    }
    if (header->header_size != WLH_FRAME_HEADER_SIZE) {
        return WLH_WIRE_INVALID_HEADER_SIZE;
    }

    if ((header->flags & (uint8_t)~WLH_FRAME_FLAGS_MASK) != 0u) {
        return WLH_WIRE_INVALID_FLAGS;
    }
    return WLH_WIRE_OK;
}

wlh_wire_result_t wlh_rpc_envelope_validate(
    const wlh_rpc_envelope_t *envelope
) {
    if (envelope == NULL) {
        return WLH_WIRE_INVALID_ARGUMENT;
    }

    if (envelope->service_id == WLH_SERVICE_INVALID ||
        envelope->method_id == 0u) {
        return WLH_WIRE_INVALID_RPC;
    }

    if (envelope->kind < WLH_RPC_KIND_REQUEST ||
        envelope->kind > WLH_RPC_KIND_EVENT) {
        return WLH_WIRE_INVALID_RPC;
    }

    if ((envelope->flags & (uint8_t)~WLH_RPC_FLAGS_MASK) != 0u) {
        return WLH_WIRE_INVALID_FLAGS;
    }

    if (envelope->kind != WLH_RPC_KIND_EVENT && envelope->request_id == 0u) {
        return WLH_WIRE_INVALID_RPC;
    }

    if (envelope->kind == WLH_RPC_KIND_REQUEST) {
        if (envelope->status_domain != WLH_STATUS_DOMAIN_NONE ||
            envelope->status_code != WLH_STATUS_OK) {
            return WLH_WIRE_INVALID_RPC;
        }
    } else if ((envelope->status_code == WLH_STATUS_OK) !=
               (envelope->status_domain == WLH_STATUS_DOMAIN_NONE)) {
        return WLH_WIRE_INVALID_RPC;
    }
    return WLH_WIRE_OK;
}

int wlh_read_u16_le(const uint8_t *input, size_t input_size, uint16_t *value) {
    if (input == NULL || value == NULL || input_size < sizeof(uint16_t)) {
        return -1;
    }
    *value = read_u16_le_unchecked(input);
    return 0;
}

int wlh_read_i16_le(const uint8_t *input, size_t input_size, int16_t *value) {
    if (input == NULL || value == NULL || input_size < sizeof(int16_t)) {
        return -1;
    }
    *value = read_i16_le_unchecked(input);
    return 0;
}

int wlh_read_u32_le(const uint8_t *input, size_t input_size, uint32_t *value) {
    if (input == NULL || value == NULL || input_size < sizeof(uint32_t)) {
        return -1;
    }
    *value = read_u32_le_unchecked(input);
    return 0;
}

int wlh_write_u16_le(uint8_t *output, size_t output_size, uint16_t value) {
    if (output == NULL || output_size < sizeof(uint16_t)) {
        return -1;
    }
    write_u16_le_unchecked(output, value);
    return 0;
}

int wlh_write_i16_le(uint8_t *output, size_t output_size, int16_t value) {
    if (output == NULL || output_size < sizeof(int16_t)) {
        return -1;
    }
    write_i16_le_unchecked(output, value);
    return 0;
}

int wlh_write_u32_le(uint8_t *output, size_t output_size, uint32_t value) {
    if (output == NULL || output_size < sizeof(uint32_t)) {
        return -1;
    }
    write_u32_le_unchecked(output, value);
    return 0;
}

void wlh_frame_header_init(wlh_frame_header_t *header, uint8_t channel) {
    if (header == NULL) {
        return;
    }
    memset(header, 0, sizeof(*header));
    header->magic = WLH_FRAME_MAGIC;
    header->protocol_major = WLH_PROTOCOL_MAJOR;
    header->header_size = (uint8_t)WLH_FRAME_HEADER_SIZE;
    header->channel = channel;
}

uint32_t wlh_sum32(const uint8_t *data, size_t size) {
    uint32_t sum = 0;
    size_t i;

    if (data == NULL && size != 0u) {
        return 0;
    }
    for (i = 0; i < size; ++i) {
        sum += data[i];
    }
    return sum;
}

uint32_t wlh_crc32c(const uint8_t *data, size_t size) {
    uint32_t crc = UINT32_C(0xffffffff);
    size_t i;
    unsigned bit;

    if (data == NULL && size != 0u) {
        return 0;
    }
    for (i = 0; i < size; ++i) {
        crc ^= data[i];
        for (bit = 0; bit < 8u; ++bit) {
            const uint32_t mask = (uint32_t)-(int32_t)(crc & 1u);
            crc = (crc >> 1) ^ (UINT32_C(0x82f63b78) & mask);
        }
    }
    return ~crc;
}

wlh_wire_result_t wlh_frame_encode(
    uint8_t *output,
    size_t output_capacity,
    size_t *output_size,
    const wlh_frame_header_t *header,
    const uint8_t *payload,
    size_t payload_size
) {
    wlh_wire_result_t result;
    uint32_t payload_checksum;
    uint32_t header_checksum;
    size_t encoded_size;

    if (output == NULL || output_size == NULL || header == NULL ||
        (payload == NULL && payload_size != 0u)) {
        return WLH_WIRE_INVALID_ARGUMENT;
    }
    *output_size = 0;

    result = wlh_frame_header_validate(header);
    if (result != WLH_WIRE_OK) {
        return result;
    }
    if (payload_size > WLH_FRAME_MAX_PAYLOAD) {
        return WLH_WIRE_INVALID_LENGTH;
    }
    encoded_size = WLH_FRAME_HEADER_SIZE + payload_size;
    if (output_capacity < encoded_size) {
        return WLH_WIRE_BUFFER_TOO_SMALL;
    }

    payload_checksum = frame_checksum(payload, payload_size, header->flags);
    /* Move an aliased payload before writing the header over its source. */
    if (payload_size != 0u) {
        memmove(output + WLH_FRAME_HEADER_SIZE, payload, payload_size);
    }

    write_u16_le_unchecked(output + 0, header->magic);
    output[2] = header->protocol_major;
    output[3] = header->header_size;
    output[4] = header->channel;
    output[5] = header->flags;
    write_u16_le_unchecked(output + 6, (uint16_t)payload_size);
    write_u32_le_unchecked(output + 8, header->session_id);
    write_u32_le_unchecked(output + 12, header->sequence);
    write_u32_le_unchecked(output + 16, 0);
    write_u32_le_unchecked(output + 20, payload_checksum);

    header_checksum =
        frame_checksum(output, WLH_FRAME_HEADER_SIZE, header->flags);
    write_u32_le_unchecked(output + 16, header_checksum);
    *output_size = encoded_size;
    return WLH_WIRE_OK;
}

wlh_wire_result_t wlh_frame_decode(
    wlh_frame_header_t *header,
    const uint8_t **payload,
    size_t *payload_size,
    const uint8_t *frame,
    size_t frame_size,
    size_t negotiated_max_frame_size
) {
    uint8_t checksum_header[WLH_FRAME_HEADER_SIZE];
    wlh_wire_result_t result;
    size_t expected_size;
    uint32_t expected_checksum;

    if (header == NULL || payload == NULL || payload_size == NULL ||
        frame == NULL) {
        return WLH_WIRE_INVALID_ARGUMENT;
    }
    *payload = NULL;
    *payload_size = 0;

    if (negotiated_max_frame_size < WLH_FRAME_HEADER_SIZE ||
        negotiated_max_frame_size > WLH_FRAME_MAX_SIZE) {
        return WLH_WIRE_INVALID_ARGUMENT;
    }
    if (frame_size < WLH_FRAME_HEADER_SIZE) {
        return WLH_WIRE_TRUNCATED;
    }
    if (frame_size > negotiated_max_frame_size) {
        return WLH_WIRE_INVALID_LENGTH;
    }

    header->magic = read_u16_le_unchecked(frame + 0);
    header->protocol_major = frame[2];
    header->header_size = frame[3];
    header->channel = frame[4];
    header->flags = frame[5];
    header->payload_size = read_u16_le_unchecked(frame + 6);
    header->session_id = read_u32_le_unchecked(frame + 8);
    header->sequence = read_u32_le_unchecked(frame + 12);
    header->header_checksum = read_u32_le_unchecked(frame + 16);
    header->payload_checksum = read_u32_le_unchecked(frame + 20);

    result = wlh_frame_header_validate(header);
    if (result != WLH_WIRE_OK) {
        return result;
    }
    expected_size = WLH_FRAME_HEADER_SIZE + (size_t)header->payload_size;
    if (expected_size > negotiated_max_frame_size) {
        return WLH_WIRE_INVALID_LENGTH;
    }
    if (frame_size < expected_size) {
        return WLH_WIRE_TRUNCATED;
    }
    if (frame_size != expected_size) {
        return WLH_WIRE_INVALID_LENGTH;
    }

    memcpy(checksum_header, frame, WLH_FRAME_HEADER_SIZE);
    memset(checksum_header + 16, 0, sizeof(uint32_t));
    expected_checksum =
        frame_checksum(checksum_header, sizeof(checksum_header), header->flags);
    if (header->header_checksum != expected_checksum) {
        return WLH_WIRE_CHECKSUM_MISMATCH;
    }
    expected_checksum = frame_checksum(
        frame + WLH_FRAME_HEADER_SIZE, header->payload_size, header->flags
    );
    if (header->payload_checksum != expected_checksum) {
        return WLH_WIRE_CHECKSUM_MISMATCH;
    }

    *payload = frame + WLH_FRAME_HEADER_SIZE;
    *payload_size = header->payload_size;
    return WLH_WIRE_OK;
}

wlh_wire_result_t wlh_frame_validate(
    const uint8_t *frame, size_t frame_size, size_t negotiated_max_frame_size
) {
    wlh_frame_header_t header;
    const uint8_t *payload;
    size_t payload_size;

    return wlh_frame_decode(
        &header,
        &payload,
        &payload_size,
        frame,
        frame_size,
        negotiated_max_frame_size
    );
}

wlh_wire_result_t wlh_rpc_encode(
    uint8_t *output,
    size_t output_capacity,
    size_t *output_size,
    const wlh_rpc_envelope_t *envelope,
    const uint8_t *payload,
    size_t payload_size
) {
    wlh_wire_result_t result;
    size_t encoded_size;

    if (output == NULL || output_size == NULL || envelope == NULL ||
        (payload == NULL && payload_size != 0u)) {
        return WLH_WIRE_INVALID_ARGUMENT;
    }
    *output_size = 0;

    result = wlh_rpc_envelope_validate(envelope);
    if (result != WLH_WIRE_OK) {
        return result;
    }
    if (payload_size > UINT16_MAX) {
        return WLH_WIRE_INVALID_LENGTH;
    }
    encoded_size = WLH_RPC_ENVELOPE_SIZE + payload_size;
    if (output_capacity < encoded_size) {
        return WLH_WIRE_BUFFER_TOO_SMALL;
    }

    /* Move an aliased payload before writing the envelope over its source. */
    if (payload_size != 0u) {
        memmove(output + WLH_RPC_ENVELOPE_SIZE, payload, payload_size);
    }

    write_u16_le_unchecked(output + 0, envelope->service_id);
    write_u16_le_unchecked(output + 2, envelope->method_id);
    write_u32_le_unchecked(output + 4, envelope->request_id);
    output[8] = envelope->kind;
    output[9] = envelope->flags;
    write_u16_le_unchecked(output + 10, (uint16_t)payload_size);
    write_u16_le_unchecked(output + 12, envelope->status_domain);
    write_i16_le_unchecked(output + 14, envelope->status_code);

    *output_size = encoded_size;
    return WLH_WIRE_OK;
}

wlh_wire_result_t wlh_rpc_decode(
    wlh_rpc_envelope_t *envelope,
    const uint8_t **payload,
    size_t *payload_size,
    const uint8_t *rpc,
    size_t rpc_size,
    size_t negotiated_max_rpc_payload
) {
    wlh_wire_result_t result;
    size_t expected_size;

    if (envelope == NULL || payload == NULL || payload_size == NULL ||
        rpc == NULL) {
        return WLH_WIRE_INVALID_ARGUMENT;
    }
    *payload = NULL;
    *payload_size = 0;

    if (negotiated_max_rpc_payload > UINT16_MAX) {
        return WLH_WIRE_INVALID_ARGUMENT;
    }
    if (rpc_size < WLH_RPC_ENVELOPE_SIZE) {
        return WLH_WIRE_TRUNCATED;
    }

    envelope->service_id = read_u16_le_unchecked(rpc + 0);
    envelope->method_id = read_u16_le_unchecked(rpc + 2);
    envelope->request_id = read_u32_le_unchecked(rpc + 4);
    envelope->kind = rpc[8];
    envelope->flags = rpc[9];
    envelope->payload_size = read_u16_le_unchecked(rpc + 10);
    envelope->status_domain = read_u16_le_unchecked(rpc + 12);
    envelope->status_code = read_i16_le_unchecked(rpc + 14);

    result = wlh_rpc_envelope_validate(envelope);
    if (result != WLH_WIRE_OK) {
        return result;
    }
    if ((size_t)envelope->payload_size > negotiated_max_rpc_payload) {
        return WLH_WIRE_INVALID_LENGTH;
    }
    expected_size = WLH_RPC_ENVELOPE_SIZE + (size_t)envelope->payload_size;
    if (rpc_size < expected_size) {
        return WLH_WIRE_TRUNCATED;
    }
    if (rpc_size != expected_size) {
        return WLH_WIRE_INVALID_LENGTH;
    }

    *payload = rpc + WLH_RPC_ENVELOPE_SIZE;
    *payload_size = envelope->payload_size;
    return WLH_WIRE_OK;
}

wlh_wire_result_t wlh_rpc_validate(
    const uint8_t *rpc, size_t rpc_size, size_t negotiated_max_rpc_payload
) {
    wlh_rpc_envelope_t envelope;
    const uint8_t *payload;
    size_t payload_size;

    return wlh_rpc_decode(
        &envelope,
        &payload,
        &payload_size,
        rpc,
        rpc_size,
        negotiated_max_rpc_payload
    );
}
