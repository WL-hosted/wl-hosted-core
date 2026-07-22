# 安全要求

- 所有 Frame、Envelope、protobuf 和 Raw Record 长度必须在使用前按协商上限验证，整数加法必须检查溢出。
- SSID、凭据、签名、KV、User payload 和厂商 detail 均为不可信输入；字符串还必须验证终止和编码策略。
- Wi-Fi credential、OTA signature、KV value 标记为敏感数据。实现不得在日志、错误、事件、coredump 摘要或统计中回显；处理后应在可行时清零临时 buffer。
- MAC/BSSID/BLE address 必须恰好 6 字节，IPv4/IPv6 分别恰好 4/16 字节，SHA-256 恰好 32 字节。
- SSID 为 0..32 bytes；WPA passphrase 通常为 8..63 bytes，或 64-byte hex PSK；开放网络 credential 必须为空。WEP 等兼容模式由 capability 声明并严格校验。
- 正式固件必须在 OTA finalize/activate 前验证 manifest hash 和签名。SUM32 和已协商并启用的 CRC32C 只用于传输损坏检测，不提供真实性或防重放。
- Session 改变后丢弃旧 Frame、清空旧 Credit、取消 pending RPC。未知/重复/乱序 Sequence 必须计数并按 Transport 策略处理。
- Vendor/Private Service 仍必须有版本、长度和权限检查，不允许传输厂商内部 struct。
