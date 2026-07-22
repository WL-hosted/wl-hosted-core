# 协议架构

## 范围

WL-hosted Protocol 只定义 Host 与无线 Coprocessor 在线上传输的内容，不包含 RTOS、HAL、板级引脚、网络栈 packet 类型或厂商 SDK ABI。

协议分三层：

1. Frame Layer：边界、Channel、长度、Session、Sequence、SUM32 和可选 CRC32C。Coprocessor 必须实现两者，默认使用 SUM32。
2. Channel Layer：Link/Control RPC/Ethernet/HCI/OTA/Diagnostics/Log/User Stream 复用及 Per-Channel Credit。
3. Service Layer：固定 RPC Envelope 与每个 Method 独立的 protobuf payload。

控制面与数据面严格分离。除 Link 外的全部 Service RPC 统一走 CONTROL_RPC，Service ID 再做分派；完整 Ethernet L2 frame 分别走 STA/AP Channel。Bluetooth 的标准互操作面是 H4 HCI Raw Channel，高层 GAP/GATT 不属于 v1 Mandatory Service。OTA 镜像块、诊断流、日志和大用户消息也不得进入 protobuf。

## 跨厂商基线

本草案综合 Espressif esp-hosted-mcu、Beken BK7251、GigaDevice GD32VW55x 与 Bouffalo SDK 的共同能力，选择以下 v1 基线：

- Wi-Fi 生命周期、扫描、STA 连接、SoftAP、国家码、功耗、统计和异步事件；
- STA/AP 完整 Ethernet L2 数据面；
- Bluetooth Controller 生命周期与 H4 HCI 数据面；
- OTA、诊断及可选 IO/ADC/KV/Device Info/User Passthrough；
- Session、心跳、能力、Credit 和分层恢复。

Monitor、企业认证、WPS/DPP、漫游、TWT、CSI、FTM、高层 BLE GAP/GATT 等不稳定或非共同能力留给后续标准 Service minor version 或显式 Vendor Service。

## 不变量

- 不发送 C struct、bitfield、指针、RTOS handle 或厂商 enum。
- 所有整数端序与长度逐字节定义；protobuf 字段遵循 protobuf wire encoding。
- SSID 是任意字节串，不假定 UTF-8/NUL；MAC/BSSID/BLE 地址是恰好 6 字节。
- 能力协商是上限来源。厂商 SDK 的扫描缓存、VIF 数量、AP 客户端数不得成为 wire 常量。
- 事件中的 `request_id` 仅用于关联异步操作；响应必须由 Envelope 的 `request_id + session_id` 精确匹配。
