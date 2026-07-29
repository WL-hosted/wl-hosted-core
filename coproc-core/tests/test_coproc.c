#include "coproc_test_cases.h"
#include "coproc_test_support.h"

#include <stdio.h>

int main(void) {
    test_hello_wifi_and_ethernet();
    test_device_info_and_user_passthrough();
    test_io_adc_kv();
    test_optional_services_not_configured();
    test_softap();
    test_oversized_ssid_rejected();
    test_large_message_allocation_failures();
    test_bluetooth_lifecycle_and_info();
    test_bluetooth_hci();
    test_bluetooth_adv_channel();
    test_ota_hello_advertisement();
    test_ota_hello_with_bluetooth();
    test_ota_full_flow();
    test_ota_errors_idle();
    test_ota_transfer_errors();
    if (failures != 0)
        return 1;
    puts("coprocessor core tests passed");
    return 0;
}
