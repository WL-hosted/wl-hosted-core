# 参考 SDK 对比与取舍

本文记录 schema 的跨厂商依据，避免把某一家 SDK 的偶然实现变成公共协议。

| 主题 | Espressif esp-hosted-mcu | Beken BK7251 | GD32VW55x | Bouffalo | v1 决策 |
|---|---|---|---|---|---|
| RPC | 单一巨型 proto/oneof、厂商 ID | 高层 Wi-Fi/BLE API | 高层 Wi-Fi/BLE + Virtual HCI | RNM 固定 C 消息头 | 固定通用 Envelope + 每 Service 独立 proto |
| Wi-Fi 扫描 | AP record 列表及完成事件 | callback 后取列表 | 流式 result + done，缓存约 32 | RNM 列表会受约 1514/2000 字节 transport 限制 | 每批最多 8 条 event + completed，不固化总数 |
| SSID/凭据 | 32/64 字节数组 | 32/64 字节并带长度 | SSID 32，passphrase 8..63 | 旧 RNM 有固定数组/`strlen` 风险 | `bytes` + nanopb 上限 + 语义长度校验 |
| STA/AP 数据 | 独立 hosted interface | lwIP Ethernet netif | fully-hosted VIF frame | STA/AP/Sniffer 分流 | STA/AP 独立 Raw Channel，完整 Ethernet L2，不走 protobuf |
| Bluetooth | HCI/VHCI | 高层 GAP/GATT | H4 Virtual HCI 支持 Command/ACL/SCO/Event/ISO | Zephyr 风格 GAP/GATT | v1 标准为 Controller RPC + Raw H4 HCI；高层服务后续可选 |
| 错误 | ESP error/resp | Beken error 范围 | ATT/GAP/GATT/HCI 多 domain | RNM 少量 status | 公共 domain/code + 原始 IEEE/HCI/vendor 诊断字段 |
| 能力上限 | 芯片/Kconfig 相关 | AP client 常见上限 4 | VIF/client/频段均为编译能力 | scan 常见上限 50 | 全部通过 Hello/Profile 协商，不写死厂商数值 |

主要查阅位置：

- `esp-hosted-mcu/common/proto/esp_hosted_rpc.proto`：现有请求、响应、Wi-Fi record、heartbeat 与 event。
- Beken `beken378/func/include/wlan_ui_pub.h`、`beken378/func/include/rw_msg_pub.h`、`beken378/driver/include/ble_api.h`：Wi-Fi/BLE 字段和长度；`beken378/func/lwip_intf/lwip-2.0.2/port/ethernetif.c`：L2 数据路径。
- GD32 `MSDK/macsw/export/macif_api.h`、`wifi_manager/wifi_vif.h`：VIF/scan/connect；`MSDK/ble/app/virtual_hci.h` 与 `blesw/src/export/comm_hci.h`：H4 HCI。
- Bouffalo `components/net/netbus/host_router/dev_net_mgmr/inc/rnm_msg.h`：现有 hosted RNM；`components/wireless/wifi6/fhost/include/wifi_mgmr_ext.h`：Wi-Fi 能力；`components/net/netbus/host_router/dev_net_sdio/net_wifi_transceiver.h`：CMD 与 STA/AP/Sniffer 数据分流。


未纳入 v1 标准面的能力包括 Monitor/Raw 802.11、WPS/DPP、Enterprise、TWT、CSI、FTM、漫游、高层 BLE GAP/GATT。原因不是这些能力无价值，而是共同语义、资源上限或 Host/Coprocessor 栈归属尚未稳定；后续应通过独立 Optional Service 和 capability 增加，而不是扩张 Wi-Fi/Bluetooth v1 的基础 ABI。
