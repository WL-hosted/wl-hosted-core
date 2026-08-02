# Wired Ethernet Service (`0x000a`)

| Method | ID | Request / Response |
|---|---:|---|
| GET_INFO | `0x0001` | `EthGetInfoRequest` / `EthGetInfoResponse` |
| LINK_STATE_CHANGED | `0x8001` | Event `EthLinkStateChangedEvent` |

v1 标准模型是 Coprocessor 提供有线以太网 MAC/PHY，Host 在其上运行自己的网络栈。数据面走 Channel `0x0a ETHERNET_ETH` 的 Ethernet Raw Record（type 1，完整 L2 frame，无 FCS），与 Wi-Fi 的 `0x02 ETHERNET_STA`/`0x03 ETHERNET_AP` 语义完全一致：Credit 按 record 计费、发送方无 Credit 不得发送、接收方即使丢弃 payload 也必须归还 Credit。该 Service 只承载控制面最小集：查询接口信息与链路状态通知；不提供启停、速率配置或统计计数。

该 Service 为 Optional。Coprocessor 仅在 Adapter 提供了 wired Ethernet 后端时在 HelloResponse 中声明 `0x000a`、Channel `0x0a` 及对应初始 Credit；未声明时 Host 的 GET_INFO 收到 NOT_SUPPORTED。链路状态变化必须由 Coprocessor 主动以 LINK_STATE_CHANGED 上报，Host 不得依赖轮询 GET_INFO 来跟踪链路。

## 枚举

`EthLinkState.DOWN` 表示链路不可用（未连接或 PHY down）；`UP` 表示可以收发帧；`UNSPECIFIED` 仅作缺省/非法值，不得出现在应答和事件中。

`EthSpeed` 取值 `10M`/`100M`/`1000M`；链路 DOWN 或速率无法确定时为 `UNSPECIFIED`。`EthDuplex` 取值 `HALF`/`FULL`；无法确定时为 `UNSPECIFIED`。

## 消息字段

### `EthGetInfoRequest`

空请求。

### `EthInfo`

| 字段 | 含义与校验 |
|---|---|
| `link_state` | 查询时接口实际链路状态。 |
| `mac_address` | 接口 MAC 地址，必须恰好 6 bytes；接口无可用地址时不得应答全零地址，而应以 ETH domain 错误拒绝请求。 |
| `speed` | 当前协商速率；DOWN 时为 `UNSPECIFIED`。 |
| `duplex` | 当前协商双工；无法确定时为 `UNSPECIFIED`。 |

### `EthLinkStateChangedEvent`

`link_state` 是变化后的状态；`speed`/`duplex` 为变化后生效的协商结果，DOWN 或未定时为 `UNSPECIFIED`。同一状态重复上报是允许的（PHY 重协商可能先 DOWN 再 UP），Host 必须容忍。
