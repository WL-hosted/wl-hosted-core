# ADR-0001：初始 Wire Header、Channel 与 RPC Registry

- 状态：Accepted for Draft 0.1
- 日期：2026-07-12

## 决策

采用 24 字节 little-endian Frame Header、SUM32 和由 Coprocessor 强制实现的可选 CRC32C、以及 16 字节 RPC Envelope、Per-Channel Sequence/Credit。`0x00` 专用于 Link，`0x01` 统一承载其余 Service RPC，`0x02..0x08` 为 Ethernet/HCI/OTA/Diagnostics/Log/User 数据面。Service/Method ID 由 `spec/services/registry.md` 及各 Service 文档分配，protobuf tag 不承担全局方法编号。

## 原因

esp-hosted-mcu 的巨型 oneof 能覆盖大量 ESP API，但会将全局命令编号、厂商结构和 codec 耦合。Bouffalo RNM 证明固定小头和请求关联有实用价值，但其 packed C 数组及 `strlen` 语义不适合作为跨厂商 ABI。Beken/GD32/Bouffalo 都要求扫描异步、Ethernet 独立流和明确事件。统一 CONTROL_RPC 既保留 Service 隔离，又避免在 Bluetooth/OTA/User Channel 中混放 Envelope 和 Raw payload。

## 后果

Frame/RPC 必须显式编码，不能直接发送 C struct。新增 Service 通常不新增 Channel；大数据能力若确需新 Channel，必须另写 ADR。Protocol 1.0 发布后，本文分配的 ID 不得复用。
