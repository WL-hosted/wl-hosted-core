#include "wlh/protocol.h"

#include <stdio.h>
#include <string.h>

static int failures;

#define CHECK(condition)                                                       \
    do {                                                                       \
        if (!(condition)) {                                                    \
            fprintf(                                                           \
                stderr,                                                        \
                "%s:%d: check failed: %s\n",                                   \
                __FILE__,                                                      \
                __LINE__,                                                      \
                #condition                                                     \
            );                                                                 \
            ++failures;                                                        \
        }                                                                      \
    } while (0)

#define CHECK_RESULT(expression, expected) CHECK((expression) == (expected))

// clang-format off
static const uint8_t sum_frame_golden[] = {
    0x57, 0x4c, 0x01, 0x18, 0x01, 0x00, 0x04, 0x00,
    0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55,
    0x02, 0xa5, 0x4c, 0x6e, 0xde, 0xad, 0xbe, 0xef,
    0xde, 0xad, 0xbe, 0xef
};

static const uint8_t crc_frame_golden[] = {
    0x57, 0x4c, 0x01, 0x18, 0x01, 0x01, 0x09, 0x00,
    0x44, 0x33, 0x22, 0x11, 0x88, 0x77, 0x66, 0x55,
    0x25, 0x04, 0x5c, 0xd2, 0x83, 0x92, 0x06, 0xe3,
    0x31, 0x32, 0x33, 0x34, 0x35, 0x36, 0x37, 0x38,
    0x39
};

static const uint8_t rpc_golden[] = {
    0x02, 0x00, 0x05, 0x00, 0x78, 0x56, 0x34, 0x12,
    0x01, 0x02, 0x02, 0x00, 0x00, 0x00, 0x00, 0x00,
    0xaa, 0xbb
};
// clang-format on

static void test_endian_and_checksums(void) {
    uint8_t bytes[4] = {0};
    uint16_t u16 = 0;
    int16_t i16 = 0;
    uint32_t u32 = 0;
    static const uint8_t sum_data[] = {0xff, 0x02};
    static const uint8_t tail_data[] = {0x01, 0x02, 0x03, 0x04, 0x05};
    static const uint8_t crc_data[] = "123456789";

    CHECK(wlh_write_u16_le(bytes, sizeof(bytes), UINT16_C(0xabcd)) == 0);
    CHECK(bytes[0] == 0xcd && bytes[1] == 0xab);
    CHECK(wlh_read_u16_le(bytes, 2, &u16) == 0 && u16 == UINT16_C(0xabcd));
    CHECK(wlh_write_i16_le(bytes, sizeof(bytes), INT16_C(-1234)) == 0);
    CHECK(wlh_read_i16_le(bytes, 2, &i16) == 0 && i16 == INT16_C(-1234));
    CHECK(wlh_write_u32_le(bytes, sizeof(bytes), UINT32_C(0x89abcdef)) == 0);
    CHECK(
        wlh_read_u32_le(bytes, sizeof(bytes), &u32) == 0 &&
        u32 == UINT32_C(0x89abcdef)
    );
    CHECK(wlh_read_u32_le(bytes, 3, &u32) != 0);
    CHECK(wlh_write_u16_le(bytes, 1, 0) != 0);
    CHECK(wlh_read_u16_le(NULL, 2, &u16) != 0);
    CHECK(wlh_read_u16_le(bytes, 2, NULL) != 0);
    CHECK(wlh_write_u32_le(NULL, 4, 0) != 0);

    CHECK(wlh_sum32(sum_data, sizeof(sum_data)) == UINT32_C(0x2ff));
    CHECK(wlh_sum32(NULL, 0) == 0);
    CHECK(wlh_sum32(tail_data, 1) == UINT32_C(0x01));
    CHECK(wlh_sum32(tail_data, 2) == UINT32_C(0x0201));
    CHECK(wlh_sum32(tail_data, 3) == UINT32_C(0x030201));
    CHECK(wlh_sum32(tail_data, 4) == UINT32_C(0x04030201));
    CHECK(wlh_sum32(tail_data, 5) == UINT32_C(0x04030206));
    CHECK(wlh_crc32c(crc_data, sizeof(crc_data) - 1) == UINT32_C(0xe3069283));
    CHECK(wlh_crc32c(NULL, 0) == 0);
}

static wlh_frame_header_t make_header(uint8_t flags) {
    wlh_frame_header_t header;

    wlh_frame_header_init(&header, WLH_CHANNEL_CONTROL_RPC);
    header.flags = flags;
    header.session_id = UINT32_C(0x11223344);
    header.sequence = UINT32_C(0x55667788);
    return header;
}

