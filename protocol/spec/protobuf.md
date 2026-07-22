# Protobuf 与 nanopb 约束

`.proto` 是唯一 payload schema 来源；固定 Frame/RPC/Raw Record Header 不使用 protobuf。所有 schema 使用 proto3，不使用 `map`，不创建全系统 oneof。删除字段必须 `reserved`，新增字段必须向后兼容。

`ChecksumMode` 仅用于 HELLO 校验能力协商：`SUM32` 表示使用无符号 32 位累加和，`CRC32C` 表示当 Frame 设置 `CRC32C_PRESENT` 时可使用 CRC32C。Coprocessor 必须实现并在 `HelloResponse.supported_checksum_modes` 中声明 `SUM32` 和 CRC32C；Host 未声明 CRC32C 时必须协商为 `SUM32`。单个 Frame 是否使用 CRC32C 由 Wire Header flags 的 `CRC32C_PRESENT` 决定，未置该 flag 则使用 SUM32。

`proto/nanopb.options` 为每个 string/bytes/repeated 字段提供静态上限。nanopb 的 string `max_size` 包含结尾 NUL，因此 wire 内容上限 N 使用 `max_size:N+1`。生成后的 C 类型只属于 codec 内部，不得出现在 Host/Coprocessor 公共 API。

## 接收时语义校验

静态数组上限不能替代语义检查：

| 字段 | 规则 |
|---|---|
| Wi-Fi SSID | 0..32 bytes，不要求 UTF-8/NUL |
| Wi-Fi credential | OPEN 为 0；WPA 为 8..63，或 64-byte hex PSK；具体模式看 capability |
| Wi-Fi/BLE MAC address | 恰好 6 bytes |
| country alpha2 | 恰好 2 个大写 ASCII 字母 |
| SHA-256 | 恰好 32 bytes |
| UID | 1..32 bytes，厂商不透明 |
| KV key/value | 合法 UTF-8，且不超过 Hello 协商值和本地静态上限 |
| repeated scan/client/channel/capability | count 不超过 nanopb `max_count`，且不超过协商能力 |

解码前先验证 Envelope payload size，解码后验证必需语义和 enum 范围。未知 enum 值按 UNSPECIFIED/NOT_SUPPORTED 处理，不得直接作为数组下标。敏感字段不得被通用 message dump 打印。

## 大数据边界

Ethernet、802.11 monitor frame、HCI packet、OTA chunk、coredump、日志和大用户消息均不进入 protobuf。扫描/AP 客户端列表使用有界 repeated 批次；若仍超过最大 RPC payload，使用多个 event 和 Envelope `MORE`，最后发送完成 event。
