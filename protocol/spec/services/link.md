# Link Service (`0x0001`)

| Method | ID | Kind | Payload |
|---|---:|---|---|
| HELLO | `0x0001` | Request/Response | `HelloRequest` / `HelloResponse` |
| CREDIT_UPDATE | `0x0002` | Event | `CreditUpdate` |
| HEARTBEAT | `0x0003` | Event | `Heartbeat` |
| CHANNEL_RESET | `0x0004` | Request/Response | `ChannelResetRequest` / `Empty` |
| SESSION_CHANGED | `0x8001` | Event | `SessionChangedEvent` |

链路建立前只允许 session 0 的 HELLO。成功后以 Coprocessor 的 `session_id` 为权威。Session 改变必须取消旧 pending RPC、清空 Credit、重置 Sequence 并重新 Hello。Hello 中所有上限取双方较小值；Service/Channel 只在双方均声明时启用。

`CRC32C` 是 Coprocessor 必须实现的校验能力，但不默认启用。Coprocessor 的 `HelloResponse.supported_checksum_modes` 必须包含 `SUM32` 和 `CRC32C`。Host 未在 `HelloRequest.checksum_modes` 中声明 `CRC32C` 时，协商结果必须为 `SUM32`，Coprocessor 不得要求或发送带 `CRC32C_PRESENT` 的 Frame。`CRC32C_PRESENT` 未置位时始终使用 SUM32；只有置位且已协商 CRC32C 时才使用 CRC32C。

## 枚举

### `ChecksumMode`

`UNSPECIFIED` 表示未选择，不能用于已建立链路；`SUM32` 表示使用无符号 32 位累加和；`CRC32C` 表示当 `CRC32C_PRESENT` 置位时使用 Castagnoli CRC。Hello 请求可给多个候选，响应只能返回一个选中值。Coprocessor 必须在 `supported_checksum_modes` 中声明 `SUM32` 和 `CRC32C`。

### `LinkState`

| 值 | 含义 |
|---|---|
| `UNSPECIFIED` | 未初始化或无法判断，不能作为健康状态。 |
| `NEGOTIATING` | 正在交换 Hello/能力，业务 RPC 尚不可用。 |
| `HEALTHY` | 链路正常，可收发已协商 Channel。 |
| `CONGESTED` | 链路仍可用，但 credit、queue 或 buffer 持续紧张。 |
| `RECOVERING` | 正在重同步、重置 Channel 或重新协商。 |
| `FAILED` | 自动恢复失败，需要 Adapter 或应用介入。 |

## 能力消息

### `ChannelCapability`

| 字段 | 含义 |
|---|---|
| `channel_id` | Wire Format 的 uint8 Channel ID；解码后必须 `<= 0xff`。 |
| `max_frame_payload` | 本端在该 Channel 可接收的单个 Frame payload 最大字节数，不含 24 字节 Frame Header。 |
| `max_aggregate_size` | 一次逻辑聚合可重组的最大总字节数；不支持跨 Frame 聚合时等于 `max_frame_payload`。 |
| `alignment` | payload/ DMA 起始地址要求，单位 byte；必须为 1 或 2 的幂。线上 padding 不计入业务 payload。 |
| `feature_flags` | Channel 专属能力位。未定义位必须发送 0、接收时忽略；具体位由 Channel 文档分配。 |

### `InitialCredit`

| 字段 | 含义 |
|---|---|
| `channel_id` | Credit 所属 Channel。 |
| `units` | 接收方初始授予对端的 credit 单元数。 |
| `unit_bytes` | 每个单元代表的 payload 字节数；为 0 非法。发送一个 Frame 消耗 `ceil(payload_size/unit_bytes)`。 |

### `IoPinCapability`

| 字段 | 含义 |
|---|---|
| `pin_id` | Profile 定义的逻辑 pin，不是厂商 GPIO 编号。 |
| `mode_flags` | bit0 INPUT、bit1 OUTPUT、bit2 OPEN_DRAIN、bit3 PULL_UP、bit4 PULL_DOWN；其他位保留。 |
| `adc_supported` | 此逻辑 pin 是否可由 ADC Service 读取。 |

### `ResourceLimits`

| 字段 | 含义 |
|---|---|
| `max_rpc_payload` | Coprocessor 可解码的 protobuf 最大字节数，不含 16 字节 Envelope。 |
| `max_pending_rpc` | 对端允许 Host 同时未完成的请求数量。0 表示 Service 不可用，而非无限。 |
| `kv_max_key_bytes` | KV key 最大 UTF-8 字节数。 |
| `kv_max_value_bytes` | KV value 最大 UTF-8 字节数。 |
| `kv_total_capacity` | KV 可供 WL-hosted 使用的总持久化容量，单位 byte。 |
| `kv_remaining_capacity` | Hello 时刻估算的剩余容量，仅作提示；写入仍可能失败。 |
| `user_max_rpc_payload` | User Passthrough RPC 中用户 payload 的最大字节数。 |
| `user_max_stream_message` | User Raw Stream 单条逻辑消息重组后的最大字节数。 |
| `io_pins` | 最多 32 个可暴露逻辑 pin 及其能力；未列出的 pin 不可访问。 |

## Hello

### `HelloRequest`