static void test_frame_golden(void) {
    static const uint8_t sum_payload[] = {0xde, 0xad, 0xbe, 0xef};
    static const uint8_t crc_payload[] = "123456789";
    uint8_t encoded[64];
    wlh_frame_header_t header = make_header(0);
    wlh_frame_header_t decoded;
    const uint8_t *payload = NULL;
    size_t payload_size = 0;
    size_t encoded_size = 99;

    CHECK_RESULT(wlh_frame_header_validate(&header), WLH_WIRE_OK);
    CHECK_RESULT(
        wlh_frame_encode(
            encoded,
            sizeof(encoded),
            &encoded_size,
            &header,
            sum_payload,
            sizeof(sum_payload)
        ),
        WLH_WIRE_OK
    );
    CHECK(encoded_size == sizeof(sum_frame_golden));
    CHECK(memcmp(encoded, sum_frame_golden, sizeof(sum_frame_golden)) == 0);
    CHECK_RESULT(
        wlh_frame_validate(encoded, encoded_size, sizeof(encoded)), WLH_WIRE_OK
    );
    CHECK_RESULT(
        wlh_frame_decode(
            &decoded,
            &payload,
            &payload_size,
            encoded,
            encoded_size,
            sizeof(encoded)
        ),
        WLH_WIRE_OK
    );
    CHECK(decoded.payload_size == sizeof(sum_payload));
    CHECK(decoded.header_checksum == UINT32_C(0x6e4ca502));
    CHECK(decoded.payload_checksum == UINT32_C(0xefbeadde));
    CHECK(payload_size == sizeof(sum_payload));
    CHECK(memcmp(payload, sum_payload, payload_size) == 0);

    header = make_header(WLH_FRAME_FLAG_CRC32C_PRESENT);
    CHECK_RESULT(
        wlh_frame_encode(
            encoded,
            sizeof(encoded),
            &encoded_size,
            &header,
            crc_payload,
            sizeof(crc_payload) - 1
        ),
        WLH_WIRE_OK
    );
    CHECK(encoded_size == sizeof(crc_frame_golden));
    CHECK(memcmp(encoded, crc_frame_golden, sizeof(crc_frame_golden)) == 0);
    CHECK_RESULT(
        wlh_frame_validate(encoded, encoded_size, sizeof(encoded)), WLH_WIRE_OK
    );

    /* Encoding supports a payload that begins at the output buffer. */
    memcpy(encoded, sum_payload, sizeof(sum_payload));
    header = make_header(0);
    CHECK_RESULT(
        wlh_frame_encode(
            encoded,
            sizeof(encoded),
            &encoded_size,
            &header,
            encoded,
            sizeof(sum_payload)
        ),
        WLH_WIRE_OK
    );
    CHECK(memcmp(encoded, sum_frame_golden, sizeof(sum_frame_golden)) == 0);
}

