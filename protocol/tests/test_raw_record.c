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

static void test_encode_single(void) {
    uint8_t output[64];
    size_t output_size = 0;
    static const uint8_t payload[] = {0xde, 0xad, 0xbe, 0xef};

    CHECK_RESULT(
        wlh_raw_record_encode(
            output,
            sizeof(output),
            &output_size,
            1u,
            0u,
            payload,
            sizeof(payload)
        ),
        WLH_WIRE_OK
    );
    CHECK(output_size == WLH_RAW_RECORD_HEADER_SIZE + sizeof(payload));
    CHECK(output[0] == 1u);
    CHECK(output[1] == 0u);
    CHECK(output[2] == 8u && output[3] == 0u);
    CHECK(
        output[4] == 4u && output[5] == 0u && output[6] == 0u && output[7] == 0u
    );
    CHECK(memcmp(output + 8, payload, sizeof(payload)) == 0);
}

static void test_encode_empty_payload(void) {
    uint8_t output[WLH_RAW_RECORD_HEADER_SIZE];
    size_t output_size = 0;

    CHECK_RESULT(
        wlh_raw_record_encode(
            output, sizeof(output), &output_size, 4u, 0u, NULL, 0u
        ),
        WLH_WIRE_OK
    );
    CHECK(output_size == WLH_RAW_RECORD_HEADER_SIZE);
    CHECK(output[0] == 4u);
    CHECK(
        output[4] == 0u && output[5] == 0u && output[6] == 0u && output[7] == 0u
    );
}

static void test_encode_errors(void) {
    uint8_t output[16];
    size_t output_size = 0xffu;
    static const uint8_t payload[] = {0x01, 0x02, 0x03};

    CHECK_RESULT(
        wlh_raw_record_encode(
            NULL, sizeof(output), &output_size, 1u, 0u, payload, 3u
        ),
        WLH_WIRE_INVALID_ARGUMENT
    );
    CHECK_RESULT(
        wlh_raw_record_encode(
            output, sizeof(output), NULL, 1u, 0u, payload, 3u
        ),
        WLH_WIRE_INVALID_ARGUMENT
    );
    CHECK_RESULT(
        wlh_raw_record_encode(
            output, sizeof(output), &output_size, 1u, 0u, NULL, 3u
        ),
        WLH_WIRE_INVALID_ARGUMENT
    );

    /* Capacity is checked before any write: one byte short must fail. */
    memset(output, 0xa5, sizeof(output));
    CHECK_RESULT(
        wlh_raw_record_encode(output, 10u, &output_size, 1u, 0u, payload, 3u),
        WLH_WIRE_BUFFER_TOO_SMALL
    );
    CHECK(output_size == 0u);
    CHECK(output[0] == 0xa5u);

#if SIZE_MAX > UINT32_MAX
    CHECK_RESULT(
        wlh_raw_record_encode(
            output,
            sizeof(output),
            &output_size,
            1u,
            0u,
            payload,
            (size_t)UINT32_MAX + 1u
        ),
        WLH_WIRE_INVALID_LENGTH
    );
#endif
}

static void test_encode_aliased_payload(void) {
    uint8_t buffer[32];
    size_t output_size = 0;

    memset(buffer, 0, sizeof(buffer));
    buffer[8] = 0x11u;
    buffer[9] = 0x22u;
    /* Payload already sits where it belongs after the header. */
    CHECK_RESULT(
        wlh_raw_record_encode(
            buffer, sizeof(buffer), &output_size, 2u, 0u, buffer + 8, 2u
        ),
        WLH_WIRE_OK
    );
    CHECK(output_size == 10u);
    CHECK(buffer[8] == 0x11u && buffer[9] == 0x22u);
}

static void test_iterate_single(void) {
    uint8_t frame[64];
    size_t frame_size = 0;
    wlh_raw_record_iterator_t iterator;
    wlh_raw_record_view_t view;
    static const uint8_t payload[] = {0x01, 0x02, 0x03, 0x04, 0x05};

    CHECK_RESULT(
        wlh_raw_record_encode(
            frame, sizeof(frame), &frame_size, 2u, 0u, payload, sizeof(payload)
        ),
        WLH_WIRE_OK
    );
    CHECK_RESULT(
        wlh_raw_record_iterator_init(&iterator, frame, frame_size), WLH_WIRE_OK
    );
    CHECK_RESULT(wlh_raw_record_iterator_next(&iterator, &view), WLH_WIRE_OK);
    CHECK(view.record_type == 2u);
    CHECK(view.flags == 0u);
    CHECK(view.payload_size == sizeof(payload));
    CHECK(memcmp(view.payload, payload, sizeof(payload)) == 0);
    CHECK_RESULT(wlh_raw_record_iterator_next(&iterator, &view), WLH_WIRE_END);
    /* Repeated calls at end remain END. */
    CHECK_RESULT(wlh_raw_record_iterator_next(&iterator, &view), WLH_WIRE_END);
}

