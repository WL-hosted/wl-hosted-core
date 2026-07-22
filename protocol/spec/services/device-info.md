# Device Information Service (`0x0009`, Optional)

Method `0x0001 GET_INFO`：`Empty` / `DeviceInfoResponse`。UID 为厂商定义的不透明 bytes。字段用于识别和诊断，不使用 JSON，也不包含厂商内部结构。

## `DeviceInfoResponse` 字段

| 字段 | 含义与校验 |
|---|---|
| `vendor` | 人类可读厂商名，最多 32 UTF-8 字节。 |
| `mcu_model` | Coprocessor SoC/模组型号，最多 32 UTF-8 字节，不得包含运行时指针或序列号。 |
| `uid` | 芯片/模组稳定唯一 ID，1..32 bytes、厂商不透明。若安全策略不允许暴露则可为空并由 capability 说明。 |
| `board_profile` | 最多 64 UTF-8 字节的固件 Profile/板卡标识，用于查 catalog 和 pin 映射。 |

响应可能包含产品身份信息，调用权限和日志策略由安全 Profile 决定。Host 不得仅凭可伪造字符串决定是否接受 OTA。