static void test_frame_malformed(void) {
    uint8_t frame[sizeof(crc_frame_golden) + 1];
    wlh_frame_header_t header = make_header(0);
    size_t output_size = 77;

    CHECK_RESULT(wlh_frame_header_validate(NULL), WLH_WIRE_INVALID_ARGUMENT);
    header.magic = 0;
    CHECK_RESULT(wlh_frame_header_validate(&header), WLH_WIRE_INVALID_MAGIC);
    header = make_header(0x80);
    CHECK_RESULT(wlh_frame_header_validate(&header), WLH_WIRE_INVALID_FLAGS);

    header = make_header(0);
    CHECK_RESULT(
        wlh_frame_encode(frame, 2, &output_size, &header, NULL, 0),
        WLH_WIRE_BUFFER_TOO_SMALL
    );
    CHECK(output_size == 0);
    CHECK_RESULT(
        wlh_frame_encode(frame, sizeof(frame), &output_size, &header, NULL, 1),
        WLH_WIRE_INVALID_ARGUMENT
    );

    CHECK_RESULT(
        wlh_frame_validate(sum_frame_golden, 23, WLH_FRAME_MAX_SIZE),
        WLH_WIRE_TRUNCATED
    );
    CHECK_RESULT(
        wlh_frame_validate(sum_frame_golden, sizeof(sum_frame_golden), 23),
        WLH_WIRE_INVALID_ARGUMENT
    );
    CHECK_RESULT(
        wlh_frame_validate(sum_frame_golden, sizeof(sum_frame_golden), 27),
        WLH_WIRE_INVALID_LENGTH
    );

    /* Mutate individual header/payload fields and expect checksum mismatches.
     */
    memcpy(frame, sum_frame_golden, sizeof(sum_frame_golden));
    frame[0] = 0;
    CHECK_RESULT(
        wlh_frame_validate(frame, sizeof(sum_frame_golden), sizeof(frame)),
        WLH_WIRE_INVALID_MAGIC
    );
    memcpy(frame, sum_frame_golden, sizeof(sum_frame_golden));
    frame[2] = 2;
    CHECK_RESULT(
        wlh_frame_validate(frame, sizeof(sum_frame_golden), sizeof(frame)),
        WLH_WIRE_UNSUPPORTED_VERSION
    );
    memcpy(frame, sum_frame_golden, sizeof(sum_frame_golden));
    frame[3] = 23;
    CHECK_RESULT(
        wlh_frame_validate(frame, sizeof(sum_frame_golden), sizeof(frame)),
        WLH_WIRE_INVALID_HEADER_SIZE
    );
    memcpy(frame, sum_frame_golden, sizeof(sum_frame_golden));
    frame[5] = 0x80;
    CHECK_RESULT(
        wlh_frame_validate(frame, sizeof(sum_frame_golden), sizeof(frame)),
        WLH_WIRE_INVALID_FLAGS
    );
    memcpy(frame, sum_frame_golden, sizeof(sum_frame_golden));
    frame[6] = 5;
    CHECK_RESULT(
        wlh_frame_validate(frame, sizeof(sum_frame_golden), sizeof(frame)),
        WLH_WIRE_TRUNCATED
    );
    memcpy(frame, sum_frame_golden, sizeof(sum_frame_golden));
    frame[sizeof(sum_frame_golden)] = 0;
    CHECK_RESULT(
        wlh_frame_validate(frame, sizeof(sum_frame_golden) + 1, sizeof(frame)),
        WLH_WIRE_INVALID_LENGTH
    );
    memcpy(frame, sum_frame_golden, sizeof(sum_frame_golden));
    frame[4] ^= 1;
    CHECK_RESULT(
        wlh_frame_validate(frame, sizeof(sum_frame_golden), sizeof(frame)),
        WLH_WIRE_CHECKSUM_MISMATCH
    );
    memcpy(frame, sum_frame_golden, sizeof(sum_frame_golden));
    frame[WLH_FRAME_HEADER_SIZE] ^= 1;
    CHECK_RESULT(
        wlh_frame_validate(frame, sizeof(sum_frame_golden), sizeof(frame)),
        WLH_WIRE_CHECKSUM_MISMATCH
    );
}

static wlh_rpc_envelope_t make_request(void) {
    wlh_rpc_envelope_t envelope;

    memset(&envelope, 0, sizeof(envelope));
    envelope.service_id = WLH_SERVICE_WIFI;
    envelope.method_id = WLH_WIFI_METHOD_CONNECT;
    envelope.request_id = UINT32_C(0x12345678);
    envelope.kind = WLH_RPC_KIND_REQUEST;
    envelope.flags = WLH_RPC_FLAG_SENSITIVE;
    return envelope;
}

static void test_rpc_golden(void) {
    static const uint8_t rpc_payload[] = {0xaa, 0xbb};
    uint8_t encoded[32];
    wlh_rpc_envelope_t envelope = make_request();
    wlh_rpc_envelope_t decoded;
    const uint8_t *payload = NULL;
    size_t payload_size = 0;
    size_t encoded_size = 0;

    CHECK_RESULT(wlh_rpc_envelope_validate(&envelope), WLH_WIRE_OK);
    CHECK_RESULT(
        wlh_rpc_encode(
            encoded,
            sizeof(encoded),
            &encoded_size,
            &envelope,
            rpc_payload,
            sizeof(rpc_payload)
        ),
        WLH_WIRE_OK
    );
    CHECK(encoded_size == sizeof(rpc_golden));
    CHECK(memcmp(encoded, rpc_golden, sizeof(rpc_golden)) == 0);
    CHECK_RESULT(
        wlh_rpc_validate(encoded, encoded_size, sizeof(rpc_payload)),
        WLH_WIRE_OK
    );
    CHECK_RESULT(
        wlh_rpc_decode(
            &decoded,
            &payload,
            &payload_size,
            encoded,
            encoded_size,
            sizeof(rpc_payload)
        ),
        WLH_WIRE_OK
    );
    CHECK(decoded.request_id == envelope.request_id);
    CHECK(decoded.payload_size == sizeof(rpc_payload));
    CHECK(payload_size == sizeof(rpc_payload));
    CHECK(memcmp(payload, rpc_payload, payload_size) == 0);

    memcpy(encoded, rpc_payload, sizeof(rpc_payload));
    CHECK_RESULT(
        wlh_rpc_encode(
            encoded,
            sizeof(encoded),
            &encoded_size,
            &envelope,
            encoded,
            sizeof(rpc_payload)
        ),
        WLH_WIRE_OK
    );
    CHECK(memcmp(encoded, rpc_golden, sizeof(rpc_golden)) == 0);
}

