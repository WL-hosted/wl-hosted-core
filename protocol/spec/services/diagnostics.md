# Diagnostics Service (`0x0005`)

Methods：`0x0001 PING`, `0x0002 GET_STATISTICS`, `0x0003 SET_LOG`, `0x0004 RESET_COPROCESSOR`；`0x8001 FAULT` 为 event。日志内容走 LOG_STREAM，不进入 RPC。统计读取不得清零计数，除非未来增加明确 Method。

## `LogLevel`

严重性从 `ERROR`、`WARN`、`INFO`、`DEBUG` 到 `VERBOSE` 逐步增加输出；`UNSPECIFIED` 表示保持当前/default，仅允许在查询/兼容场景，SET_LOG 中不能依赖其语义。

## 消息字段

### `DiagnosticsPingRequest` / `DiagnosticsPingResponse`

`DiagnosticsPingRequest.cookie` 是 Host 选择的 32-bit 回显值；`host_time_us` 是发送前 Host monotonic 时间。响应中的 `cookie` 和 `host_time_us` 必须原样返回，`coprocessor_uptime_us` 是生成响应时的 Coprocessor uptime。Host 可计算 RTT，但不能假设双方时钟同源。

### `MemoryPoolStatistics`

`name` 是最多 24 UTF-8 字节的稳定池名；`capacity`、`in_use` 和 `high_watermark` 使用该池自然单位（推荐 buffer 个数，单位必须由实现文档固定）；`allocation_failures` 是本 Session 内累计失败次数。`high_watermark` 必须 `>= in_use` 且不超过 capacity。

### `DiagnosticsStatistics`

| 字段 | 含义 |
|---|---|
| `uptime_ms` | Coprocessor 自启动以来毫秒数。 |
| `free_heap_bytes` | 当前可分配 heap 字节数；纯静态实现可为 0。 |
| `minimum_free_heap_bytes` | 本次启动以来观测到的最小 free heap；不适用时为 0。 |
| `pools` | 最多 16 个关键静态/动态 pool 统计。 |
| `reset_reason` | 最近一次启动的标准化 reset reason；0 未知。 |
| `assert_count` | 本固件持久记录或本次启动检测到的 assert 数，语义由 Profile 声明；不支持则 0。 |

### `DiagnosticsSetLogRequest`

`level` 是要允许输出的最高详细级别；`stream_enabled` 控制 LOG_STREAM 是否发送。关闭 stream 不要求关闭本地日志，也不能影响 LINK_CONTROL 保留资源。

### `DiagnosticsResetRequest`

`reason` 是写入重启诊断记录的管理原因，0 表示普通软件重启；`delay_ms` 是响应成功后到执行 reset 的最小延迟，0 表示尽快但必须给响应发送机会。收到响应后 Host 应预期 Session 改变。

### `DiagnosticsFaultEvent`

`fault_class` 表示协议定义的故障大类，`fault_code` 是类内代码；`summary` 是最多 96 UTF-8 字节、不得含密钥/指针的简述。完整 dump/trace 走 DIAGNOSTIC_STREAM，Event Envelope status 表示公共严重错误语义。
