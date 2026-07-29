#include "host_test_cases.h"

#include <stdio.h>

int main(void) {
    test_handshake_and_rpc();
    test_timeout_credit_and_session();
    test_ap_ethernet();
    test_asynchronous_transport_start();
    test_device_info_and_user_passthrough();
    test_io_adc_kv_clients();
    test_wifi_softap();
    test_large_message_allocation_failures();
    test_bluetooth_not_negotiated();
    test_bluetooth_lifecycle_and_info();
    test_bluetooth_hci_channel();
    test_bluetooth_adv_channel();
    puts("host core tests passed");
    return 0;
}