static void test_rpc_malformed(void) {
    uint8_t rpc[sizeof(rpc_golden) + 1];
    wlh_rpc_envelope_t envelope = make_request();
    size_t output_size = 55;

    CHECK_RESULT(wlh_rpc_envelope_validate(NULL), WLH_WIRE_INVALID_ARGUMENT);
    envelope.request_id = 0;
    CHECK_RESULT(wlh_rpc_envelope_validate(&envelope), WLH_WIRE_INVALID_RPC);
    envelope = make_request();
    envelope.service_id = WLH_SERVICE_INVALID;
    CHECK_RESULT(wlh_rpc_envelope_validate(&envelope), WLH_WIRE_INVALID_RPC);
    envelope = make_request();
    envelope.method_id = 0;
    CHECK_RESULT(wlh_rpc_envelope_validate(&envelope), WLH_WIRE_INVALID_RPC);
    envelope = make_request();
    envelope.kind = 4;
    CHECK_RESULT(wlh_rpc_envelope_validate(&envelope), WLH_WIRE_INVALID_RPC);
    envelope = make_request();
    envelope.flags = 0x80;
    CHECK_RESULT(wlh_rpc_envelope_validate(&envelope), WLH_WIRE_INVALID_FLAGS);
    envelope = make_request();
    envelope.status_domain = WLH_STATUS_DOMAIN_PROTOCOL;
    CHECK_RESULT(wlh_rpc_envelope_validate(&envelope), WLH_WIRE_INVALID_RPC);
    envelope.kind = WLH_RPC_KIND_RESPONSE;
    envelope.status_code = WLH_STATUS_INTERNAL;
    CHECK_RESULT(wlh_rpc_envelope_validate(&envelope), WLH_WIRE_OK);
    envelope.status_code = WLH_STATUS_OK;
    CHECK_RESULT(wlh_rpc_envelope_validate(&envelope), WLH_WIRE_INVALID_RPC);

    envelope = make_request();
    CHECK_RESULT(
        wlh_rpc_encode(rpc, 1, &output_size, &envelope, NULL, 0),
        WLH_WIRE_BUFFER_TOO_SMALL
    );
    CHECK(output_size == 0);
    CHECK_RESULT(
        wlh_rpc_encode(rpc, sizeof(rpc), &output_size, &envelope, NULL, 1),
        WLH_WIRE_INVALID_ARGUMENT
    );
    CHECK_RESULT(wlh_rpc_validate(rpc_golden, 15, 2), WLH_WIRE_TRUNCATED);
    CHECK_RESULT(
        wlh_rpc_validate(rpc_golden, sizeof(rpc_golden), 1),
        WLH_WIRE_INVALID_LENGTH
    );
    CHECK_RESULT(
        wlh_rpc_validate(
            rpc_golden, sizeof(rpc_golden), (size_t)UINT16_MAX + 1u
        ),
        WLH_WIRE_INVALID_ARGUMENT
    );

    memcpy(rpc, rpc_golden, sizeof(rpc_golden));
    rpc[10] = 3;
    CHECK_RESULT(
        wlh_rpc_validate(rpc, sizeof(rpc_golden), 3), WLH_WIRE_TRUNCATED
    );
    memcpy(rpc, rpc_golden, sizeof(rpc_golden));
    rpc[sizeof(rpc_golden)] = 0;
    CHECK_RESULT(
        wlh_rpc_validate(rpc, sizeof(rpc_golden) + 1, 3),
        WLH_WIRE_INVALID_LENGTH
    );
    memcpy(rpc, rpc_golden, sizeof(rpc_golden));
    rpc[4] = rpc[5] = rpc[6] = rpc[7] = 0;
    CHECK_RESULT(
        wlh_rpc_validate(rpc, sizeof(rpc_golden), 2), WLH_WIRE_INVALID_RPC
    );
}

int main(void) {
    test_endian_and_checksums();
    test_frame_golden();
    test_frame_malformed();
    test_rpc_golden();
    test_rpc_malformed();

    if (failures != 0) {
        fprintf(stderr, "%d test checks failed\n", failures);
        return 1;
    }
    puts("all wire protocol checks passed");
    return 0;
}
