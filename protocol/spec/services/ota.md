# OTA Service (`0x0004`)

Methods：`0x0001 BEGIN`, `0x0002 FINALIZE`, `0x0003 ABORT`, `0x0004 ACTIVATE`, `0x0005 QUERY`；`0x8001 PROGRESS` 为 event。payload 对应 `ota.proto` 中同名消息。

BEGIN 返回 `transfer_id/chunk_size/alignment`。镜像块只走 OTA_STREAM，offset 必须无重叠、在 image size 内并满足协商 alignment。FINALIZE 验证长度、SHA-256 和正式 profile 要求的签名；ACTIVATE 前不得信任镜像。Session 改变后 Host 必须 QUERY，不能假设可续传。

## 枚举

`OtaImageType.UNSPECIFIED` 非法；`COPROCESSOR_FIRMWARE` 表示完整 Coprocessor 固件/Profile 镜像。分区表、证书或无线固件等其他镜像类型必须另行标准化。

`OtaState`：`IDLE` 无进行中传输；`RECEIVING` 正在接收 OTA_STREAM；`VERIFYING` 正在校验 hash/签名/镜像格式；`READY_TO_ACTIVATE` 已验证且可切换；`FAILED` 本次传输不可继续；`UNSPECIFIED` 表示未知。

## 消息字段

### `OtaBeginRequest`

| 字段 | 含义与校验 |
|---|---|
| `image_type` | 镜像类型，不能为 UNSPECIFIED。 |
| `image_size` | 完整镜像字节数，必须非 0 且不超过目标 slot/capability。 |
| `sha256` | 完整镜像 SHA-256，必须恰好 32 bytes。 |
| `target_version` | Manifest 中目标固件版本，最多 32 UTF-8 字节；用于策略检查，不代替 hash。 |
| `slot` | 逻辑目标 slot 名，最多 16 UTF-8 字节；空表示由 Coprocessor 安全选择非活动 slot。不得直接传厂商分区指针/地址。 |
| `signature` | 最多 512 bytes 的 manifest/image 签名；正式 profile 必须按声明算法验证，空值只允许开发 profile。 |

### `OtaBeginResponse`

`transfer_id` 是本次非 0 传输 ID；`stream_chunk_size` 是建议且允许的最大 data 字节数；`stream_alignment` 是 offset/非末块长度对齐，必须为 1 或 2 的幂。

### `OtaFinalizeRequest`

`transfer_id` 必须对应 RECEIVING 传输；`bytes_sent` 是 Host 已发送的逻辑镜像字节数，应等于 begin 的 `image_size`。它不能替代 Coprocessor 自己统计和 hash 验证。

### `OtaAbortRequest`

`transfer_id` 指定要中止的传输。成功后状态回到 IDLE 或实现定义的 FAILED/cleanup 状态，未匹配 ID 返回 NOT_FOUND。

### `OtaActivateRequest`

`transfer_id` 必须处于 READY_TO_ACTIVATE；`reboot=true` 允许验证切换后立即重启，false 只标记下次启动切换。成功响应不保证 reboot 后仍是同一 Session。

### `OtaQueryResponse`

`state` 是当前 OTA 状态；`transfer_id` 在 IDLE 时为 0；`image_size` 是声明总长；`bytes_received` 是已持久接收且可计入验证的字节数；`target_version` 是 Begin 中已接受的版本。QUERY 不承诺可以从 `bytes_received` 续传，续传能力必须另行协商。

### `OtaProgressEvent`

`transfer_id` 关联传输；`state` 是事件时状态；`bytes_received` 是单调不减的持久接收进度。失败原因使用 Event Envelope OTA status；Event 是提示，Host 最终应 QUERY 确认。
