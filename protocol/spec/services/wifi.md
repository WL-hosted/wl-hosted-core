# Wi-Fi Service (`0x0002`)

| Method | ID | Request / Response |
|---|---:|---|
| INITIALIZE | `0x0001` | `WifiInitializeRequest` / `Empty` |
| DEINITIALIZE | `0x0002` | `Empty` / `Empty` |
| SCAN_START | `0x0003` | `WifiScanRequest` / `Empty` |
| SCAN_CANCEL | `0x0004` | `WifiScanCancelRequest` / `Empty` |
| CONNECT | `0x0005` | `WifiConnectRequest` / `Empty` |
| DISCONNECT | `0x0006` | `WifiDisconnectRequest` / `Empty` |
| GET_LINK_INFO | `0x0007` | `WifiLinkInfoRequest` / `WifiLinkInfo` |
| SET_COUNTRY | `0x0008` | `WifiSetCountryRequest` / `Empty` |
| GET_COUNTRY | `0x0009` | `Empty` / `WifiCountry` |
| SET_POWER_MODE | `0x000a` | `WifiSetPowerModeRequest` / `Empty` |
| GET_POWER_MODE | `0x000b` | `WifiGetPowerModeRequest` / `WifiGetPowerModeResponse` |
| GET_STATISTICS | `0x000c` | `WifiStatisticsRequest` / `WifiStatistics` |
| START_AP | `0x000d` | `WifiStartApRequest` / `Empty` |
| STOP_AP | `0x000e` | `Empty` / `Empty` |
| GET_AP_CLIENTS | `0x000f` | `Empty` / `WifiGetApClientsResponse` |

Events：`0x8001 SCAN_RESULT`, `0x8002 SCAN_COMPLETED`, `0x8003 CONNECTED`, `0x8004 DISCONNECTED`, `0x8005 AP_CLIENT_JOINED`, `0x8006 AP_CLIENT_LEFT`，payload 为同名 `Event` message。

`SCAN_START` 成功只表示已接受；结果以最多 8 条一批的事件发送，结束必须发送 `SCAN_COMPLETED`。`CONNECT` 与 `START_AP` 同理，最终状态由 event/get 方法确认。Status 在 Envelope，schema 不重复厂商 `resp` 字段。

SSID 是 0..32 字节；MAC/BSSID 是 6 字节；credential 不得出现在响应或事件。国家码是两个大写 ASCII 字母。STA/AP Ethernet 使用 `0x02/0x03` Raw Channel，完整 L2 frame 包含 EAPOL/VLAN 且不含 FCS。Host 持有 TCP/IP 栈，因此 v1 不传 DHCP/IP 配置事件。

## 枚举

### `WifiInterface`

`UNSPECIFIED` 仅作缺省/非法值；`STA` 表示基础设施 Station；`AP` 表示 SoftAP。v1 每种最多一个逻辑接口，数据面分别映射到 ETHERNET_STA/AP Channel。

### `WifiBand`

`UNSPECIFIED` 表示不限制或未知；`2_4_GHZ`、`5_GHZ`、`6_GHZ` 表示 IEEE 802.11 频段。是否支持由 capability/Profile 决定，不能从 enum 存在推断硬件支持。

### `WifiSecurity`

| 值 | 含义 |
|---|---|
| `UNSPECIFIED` | 扫描结果无法判断；配置请求中非法。 |
| `OPEN` | 无链路层认证/加密，credential 必须为空。 |
| `WEP` | 兼容 WEP，仅 capability 明确声明时允许。 |
| `WPA_PSK` | WPA Personal。 |
| `WPA2_PSK` | WPA2 Personal。 |
| `WPA_WPA2_PSK` | WPA/WPA2 transition。 |
| `WPA3_SAE` | WPA3 Personal SAE。 |
| `WPA2_WPA3_PSK` | WPA2/WPA3 transition。 |
| `OWE` | Opportunistic Wireless Encryption，无口令。 |
| `WPA2_ENTERPRISE` | WPA2 Enterprise；v1 尚未定义证书/身份字段，基础 CONNECT 应返回 NOT_SUPPORTED。 |
| `WPA3_ENTERPRISE` | WPA3 Enterprise；同上。 |

### `WifiPowerMode`

`ACTIVE` 禁用 modem power save；`MIN_MODEM` 在保持较低延迟的情况下省电；`MAX_MODEM` 优先省电，延迟取决于 AP listen/DTIM。`UNSPECIFIED` 不能用于 SET。

### `WifiPhy`

`11B/11G/11A/11N/11AC/11AX` 表示当前 BSS 协商或扫描观察到的最高 PHY 世代；`UNSPECIFIED` 表示未知。它不编码带宽、MCS 或空间流数量。

### `WifiDisconnectReason`

