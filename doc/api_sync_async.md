# WL-hosted Core 接口同步与异步语义

本文档标明 `wlh/host.h` 和 `wlh/coproc.h` 中公开 Core API，以及 Adapter
回调的完成语义。这里的“同步”只描述**调用何时返回**；函数返回
`WLH_*_OK` 通常只表示参数校验或提交成功，不等同于链路、远端服务或硬件
操作已经完成。

## 术语与通用约定

| 标记 | 含义 |
|---|---|
| 同步 | 调用在返回前完成该接口承诺的工作；可能短暂等待锁、队列或 task join。 |
| 同步提交 | 调用在返回前完成本地资源创建或请求提交，但不等待后续链路/硬件状态。 |
| 异步请求 | 返回值只表示是否接受请求；最终结果由指定 completion 回调报告。 |
| 非阻塞 ingress | 调用复制或入队数据后返回；Core task 稍后处理。不是完成通知。 |

- 不要在 Core 的 `on_event`、RPC completion、HCI callback 或 Adapter
  completion 中调用 `wlh_host_stop()` / `wlh_coproc_stop()`；`stop()` 会等待
  Core task，可能死锁。
- 所有异步 completion 和事件都应视为稍后发生；不得依赖回调 inline 触发。
- 对标注为“非阻塞 ingress”的接口，传入的 payload/frame 在函数返回后不再由
  调用方保留给 Core 使用；调用方仍须遵循各接口对返回值、credit 和重试的约定。

## Host Core：应用调用的接口

### 生命周期与诊断

| 接口 | 语义 | 完成/注意事项 |
|---|---|---|
| `wlh_host_init` | 同步 | 校验并初始化对象；返回时状态为 `UNINITIALIZED`。 |
| `wlh_host_start` | 同步提交 | 创建 Core task 后返回，不等待 transport start、Hello 或 `READY`。通过 `WLH_HOST_EVENT_STATE_CHANGED` 观察就绪或失败。 |
| `wlh_host_stop` | 同步、可阻塞 | 提交停止并等待 Core task 与 transport stop completion，最长为 `stop_timeout_ms`。 |
| `wlh_host_get_diagnostics` | 同步 | 取得一致快照；运行中可能等待 state mutex。 |
| `wlh_host_get_peer_version` | 同步 | 返回当前缓存字符串；该值会随协商/会话变化，不等待远端查询。 |

### RPC 与服务客户端

以下接口均为**异步请求**。返回 `WLH_HOST_OK` 仅表示请求已被 Core 接受；
远端响应、远端服务错误、超时、会话变化或 transport 故障均通过传入的
completion 报告。

| 接口组 | 接口 |
|---|---|
| 通用 RPC | `wlh_host_rpc_request` |
| Wi-Fi | `wlh_host_wifi_initialize`、`wlh_host_wifi_scan`、`wlh_host_wifi_connect`、`wlh_host_wifi_disconnect`、`wlh_host_wifi_start_ap`、`wlh_host_wifi_stop_ap` |
| 蓝牙控制器 | `wlh_host_bluetooth_initialize`、`wlh_host_bluetooth_enable`、`wlh_host_bluetooth_disable`、`wlh_host_bluetooth_deinitialize`、`wlh_host_bluetooth_get_info` |
| OTA 控制 | `wlh_host_ota_begin`、`wlh_host_ota_finalize`、`wlh_host_ota_abort`、`wlh_host_ota_activate`、`wlh_host_ota_query` |
| 可选服务 | `wlh_host_get_device_info`、`wlh_host_user_message_send`、`wlh_host_io_configure`、`wlh_host_io_read`、`wlh_host_io_write`、`wlh_host_adc_read`、`wlh_host_kv_read`、`wlh_host_kv_write`、`wlh_host_kv_erase` |

`wlh_host_user_message_send` 的 completion 只对应 SEND 的 RPC acknowledgement；
后续业务 RESULT 事件通过 `WLH_HOST_EVENT_USER_MESSAGE_RESULT` 到达。

### 数据发送、链路通知与 ingress

| 接口 | 语义 | 完成/注意事项 |
|---|---|---|
| `wlh_host_on_frame` | 非阻塞 ingress | 拷贝收到的 wire frame 并排入 Core task；返回不表示 frame 已解析或已被远端业务处理。 |
| `wlh_host_transport_lost` | 非阻塞通知 | 仅提交链路丢失处理；Host 随后进入恢复流程。 |
| `wlh_host_bluetooth_hci_send` | 同步提交 | 仅提交一个 HCI record，不等控制器已处理；`NO_CREDIT` 时未入队，应在 `bluetooth_hci_tx_ready` 后重试。 |
| `wlh_host_ota_stream_send` | 同步提交 | 仅提交一个 OTA chunk，不等 flash 持久化；`NO_CREDIT` 时未入队，应在 `ota_stream_tx_ready` 后重试。 |
| `wlh_host_ethernet_sta_send`、`wlh_host_ethernet_ap_send` | 同步提交 | 仅提交 Ethernet frame 给 transport；返回不表示对端网络栈已接收。 |

## Coprocessor Core：Adapter 调用的接口

### 生命周期、诊断与 frame ingress

