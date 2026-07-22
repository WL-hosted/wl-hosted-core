# User Passthrough Service (`0x000F`, Optional)

Method `0x0001 SEND`：`UserMessageSendRequest` / `Empty`；`0x8001 RESULT` 为可选异步 event。小消息 payload 上限来自 Hello。大消息使用 USER_PASSTHROUGH Raw Stream，并以 endpoint/type/flags 标识；接收方必须有界重组，Session 变化时丢弃未完成消息。

## `UserMessageSendRequest`

| 字段 | 含义 |
|---|---|
| `endpoint_id` | Profile/应用注册的逻辑端点；0 保留，标准与私有范围应由产品协议分配。未知端点返回 NOT_FOUND。 |
| `message_type` | 端点内部的版本化消息类型；WL-hosted 不解释其内容。 |
| `flags` | bit0 `EXPECT_RESULT`、bit1 `SENSITIVE`、bit2 `IDEMPOTENT`；其他位由端点协议定义，不能与 WL-hosted 标准位冲突。 |
| `payload` | 最多 512 bytes，且不得超过 Hello `user_max_rpc_payload` 的不透明内容。超过上限使用 Raw Stream。 |

请求 Envelope 的 `request_id` 是异步关联主键。SEND 成功只表示端点接受消息；设置 EXPECT_RESULT 后，业务完成可另发 Result Event。

## `UserMessageResultEvent`

| 字段 | 含义 |
|---|---|
| `endpoint_id` | 产生结果的端点，必须与原请求一致。 |
| `message_type` | 结果类型；可等于请求类型或使用端点协议定义的响应类型。 |
| `correlation_id` | 原 SEND 的 Envelope `request_id`；不得跨 Session 关联。 |
| `result` | 端点自定义有符号业务结果。公共传输/服务错误仍使用 Event Envelope status。 |
| `payload` | 最多 512 bytes 的可选结果数据，并受协商 RPC 上限约束。 |

## Raw Stream 字段

User Record Header 中 `endpoint_id/message_type/flags` 与 RPC 相同；`message_size` 是完整逻辑消息总长度，不是当前分片长度。Record flags 必须标记 FIRST/LAST，接收方按 Session、endpoint 和消息序号有界重组。超过 `user_max_stream_message`、分片重叠、缺失或 Session 改变时丢弃整条消息并计数。