| 值 | 含义 |
|---|---|
| `UNSPECIFIED` | Adapter 无法归一化；应同时查看原始 IEEE reason。 |
| `LOCAL_REQUEST` | Host/Coprocessor 主动断开。 |
| `AP_NOT_FOUND` | 扫描阶段未找到目标 BSS。 |
| `AUTH_FAILED` | 802.11 authentication 或凭据校验失败。 |
| `ASSOC_FAILED` | association 被拒绝或超时。 |
| `HANDSHAKE_TIMEOUT` | WPA/WPA2/WPA3 key handshake 未完成。 |
| `BEACON_LOST` | 已连接后持续丢失 beacon。 |
| `PEER_DEAUTH` | AP 发起 deauth/disassoc。 |
| `LINK_LOST` | 其他无线/驱动原因造成链路丢失。 |

## 生命周期与扫描消息

### `WifiInitializeRequest`

`interface_flags` 是要初始化的接口 bitmap：bit0 STA、bit1 AP；其他位保留并必须为 0。它声明资源需求，不表示接口已经连接或启动。

### `WifiScanRequest`

| 字段 | 含义与校验 |
|---|---|
| `scan_id` | Host 生成的非 0 操作 ID，在 result/completed/cancel 中保持一致；不得与当前未结束扫描重复。 |
| `interface` | 执行扫描的接口，v1 必须为 STA。 |
| `ssid` | 可选定向 SSID，0 字节表示任意，最大 32 字节。 |
| `bssid` | BSSID filter 值；启用时必须恰好 6 字节。 |
| `bssid_filter_enabled` | true 才解释 `bssid`；false 时 `bssid` 必须为空。 |
| `band` | 指定频段；UNSPECIFIED 表示所有已启用频段。 |
| `channel` | 0 表示该 band 的全部合法信道；非 0 必须符合国家码和能力。 |
| `include_hidden` | 是否在结果中保留无 SSID/隐藏 SSID BSS。 |
| `passive` | true 使用被动扫描；false 使用主动扫描。 |
| `active_min_ms` | 主动扫描每信道最短驻留时间；0 使用 Profile 默认值。 |
| `active_max_ms` | 主动扫描每信道最长驻留时间；0 使用默认，非 0 必须 `>= active_min_ms`。 |
| `passive_ms` | 被动扫描每信道驻留时间；0 使用默认。 |
| `max_results` | Host 希望接收的结果上限；0 表示在协议/资源上限内不额外限制，不保证 SDK 能缓存同样数量。 |

### `WifiScanCancelRequest`

`scan_id` 指向当前扫描。成功响应只表示接受取消；仍须以同 ID 的 `WifiScanCompletedEvent.cancelled=true` 结束。已完成/未知 ID 返回 NOT_FOUND。

### `WifiNetwork`

| 字段 | 含义与校验 |
|---|---|
| `ssid` | 原始 SSID 0..32 bytes，不保证 UTF-8。 |
| `bssid` | AP BSSID，恰好 6 bytes。 |
| `band` | 观察到的频段。 |
| `channel` | 802.11 primary channel；未知可为 0。 |
| `center_frequency_mhz` | primary channel 中心频率，单位 MHz；未知可为 0。与 channel 同时存在时必须一致。 |
| `rssi_dbm` | 接收信号强度 dBm，使用有符号值；未知使用实现定义最小值而非把负数转 uint8。 |
| `security` | 归一化认证模式。 |
| `phy` | 观察到的最高 PHY 世代。 |
| `capability_flags` | bit0 hidden、bit1 WPS、bit2 PMF capable、bit3 PMF required；其他位保留。 |

### `WifiScanResultEvent`

`scan_id` 关联请求；`networks` 是本批结果，nanopb 最多 8 项。Envelope `MORE` 表示后续仍有结果批次，但无论该标志如何都必须以 Completed Event 收尾。

### `WifiScanCompletedEvent`

`scan_id` 关联请求；`result_count` 是实际成功上报给 Host 的总 BSS 数；`cancelled` 表示扫描因取消结束；`dropped_results` 是因 SDK 缓存、RPC 上限或 backpressure 未能上报的数量。扫描失败由 Event Envelope status 表示。

## STA 连接和状态

### `WifiConnectRequest`

| 字段 | 含义与校验 |
|---|---|
| `ssid` | 目标 SSID，1..32 bytes；不依赖 NUL。 |
| `credential` | 最大 64 bytes。OPEN/OWE 必须为空；WPA passphrase 通常 8..63，64 bytes 仅允许 hex PSK。禁止回显。 |
| `security` | 期望的最低/明确安全模式，UNSPECIFIED 非法。 |
| `bssid` | 可选锁定 BSSID；启用时恰好 6 bytes。 |
| `bssid_lock` | true 时只连接指定 BSSID；false 时 bssid 必须为空。 |
| `band` | 可选频段限制；UNSPECIFIED 不限制。 |
| `channel_hint` | 0 表示未知；非 0 只是加速提示，除非 bssid_lock=true，否则实现可回退全信道扫描。 |
| `timeout_ms` | 从接受请求到 Connected/Disconnected 结果的 deadline；0 使用 Profile 默认值。 |
| `pmf_required` | true 要求 Protected Management Frames，不支持时返回 NOT_SUPPORTED。 |