| 接口 | 语义 | 完成/注意事项 |
|---|---|---|
| `wlh_coproc_init` | 同步 | 校验并初始化对象。 |
| `wlh_coproc_start` | 同步提交 | 创建 Core task 后返回，不等待 Host Hello 或 `READY`。 |
| `wlh_coproc_stop` | 同步、可阻塞 | 等待 Core task 退出，最长由 `stop_timeout_ms` 约束。 |
| `wlh_coproc_get_diagnostics` | 同步 | 取得一致快照；运行中可能等待 state mutex。 |
| `wlh_coproc_on_frame` | 非阻塞 ingress | 拷贝 Host 发来的 wire frame，稍后由 Core task 处理。 |

### Adapter 事件与异步 backend completion

以下接口都是**非阻塞 ingress**：它们将 Adapter 事件或 backend completion
排入 Core task；返回值仅表示事件是否被接受，并非 wire event/RPC response
已经发送完成。

| 类别 | 接口 |
|---|---|
| Wi-Fi 事件 | `wlh_coproc_wifi_initialized`、`wlh_coproc_wifi_scan_result`、`wlh_coproc_wifi_scan_completed`、`wlh_coproc_wifi_connected`、`wlh_coproc_wifi_disconnected`、`wlh_coproc_wifi_ap_started`、`wlh_coproc_wifi_ap_stopped`、`wlh_coproc_wifi_ap_client_joined`、`wlh_coproc_wifi_ap_client_left` |
| User Passthrough | `wlh_coproc_user_message_result` |
| Bluetooth completion | `wlh_coproc_bluetooth_operation_complete`、`wlh_coproc_bluetooth_info_result` |
| OTA completion | `wlh_coproc_ota_begin_complete`、`wlh_coproc_ota_write_complete`、`wlh_coproc_ota_finalize_complete`、`wlh_coproc_ota_abort_complete`、`wlh_coproc_ota_activate_complete` |

尤其是 `wlh_coproc_ota_write_complete`：成功入队后才会由 Core 归还
`OTA_STREAM` credit；它表示 backend 已报告该 chunk 的结果，而不是调用它本身
同步执行了 flash 写入。

### 数据发送与状态上报

| 接口 | 语义 | 完成/注意事项 |
|---|---|---|
| `wlh_coproc_ethernet_sta_send`、`wlh_coproc_ethernet_ap_send` | 同步提交 | 仅提交 Ethernet frame；返回不表示 Host 网络栈已经接收。 |
| `wlh_coproc_bluetooth_hci_send` | 同步提交 | 仅提交 Controller→Host HCI packet；`NO_CREDIT` 时 backend 必须保留 packet，并在 `hci_tx_ready` 后重试。 |
| `wlh_coproc_bluetooth_fatal_error` | 同步提交 | 提交蓝牙 ERROR 状态及事件；返回不表示 Host 已观察到状态变化。 |

## Adapter 回调接口

### Host transport / executor

| 回调 | 语义 |
|---|---|
| `transport.start`、`transport.stop` | 异步请求。返回 0 表示 Adapter 已接受；必须稍后且恰好一次调用 lifecycle completion，不能 inline 或从 ISR 调用。 |
| `transport.submit_tx` | 异步请求。成功后 Adapter 暂时拥有 frame，必须稍后且恰好一次调用 `tx_complete` 归还所有权。 |
| `executor.post` | 非阻塞提交。只能排队，不能 inline 执行 task 或等待 consumer。 |
| `on_event`、RPC completion、HCI RX/TX-ready、OTA RX/TX-ready | 异步回调，由 Core 在其执行上下文触发；遵守各回调的 payload 生命周期，避免阻塞或重入 Core。 |

### Coprocessor port 与 backend ops

| 回调组 | 语义 |
|---|---|
| `port.submit_tx` | 异步请求。成功后 Adapter 拥有 frame，必须以 `wlh_coproc_tx_complete_fn` 异步归还。 |
| `port.ethernet_sta_rx`、`port.ethernet_ap_rx` | 同步、必须快速返回。Adapter 必须复制/入队，不能在 Core task 中运行网络栈。 |
| Wi-Fi ops（`initialize`、`scan`、`connect`、`disconnect`、`start_ap`、`stop_ap`） | 异步请求。返回 0 仅表示接受；结果以相应 `wlh_coproc_wifi_*` ingress 上报。 |
| Bluetooth lifecycle / GET_INFO ops | 异步请求。以相同 `operation_id` 调用 `wlh_coproc_bluetooth_operation_complete` 或 `wlh_coproc_bluetooth_info_result`。`hci_send` 则是同步、快速的 packet 交付回调。 |
| OTA `begin`、`finalize`、`abort`、`activate` | 异步请求。以相同 `operation_id` 调用相应 `wlh_coproc_ota_*_complete`。 |
| OTA `write` | 异步请求。复制/入队 chunk 后返回；持久化结果通过 `wlh_coproc_ota_write_complete` 上报。 |
| Device info、IO、ADC、KV、User Passthrough backend ops | 同步、必须非阻塞。它们运行在 Core task 中，返回值直接决定当前 RPC 的响应。 |

测试钩子（`WLH_ENABLE_TEST_HOOKS`）只服务于测试，不构成生产并发契约。

相关背景参见 [lifecycle.md](lifecycle.md)、[host_core_integration.md](host_core_integration.md)
和 [coproc_core_integration.md](coproc_core_integration.md)。
