# ADR-0002：有线以太网数据面 Channel `0x0a`

- 状态：Accepted for Draft 0.1
- 日期：2026-08-02

## 决策

为有线以太网新增独立数据面 Channel `0x0a ETHERNET_ETH`，与 Wi-Fi 的 `0x02 ETHERNET_STA`/`0x03 ETHERNET_AP` 平级，命名沿用 `ETHERNET_` 前缀。该 Channel 复用 Ethernet Raw Record type 1（完整 L2 frame，无 FCS）与 per-record Credit 记账，Sequence/Credit/聚合语义与 `0x02`/`0x03` 完全一致。控制面不消耗新 Channel：查询接口信息与链路状态事件由 Optional Service `0x000a Wired Ethernet`（`GET_INFO` + `LINK_STATE_CHANGED`）经 CONTROL_RPC 承载。该 Channel 是 Optional 能力，仅当 Adapter 提供 wired Ethernet 后端时才在 HelloResponse 中声明并授予初始 Credit。

## 原因

ADR-0001 要求新增大数据 Channel 必须单独论证。有线以太网与 Wi-Fi STA/AP 是三个互不相同的物理接口，混用任一现有 Channel 都无法表达"帧应从哪个接口进出"，而引入接口标签字段又会破坏 Ethernet Record type 1 已发布的 payload 布局。独立 Channel 复用全部既有 wire 语义（帧头、Raw Record、Sequence、Credit、聚合），实现侧只需把现有的 interface 数组从 2 路扩到 3 路，不引入任何新编解码路径。控制面最小集（只读信息 + 链路事件）沿用「Service 不新增 Channel」的既定原则，避免为有低速率控制流量的接口浪费 Channel ID。

## 后果

`0x0a` 从 Reserved Standard 段移出并发布，此后不得复用或改义。实现必须按接口维护独立的 TX/RX 记账（interface index 0=STA、1=AP、2=ETH），且 Hello 协商只声明 Adapter 实际具备的接口。Wi-Fi 的 `0x02`/`0x03` 语义不受影响。未来新增速率配置、启停或统计属于 Service minor 演进，不得改变本 Channel 的数据面语义。
