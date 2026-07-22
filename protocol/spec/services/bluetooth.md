# Bluetooth Controller Service (`0x0003`)

| Method | ID | Request / Response |
|---|---:|---|
| INITIALIZE | `0x0001` | `BluetoothInitializeRequest` / `Empty` |
| DEINITIALIZE | `0x0002` | `BluetoothDeinitializeRequest` / `Empty` |
| ENABLE | `0x0003` | `BluetoothEnableRequest` / `Empty` |
| DISABLE | `0x0004` | `Empty` / `Empty` |
| GET_INFO | `0x0005` | `Empty` / `BluetoothControllerInfo` |
| STATE_CHANGED | `0x8001` | Event `BluetoothStateChangedEvent` |

v1 标准模型是 Coprocessor 提供 Controller、Host 运行 BLE Host Stack。HCI packet 使用 Channel `0x04` 的 H4 Record（Command/ACL/SCO/Event/ISO），而不是 protobuf。地址必须为 6 字节；HCI Command 参数以及 ACL/ISO 长度还必须受 Controller 和协商 frame 上限约束。

Beken、GD32 和 Bouffalo 都具备高层 GAP/GATT API，但能力/状态模型差异较大；它们可由未来 Optional Service 定义，不得混入 Controller v1 或用厂商 struct 透传。

## 枚举

`BluetoothTransport.UNSPECIFIED` 仅作缺省/非法值；`HCI` 表示 Host 运行 Bluetooth Host Stack、Coprocessor 暴露 Controller H4 数据流。

`BluetoothControllerState.DISABLED` 表示 Controller 已初始化或可用但未收发 HCI；`ENABLED` 表示 HCI Channel 可用；`ERROR` 表示 Controller 进入不可正常工作的状态；`UNSPECIFIED` 表示尚未获得状态。

## 消息字段

### `BluetoothInitializeRequest`

| 字段 | 含义 |
|---|---|
| `transport` | v1 必须为 HCI。未来增加其他模型需升级 Service minor。 |
| `feature_flags` | Host 请求的可选 Controller 功能 bitmap；未定义位必须为 0，Coprocessor 只启用双方能力交集。它不是标准 Bluetooth LMP feature bitmap。 |

### `BluetoothEnableRequest`

`mode_flags` 指定 Controller radio/mode 组合：Draft v1 bit0 BR/EDR、bit1 LE；0 表示使用 Profile 默认。请求不支持的组合返回 NOT_SUPPORTED，不能静默降级。

### `BluetoothDeinitializeRequest`

`release_memory=true` 允许 Adapter 释放仅供 Bluetooth 使用且之后可能无法无复位重建的内存；false 要求保留可再次 initialize 的资源。Profile 必须说明 release 后是否需要 Coprocessor reset。

### `BluetoothControllerInfo`

| 字段 | 含义与校验 |
|---|---|
| `state` | 查询时 Controller 实际状态。 |
| `public_address` | Bluetooth public device address，存在时必须恰好 6 bytes；无 public address 时为空，不得用全零地址冒充。 |
| `hci_version` | Bluetooth Assigned Numbers 中的 HCI Version 数值，低 8 位有效。 |
| `manufacturer_id` | Bluetooth SIG Company Identifier，低 16 位有效。 |
| `feature_bits` | WL-hosted Controller 能力 bitmap，不直接复制厂商 bitfield；未知位忽略。 |
| `max_hci_packet` | HCI Raw Channel 可接受的单条 record payload 最大字节数，不含 H4 type 和 8 字节 Raw Record Header；最终还受 Channel frame 上限约束。 |

### `BluetoothStateChangedEvent`

`state` 是变化后的状态；`reason` 是标准化原因，0 表示正常命令导致。Controller/HCI 原始错误应通过 Envelope Bluetooth domain status 或 HCI event 保留，不能把厂商错误直接塞入该字段。

## HCI Raw Record

Frame Raw Record 的 `record_type` 直接使用 H4 type：1 Command、2 ACL、3 SCO、4 Event、5 ISO；`payload` 不含 H4 type octet。方向必须符合 HCI：Host→Controller 通常为 Command/ACL/SCO/ISO，Controller→Host 为 Event/ACL/SCO/ISO。长度必须同时满足 HCI 包头、`max_hci_packet` 和协商 Channel 上限。
