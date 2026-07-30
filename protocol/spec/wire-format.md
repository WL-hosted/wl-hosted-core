# Wire Format v1

## 字节序和边界

固定 Header 的所有多字节整数均为 little-endian。接收方在读取 payload 前必须先验证 magic、版本、header size 和长度上限，并验证校验和。任何 reserved bit 非零均返回协议错误或丢弃，具体取决于链路是否已建立。

## Frame Header

v1 Header 固定为 24 字节：

| Offset | Size | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 2 | magic | `0x4c57`；线上字节为 `57 4c`（`WL`） |
| 2 | 1 | protocol_major | v1 为 `1` |
| 3 | 1 | header_size | v1 为 `24` |
| 4 | 1 | channel | Channel ID |
| 5 | 1 | flags | bit0 `CRC32C_PRESENT`；bit1 `AGGREGATED`；其余保留 |
| 6 | 2 | payload_size | 当前 Frame payload 字节数 |
| 8 | 4 | session_id | 协商前为 0；建立后必须匹配当前 Session |
| 12 | 4 | sequence | 每方向、每 Channel 独立递增，回绕按 uint32 处理 |
| 16 | 4 | header_checksum | 校验 24 字节 Header，计算时本字段置 0 |
| 20 | 4 | payload_checksum | 校验 payload |

`CRC32C_PRESENT` 置位时，`header_checksum` 和 `payload_checksum` 使用 CRC-32C Castagnoli；未置位时使用无符号 32 位累加和（SUM32）。SUM32 初值为 0，将数据按 little-endian 依次取 32 位字累加并在 `2^32` 上回绕；末尾不足 4 字节时按 little-endian 取值、高位字节补 0 后累加；不使用反转、补码或其他变换。两种算法计算时都将 `header_checksum` 字段置 0。Coprocessor 必须实现 CRC32C 和 SUM32；默认使用 SUM32。`payload_size` 不得超过协商的 `max_frame_size - header_size`。

## Channel ID

| ID | 名称 | Payload |
|---:|---|---|
| `0x00` | LINK_CONTROL | RPC Envelope + link protobuf |
| `0x01` | CONTROL_RPC | 除 Link 外所有 Service 的 RPC Envelope + protobuf |
| `0x02` | ETHERNET_STA | 一个或多个 Raw Record |
| `0x03` | ETHERNET_AP | 一个或多个 Raw Record |
| `0x04` | BLUETOOTH_HCI | 一个或多个 HCI Record |
| `0x05` | OTA_STREAM | OTA Stream Record |
| `0x06` | DIAGNOSTIC_STREAM | coredump/trace 等有界 Raw Record |
| `0x07` | LOG_STREAM | UTF-8/二进制日志 Record |
| `0x08` | USER_PASSTHROUGH | User Stream Record |
| `0x09` | BLUETOOTH_HCI_ADV | 一个或多个 HCI Event Record（best-effort LE 广播/扫描报告） |
| `0x0a..0x3f` | Reserved Standard | 禁止私用 |
| `0x40..0x7f` | Experimental | 不承诺兼容 |
| `0x80..0xff` | Vendor/Private | 必须由 profile 声明 |

Service 到 Channel 的路由是固定的：Link Service 只使用 `0x00`；其他标准 Service 的 Request/Response/Event 均使用 `0x01`。`0x02..0x09` 只承载数据面 Raw Record。这样 Bluetooth/OTA/User 控制不会与各自的大数据流混合，也避免为每个新 Service 消耗 Channel ID。

`0x09 BLUETOOTH_HCI_ADV` 仅承载 Controller→Host 方向的 LE 广播/扫描报告 Event Record（LE Meta 子事件 `0x02/0x0b/0x0d/0x0f`），语义为 best-effort：发送方无 Credit 时就地丢弃报告而不是背压；接收方即使丢弃 payload 也必须归还 Credit。其余 HCI（Command Complete/Status、连接事件、SM、ACL 等）必须走 `0x04 BLUETOOTH_HCI` 可靠通道。该拆分保证可靠 HCI 事件不会在共享流水线中排在广播泛洪之后（控制面 ack 的队头阻塞上界由 `0x09` 的 Credit 窗口限定）。`0x09` 由 Host 在 HelloRequest `channels` 中声明、Coprocessor 在 HelloResponse 中授予 Credit 后启用；未协商时报告退回 `0x04` 旧行为。

## RPC Envelope

RPC Envelope 固定 16 字节，紧接其后的 payload 才是 protobuf：

| Offset | Size | 字段 | 说明 |
|---:|---:|---|---|
| 0 | 2 | service_id | 见 Service Registry |
| 2 | 2 | method_id | 仅在该 Service 内唯一 |
| 4 | 4 | request_id | 0 只允许单向 Event；请求/响应必须非 0 |
| 8 | 1 | kind | 1=Request, 2=Response, 3=Event |
| 9 | 1 | flags | bit0 `MORE`; bit1 `SENSITIVE`; 其余保留 |
| 10 | 2 | payload_size | protobuf 长度，不含 Envelope |
| 12 | 2 | status_domain | Request 必须为 0；Response/完成类 Event 可携带结果 |
| 14 | 2 | status_code | little-endian signed int16；成功为 0 |

响应必须复用请求的 service/method/request ID。异步操作的结果 Event 应复用原请求 ID，纯状态广播可使用 0。错误响应允许 payload 为空；厂商诊断可使用 `VendorStatus`，但公共语义必须由 Envelope status 给出。

## Raw Record

聚合 Frame 内每条 Record 使用 8 字节头：`record_type:u8, flags:u8, header_size:u16, payload_size:u32`，均为 little-endian。`header_size` v1 为 8。解析器必须以两个长度进行越界检查，并允许跳过未知 `record_type`。

- Ethernet Record type `1`：payload 是完整 L2 frame，从目的 MAC 开始，包含 VLAN/EAPOL；不含 FCS。Channel 决定 STA/AP interface。协商上限不得小于 1514 才能声明标准 MTU 1500。
- HCI Record type 为 H4 type：1 Command、2 ACL、3 SCO、4 Event、5 ISO；payload 不包含 H4 type octet。
- OTA Record type `1`：8 字节 Record Header 后依次为 `transfer_id:u32, offset:u64, data_size:u16, flags:u16, data`。
- User Record type `1`：依次为 `endpoint_id:u32, message_type:u32, flags:u32, message_size:u32, message bytes`，允许跨 Frame 分片。

Credit 单位及 `unit_bytes` 在 Hello 中协商。发送者只有在拥有足够 Channel Credit 时才能发送非保留数据；LINK_CONTROL 心跳和 Credit Update 使用保留资源，不受普通数据耗尽影响。
