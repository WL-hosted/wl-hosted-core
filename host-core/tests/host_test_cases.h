#ifndef WLH_HOST_TEST_CASES_H
#define WLH_HOST_TEST_CASES_H

void test_handshake_and_rpc(void);
void test_timeout_credit_and_session(void);
void test_ap_ethernet(void);
void test_ethernet_aggregated_credit_accounting(void);
void test_ethernet_tx_aggregation(void);
void test_asynchronous_transport_start(void);
void test_device_info_and_user_passthrough(void);
void test_io_adc_kv_clients(void);
void test_wifi_softap(void);
void test_large_message_allocation_failures(void);
void test_bluetooth_not_negotiated(void);
void test_bluetooth_lifecycle_and_info(void);
void test_bluetooth_hci_channel(void);
void test_bluetooth_adv_channel(void);
void test_eth_get_info_and_link_event(void);
void test_eth_data_channel(void);

#endif
