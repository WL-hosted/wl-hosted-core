# 错误模型

## Status Domain

| 值 | Domain |
|---:|---|
| 0 | NONE |
| 1 | PROTOCOL |
| 2 | TRANSPORT |
| 3 | LINK |
| 4 | WIFI |
| 5 | BLUETOOTH |
| 6 | OTA |
| 7 | PERIPHERAL |
| 8 | STORAGE |
| 9 | DEVICE_INFO |
| 10 | USER |
| `0x8000..0xffff` | VENDOR |

## 公共 Status Code

`0 OK, -1 INVALID_ARGUMENT, -2 NOT_SUPPORTED, -3 NOT_READY, -4 BUSY, -5 WOULD_BLOCK, -6 NO_MEMORY, -7 NO_CREDIT, -8 TIMEOUT, -9 CANCELLED, -10 VERSION_MISMATCH, -11 SESSION_CHANGED, -12 TRANSPORT_FAILURE, -13 PEER_UNRESPONSIVE, -14 AUTHENTICATION_FAILED, -15 NOT_FOUND, -16 ALREADY_EXISTS, -17 PERMISSION_DENIED, -18 DATA_LOSS, -19 RESOURCE_EXHAUSTED, -20 INTERNAL`。

Wi-Fi 断开原因、802.11 reason/status、HCI status 和 ATT error 是事件业务字段，不替代 RPC Status。Adapter 必须先映射到公共语义；可在 protobuf `VendorStatus` 或明确的 raw reason 字段中保留厂商码，禁止将 ESP/Beken/GD32/Bouffalo 原生返回值直接当公共 status。
