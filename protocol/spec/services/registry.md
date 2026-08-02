# Service Registry

所有 ID 为 uint16。`0x0000` 无效；`0x8000..0xffff` 保留给 Vendor/Private Service。已发布 ID 不得复用。

| Service ID | 名称 | v1 级别 |
|---:|---|---|
| `0x0001` | Link | Mandatory |
| `0x0002` | Wi-Fi | Mandatory for Wi-Fi profile |
| `0x0003` | Bluetooth Controller | Mandatory for Bluetooth profile |
| `0x0004` | OTA | Optional |
| `0x0005` | Diagnostics | Mandatory |
| `0x0006` | IO | Optional |
| `0x0007` | ADC | Optional |
| `0x0008` | KV | Optional |
| `0x0009` | Device Information | Optional |
| `0x000A` | Wired Ethernet | Optional |
| `0x000F` | User Passthrough | Optional |

每个 Service 的 Method ID `0x0001..0x7fff` 为 request/response，`0x8000..0xbfff` 为标准 event，`0xc000..0xffff` 保留。RPC kind 仍是最终判据。

公共版本和厂商诊断字段见 `common.md`。
