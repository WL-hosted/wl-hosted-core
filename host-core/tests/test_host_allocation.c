#include "host_test_support.h"

void test_large_message_allocation_failures(void) {
    static const uint8_t payload[] = "allocation-test";
    wlh_host_t host;
    failing_allocator_t allocator;

    memset(&host, 0, sizeof(host));
    memset(&allocator, 0, sizeof(allocator));
    host.config.buffers = (wlh_buffer_ops_t){&allocator,
                                             failing_buffer_alloc,
                                             failing_buffer_free};

    allocator.fail_at = 1u;
    assert(
        wlh_host_user_message_send(
            &host, 1u, 1u, 0u, payload, sizeof(payload) - 1u, NULL, NULL
        ) == WLH_HOST_NO_MEMORY
    );
    assert(allocator.outstanding == 0u);

    allocator.attempts = 0u;
    allocator.fail_at = 2u;
    assert(
        wlh_host_user_message_send(
            &host, 1u, 1u, 0u, payload, sizeof(payload) - 1u, NULL, NULL
        ) == WLH_HOST_NO_MEMORY
    );
    assert(allocator.outstanding == 0u);
}
