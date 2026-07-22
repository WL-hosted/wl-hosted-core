# 兼容性与版本

Wire Protocol、Service、Core/Adapter 和固件版本相互独立。Frame 中携带 Protocol Major；共同 Minor 在 Link Hello 中协商。Major 无交集必须拒绝，Minor 选择共同支持的最高值。

每个 Service 独立声明 `(service_id, major, min_minor, max_minor)`。Host 不得调用未声明的 Service/Method。新增 proto 字段只能使用新 tag；删除字段必须在 schema 中 `reserved`；未知字段必须安全跳过。已发布的 Channel、Service、Method、enum 数值和 protobuf tag 永不复用。

兼容新增包括：新 Optional Service、新 Method、新 Event、新 optional protobuf 字段、新 enum 值及更大的协商上限。改变字段含义、固定 Header、端序、已有数值或必需状态机属于不兼容变更。

当前所有文件为 Draft 0.1，进入 Protocol 1.0 前可以通过 ADR 修订编号；一旦标记 Published，上述不复用规则立即生效。

CRC32C 是 Coprocessor 必须实现的能力，但不是必须在每个 Frame 中启用的功能。所有 Frame 始终使用校验：`CRC32C_PRESENT` 清除时使用 `CHECKSUM_MODE_SUM32`，置位时必须且只能在协商结果为 `CHECKSUM_MODE_CRC32C` 时使用 CRC32C。Host 未声明 CRC32C 时，协商结果必须为 `CHECKSUM_MODE_SUM32`。