static void test_iterate_aggregated(void) {
    uint8_t frame[128];
    size_t frame_size = 0;
    size_t record_size = 0;
    wlh_raw_record_iterator_t iterator;
    wlh_raw_record_view_t view;
    static const uint8_t first[] = {0xaa, 0xbb};
    static const uint8_t third[] = {0x10, 0x20, 0x30};

    CHECK_RESULT(
        wlh_raw_record_encode(
            frame, sizeof(frame), &record_size, 4u, 0u, first, sizeof(first)
        ),
        WLH_WIRE_OK
    );
    frame_size += record_size;
    CHECK_RESULT(
        wlh_raw_record_encode(
            frame + frame_size,
            sizeof(frame) - frame_size,
            &record_size,
            4u,
            0u,
            NULL,
            0u
        ),
        WLH_WIRE_OK
    );
    frame_size += record_size;
    CHECK_RESULT(
        wlh_raw_record_encode(
            frame + frame_size,
            sizeof(frame) - frame_size,
            &record_size,
            2u,
            0u,
            third,
            sizeof(third)
        ),
        WLH_WIRE_OK
    );
    frame_size += record_size;

    CHECK_RESULT(
        wlh_raw_record_iterator_init(&iterator, frame, frame_size), WLH_WIRE_OK
    );
    CHECK_RESULT(wlh_raw_record_iterator_next(&iterator, &view), WLH_WIRE_OK);
    CHECK(view.record_type == 4u && view.payload_size == sizeof(first));
    CHECK(memcmp(view.payload, first, sizeof(first)) == 0);
    CHECK_RESULT(wlh_raw_record_iterator_next(&iterator, &view), WLH_WIRE_OK);
    CHECK(view.record_type == 4u && view.payload_size == 0u);
    CHECK_RESULT(wlh_raw_record_iterator_next(&iterator, &view), WLH_WIRE_OK);
    CHECK(view.record_type == 2u && view.payload_size == sizeof(third));
    CHECK(memcmp(view.payload, third, sizeof(third)) == 0);
    CHECK_RESULT(wlh_raw_record_iterator_next(&iterator, &view), WLH_WIRE_END);
}

static void test_iterate_empty_input(void) {
    wlh_raw_record_iterator_t iterator;
    wlh_raw_record_view_t view;

    CHECK_RESULT(
        wlh_raw_record_iterator_init(&iterator, NULL, 0u), WLH_WIRE_OK
    );
    CHECK_RESULT(wlh_raw_record_iterator_next(&iterator, &view), WLH_WIRE_END);
}

static void test_iterate_extended_header(void) {
    /* header_size 12 with four extra header bytes must be skipped. */
    // clang-format off
    static const uint8_t frame[] = {
        0x07, 0x00, 0x0c, 0x00, 0x02, 0x00, 0x00, 0x00,
        0xf0, 0xf1, 0xf2, 0xf3,
        0x51, 0x52
    };
    // clang-format on
    wlh_raw_record_iterator_t iterator;
    wlh_raw_record_view_t view;

    CHECK_RESULT(
        wlh_raw_record_iterator_init(&iterator, frame, sizeof(frame)),
        WLH_WIRE_OK
    );
    CHECK_RESULT(wlh_raw_record_iterator_next(&iterator, &view), WLH_WIRE_OK);
    CHECK(view.record_type == 7u);
    CHECK(view.payload_size == 2u);
    CHECK(view.payload == frame + 12);
    CHECK(view.payload[0] == 0x51u && view.payload[1] == 0x52u);
    CHECK_RESULT(wlh_raw_record_iterator_next(&iterator, &view), WLH_WIRE_END);
}

