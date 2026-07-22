# Common Proto

对应 `proto/common.proto`。这些消息被多个 Service 复用，不单独分配 Service ID。

## `Empty`

空 payload。用于没有参数或没有业务返回值的 Method。Envelope 的 status 仍必须存在，不能用“是否收到 Empty”表示成功失败。

## `Version`

| 字段 | 含义 |
|---|---|
| `major` | 不兼容版本号；不同 major 不能直接互通。 |
| `minor` | 同一 major 内向后兼容的功能版本。 |
| `patch` | 不改变 wire 语义的修订号；不参与能力交集计算。 |

## `VersionRange`

| 字段 | 含义 |
|---|---|
| `major` | 此范围对应的 Protocol Major。每个支持的 major 使用一个元素。 |
| `min_minor` | 本端能接受的最低 minor，包含该值。 |
| `max_minor` | 本端能接受的最高 minor，包含该值；必须 `>= min_minor`。 |

## `ServiceVersionRange`

| 字段 | 含义 |
|---|---|
| `service_id` | `registry.md` 分配的 uint16 Service ID；protobuf 使用 uint32，但接收时必须拒绝大于 `0xffff` 的值。 |
| `major` | 该 Service 的主版本。 |
| `min_minor` | 支持的最低 Service Minor。 |
| `max_minor` | 支持的最高 Service Minor，必须 `>= min_minor`。 |

同一 `service_id + major` 不得重复。协商时选择双方共同支持的最高 minor；没有交集表示该 Service 不可用。

## `VendorStatus`

| 字段 | 含义 |
|---|---|
| `vendor_id` | 协议注册的厂商标识，不是 USB VID、PCI VID 或芯片 ID，除非注册表明确规定。 |
| `code` | 厂商原始有符号错误码，仅用于诊断。 |
| `detail` | 最多 64 字节的不透明、版本化诊断数据；不得包含指针、C struct 或敏感凭据。 |

公共成功/失败语义始终来自 RPC Envelope 的 `status_domain/status_code`。Host 不得依赖 `VendorStatus.code` 实现跨厂商业务逻辑。