| 字段 | 含义 |
|---|---|
| `protocol_versions` | Host 支持的 Protocol major/minor 范围，最多 4 项。 |
| `implementation` | Host Core/Adapter 实现名，最多 48 UTF-8 字节，仅用于诊断。 |
| `implementation_version` | Host 软件版本，最多 32 UTF-8 字节，不参与 wire 兼容判断。 |
| `max_frame_size` | Host 可接收的完整 Frame 最大字节数，包含 Frame Header。 |
| `alignment` | Host 接收 buffer 的最严格对齐要求，单位 byte。 |
| `checksum_modes` | Host 支持的校验方式，最多 4 项。 |
| `services` | Host 支持的 Service 版本范围，最多 24 项。 |
| `channels` | Host 支持的 Channel 及上限，最多 16 项。 |
| `feature_bits` | 全局标准 feature bitmap；未知位忽略。具体位必须由后续 registry 分配。 |
| `max_rpc_payload` | Host 可解码的最大 protobuf payload 字节数。 |
| `transport_feature_bits` | 当前 Transport Binding 能力，例如聚合、唤醒、硬件流控；位定义属于相应 transport 文档。 |

### `HelloResponse`

| 字段 | 含义 |
|---|---|
| `selected_protocol` | 双方交集内选中的 Protocol 版本；patch 为 Coprocessor 实现修订。 |
| `session_id` | Coprocessor 本次运行生成的非 0 随机/单调标识，写入后续所有 Frame。 |
| `boot_id` | 更宽的启动标识，用于诊断 session 回绕；不参与 Frame 匹配。 |
| `implementation` | Coprocessor 实现名，最多 48 UTF-8 字节。 |
| `implementation_version` | Coprocessor Adapter/Core 软件版本，最多 32 UTF-8 字节。 |
| `max_frame_size` | 协商后双方都能接收的完整 Frame 上限。 |
| `alignment` | 协商后必须满足的 payload/DMA 对齐。 |
| `checksum_mode` | 从请求候选中选中的唯一模式；未置 `CRC32C_PRESENT` 时使用 SUM32。 |
| `supported_checksum_modes` | Coprocessor 实现的校验模式，必须包含 `SUM32` 和 `CRC32C`。 |
| `services` | 实际启用的 Service 和选定版本范围；不得包含 Host 未声明的 Service。 |
| `channels` | 实际启用 Channel 及协商后的较小上限。 |
| `initial_credits` | Coprocessor 初始授予 Host 的各 Channel Credit。未列出表示 0 credit。 |
| `feature_bits` | 实际启用的全局 feature 位，只能是双方能力交集。 |
| `limits` | Coprocessor RPC、KV、User、IO 的资源上限。 |
| `transport_feature_bits` | 实际启用的 Transport 能力交集。 |

## 运行时消息

### `CreditUpdate`

`channel_id` 指定补充的 Channel；`units` 是新增 credit，而不是新的绝对余额；`credit_epoch` 在 Channel/Session 重置时递增，用来拒绝迟到的旧 credit。uint32 回绕按序号规则比较。

### `ChannelWatermark`

| 字段 | 含义 |
|---|---|
| `channel_id` | 统计所属 Channel。 |
| `tx_queued` | 当前等待发送的 record/frame 数量。 |
| `rx_queued` | 当前等待消费的接收项数量。 |
| `high_watermark` | 本 Session 观察到的 queue 最大占用；单位与对应 queued 字段一致。 |
| `available_credit` | 发送方向当前剩余 credit 单元数。 |

### `LinkCounters`

六个字段均为本 Session 内累计、允许饱和的计数器：`buffer_allocation_failures` 是 buffer/pool 申请失败；`checksum_errors` 是 SUM32 或 CRC32C 校验错误；`sequence_gaps` 是检测到的缺失序号数；`rpc_timeouts` 是本端 pending RPC 超时；`peer_resets` 是检测到的对端重启次数；`transport_resets` 是本端执行的总线/端点重置次数。

### `Heartbeat`

| 字段 | 含义 |
|---|---|
| `session_id` | 必须与外层 Frame Header 一致，否则丢弃。 |
| `state` | 发送方当前 Link State。 |
| `uptime_ms` | 发送方自启动以来的单调毫秒数。 |
| `monotonic_ms` | 发送方 OSAL monotonic clock 快照，仅用于 RTT/停顿分析，不与对端时钟直接比较。 |
| `channels` | 最多 16 个 Channel queue/credit 摘要。 |
| `counters` | 本 Session 的链路错误累计值。 |
| `watchdog_state` | Profile 定义的只读 watchdog 状态码；0 表示未提供。 |

### `ChannelResetRequest`

`channel_id` 是要清空 queue、聚合和 credit epoch 的 Channel；`reason` 是协议标准化重置原因，0 表示管理请求。不得用 Channel Reset 暗中重启整个 Coprocessor。

### `SessionChangedEvent`

`old_session_id` 是 Host 已知旧 Session，首次启动可为 0；`new_session_id` 是新的非 0 Session；`reset_reason` 是标准/厂商映射后的启动原因；`boot_id` 用于确认这是新启动而不是重复 event。收到后必须先取消旧请求，再重新 Hello。