static void test_iterate_malformed(void) {
    wlh_raw_record_iterator_t iterator;
    wlh_raw_record_view_t view;

    /* Truncated header. */
    {
        static const uint8_t frame[] = {0x01, 0x00, 0x08, 0x00, 0x00};
        CHECK_RESULT(
            wlh_raw_record_iterator_init(&iterator, frame, sizeof(frame)),
            WLH_WIRE_OK
        );
        CHECK_RESULT(
            wlh_raw_record_iterator_next(&iterator, &view), WLH_WIRE_TRUNCATED
        );
    }

    /* header_size below 8. */
    {
        static const uint8_t frame[] = {
            0x01, 0x00, 0x04, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        CHECK_RESULT(
            wlh_raw_record_iterator_init(&iterator, frame, sizeof(frame)),
            WLH_WIRE_OK
        );
        CHECK_RESULT(
            wlh_raw_record_iterator_next(&iterator, &view),
            WLH_WIRE_INVALID_HEADER_SIZE
        );
    }

    /* header_size beyond input. */
    {
        static const uint8_t frame[] = {
            0x01, 0x00, 0x20, 0x00, 0x00, 0x00, 0x00, 0x00
        };
        CHECK_RESULT(
            wlh_raw_record_iterator_init(&iterator, frame, sizeof(frame)),
            WLH_WIRE_OK
        );
        CHECK_RESULT(
            wlh_raw_record_iterator_next(&iterator, &view), WLH_WIRE_TRUNCATED
        );
    }

    /* Truncated payload. */
    {
        static const uint8_t frame[] = {
            0x01, 0x00, 0x08, 0x00, 0x04, 0x00, 0x00, 0x00, 0xaa, 0xbb
        };
        CHECK_RESULT(
            wlh_raw_record_iterator_init(&iterator, frame, sizeof(frame)),
            WLH_WIRE_OK
        );
        CHECK_RESULT(
            wlh_raw_record_iterator_next(&iterator, &view), WLH_WIRE_TRUNCATED
        );
    }

    /* Length overflow attempt: payload_size 0xffffffff. */
    {
        static const uint8_t frame[] = {
            0x01, 0x00, 0x08, 0x00, 0xff, 0xff, 0xff, 0xff, 0x00
        };
        CHECK_RESULT(
            wlh_raw_record_iterator_init(&iterator, frame, sizeof(frame)),
            WLH_WIRE_OK
        );
        CHECK_RESULT(
            wlh_raw_record_iterator_next(&iterator, &view), WLH_WIRE_TRUNCATED
        );
    }

    /* Valid first record followed by trailing garbage. */
    {
        static const uint8_t frame[] = {
            0x02,
            0x00,
            0x08,
            0x00,
            0x02,
            0x00,
            0x00,
            0x00,
            0x11,
            0x22,
            0x03,
            0x00,
            0x08
        };
        CHECK_RESULT(
            wlh_raw_record_iterator_init(&iterator, frame, sizeof(frame)),
            WLH_WIRE_OK
        );
        CHECK_RESULT(
            wlh_raw_record_iterator_next(&iterator, &view), WLH_WIRE_OK
        );
        CHECK(view.record_type == 2u && view.payload_size == 2u);
        CHECK_RESULT(
            wlh_raw_record_iterator_next(&iterator, &view), WLH_WIRE_TRUNCATED
        );
    }
}

static void test_iterator_arguments(void) {
    wlh_raw_record_iterator_t iterator;
    wlh_raw_record_view_t view;
    static const uint8_t frame[] = {
        0x01, 0x00, 0x08, 0x00, 0x00, 0x00, 0x00, 0x00
    };

    CHECK_RESULT(
        wlh_raw_record_iterator_init(NULL, frame, sizeof(frame)),
        WLH_WIRE_INVALID_ARGUMENT
    );
    CHECK_RESULT(
        wlh_raw_record_iterator_init(&iterator, NULL, 4u),
        WLH_WIRE_INVALID_ARGUMENT
    );
    CHECK_RESULT(
        wlh_raw_record_iterator_init(&iterator, frame, sizeof(frame)),
        WLH_WIRE_OK
    );
    CHECK_RESULT(
        wlh_raw_record_iterator_next(NULL, &view), WLH_WIRE_INVALID_ARGUMENT
    );
    CHECK_RESULT(
        wlh_raw_record_iterator_next(&iterator, NULL), WLH_WIRE_INVALID_ARGUMENT
    );
}

int main(void) {
    test_encode_single();
    test_encode_empty_payload();
    test_encode_errors();
    test_encode_aliased_payload();
    test_iterate_single();
    test_iterate_aggregated();
    test_iterate_empty_input();
    test_iterate_extended_header();
    test_iterate_malformed();
    test_iterator_arguments();

    if (failures != 0) {
        fprintf(stderr, "%d raw record check(s) failed\n", failures);
        return 1;
    }
    printf("wlh_protocol raw record tests passed\n");
    return 0;
}