### `WifiDisconnectRequest` 与 `WifiLinkInfoRequest`

两者的 `interface` 都指定目标接口；v1 CONNECT 仅使用 STA。DISCONNECT 响应表示命令接受，最终状态由 Disconnected Event 确认。GET_LINK_INFO 可查询 STA 或 AP。

### `WifiLinkInfo`

| 字段 | 含义 |
|---|---|
| `interface` | 状态所属 STA/AP。 |
| `connected` | STA 是否关联，或 AP 是否已启动。false 时后续 BSS 字段可为空/0。 |
| `ssid` | 当前 BSS/SoftAP SSID，0..32 bytes。 |
| `bssid` | 对端 AP BSSID或本地 SoftAP BSSID，connected=true 时必须 6 bytes。 |
| `band` | 当前工作频段。 |
| `channel` | 当前 primary channel。 |
| `center_frequency_mhz` | 当前中心频率 MHz。 |
| `rssi_dbm` | STA 到 AP 的当前 RSSI；AP 接口或未知时为 0。 |
| `security` | 当前链路/SoftAP 安全模式。 |
| `phy` | 当前协商最高 PHY。 |
| `mac` | 本接口 MAC，必须恰好 6 bytes。 |

## 国家码、功耗和统计

`WifiCountry.alpha2` 是恰好两个大写 ASCII 字母的 ISO 3166-1 alpha-2 国家码；`00` 表示世界域仅在 Profile 允许时使用。`WifiSetCountryRequest.country` 是待应用值，成功后影响扫描、连接和 AP 合法信道。

`WifiSetPowerModeRequest.interface` 指定 STA/AP，`mode` 是目标模式；`WifiGetPowerModeRequest.interface` 指定查询对象；`WifiGetPowerModeResponse.mode` 返回实际生效模式，不是最后请求值。

`WifiStatisticsRequest.interface` 指定计数接口。`WifiStatistics` 中 `tx_packets/tx_bytes` 是成功交给无线数据面的累计量；`tx_dropped` 是资源/队列丢弃；`tx_retries` 是 MAC 重传；`rx_packets/rx_bytes` 是成功交给 Host 的累计量；`rx_dropped` 是接收后因资源/校验丢弃；`rx_errors` 是 PHY/MAC/解封装错误；`current_rssi_dbm` 仅 STA 已连接时有效。所有 uint64 计数自本次 initialize/session 起累计，允许回绕/重置，读取不清零。

## SoftAP

### `WifiStartApRequest`

| 字段 | 含义与校验 |
|---|---|
| `ssid` | SoftAP SSID，1..32 bytes。 |
| `credential` | 最大 64 bytes；规则与 Connect 相同且不得回显。 |
| `security` | AP 安全模式；WEP、Enterprise、OWE 是否可用由 capability 决定。 |
| `band` | AP 频段，不能为 UNSPECIFIED，除非 Profile 定义自动选择。 |
| `channel` | 0 表示实现自动选择；非 0 必须符合当前国家码。 |
| `hidden` | true 时 beacon 不公开 SSID，但不构成安全机制。 |
| `max_clients` | 希望允许的最大关联客户端；必须为 1..协商上限，不能假定 Beken 的 4 或其他 SDK 编译值。 |
| `beacon_interval_tu` | Beacon interval，单位 TU（1024 µs）；0 使用 Profile 默认。 |
| `pmf_required` | 是否强制客户端支持 PMF。 |

### `WifiApClient`

`mac` 是客户端地址，恰好 6 bytes；`rssi_dbm` 是最近/平均 RSSI，未知为 0；`association_id` 是本次关联的 802.11 AID，不可作为跨重连稳定身份。

`WifiGetApClientsResponse.clients` 最多返回 16 项；若实际协商上限更大，未来必须分页，不能静默截断。`WifiConnectedEvent.link` 是连接完成时的完整快照。

`WifiDisconnectedEvent.interface` 标识断开的接口；`reason` 是公共归一原因；`ieee80211_reason` 保留 uint16 IEEE 802.11 reason/status（大于 `0xffff` 非法）；`locally_initiated` 区分本地命令与无线对端/链路原因。

`WifiApClientJoinedEvent.client` 是加入客户端快照。`WifiApClientLeftEvent.mac` 和 `association_id` 标识离开者，`ieee80211_reason` 保留 deauth/disassoc 原因。
