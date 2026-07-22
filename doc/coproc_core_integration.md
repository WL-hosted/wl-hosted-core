# Coprocessor Core 集成指南

Coprocessor Core 是 WL-hosted 协议在无线 Coprocessor 上的服务端运行时，负责响应 Host 的 Hello、执行 Wi-Fi 后端操作、注入异步事件、转发 Ethernet 数据以及可选的设备信息与用户透传服务。本文档说明如何配置与集成 `wlh/coproc.h` 提供的 API。

## 公共 API 概览

```c
wlh_coproc_result_t wlh_coproc_init(wlh_coproc_t *coproc, const wlh_coproc_config_t *config);
wlh_coproc_result_t wlh_coproc_start(wlh_coproc_t *coproc);
wlh_coproc_result_t wlh_coproc_stop(wlh_coproc_t *coproc);
wlh_coproc_result_t wlh_coproc_on_frame(wlh_coproc_t *coproc, const uint8_t *frame, size_t size);

wlh_coproc_result_t wlh_coproc_wifi_scan_result(...);
wlh_coproc_result_t wlh_coproc_wifi_initialized(...);
wlh_coproc_result_t wlh_coproc_wifi_scan_completed(...);
wlh_coproc_result_t wlh_coproc_wifi_connected(...);
wlh_coproc_result_t wlh_coproc_wifi_disconnected(...);
wlh_coproc_result_t wlh_coproc_wifi_ap_client_joined(...);
wlh_coproc_result_t wlh_coproc_wifi_ap_client_left(...);
wlh_coproc_result_t wlh_coproc_ethernet_sta_send(...);
wlh_coproc_result_t wlh_coproc_user_message_result(...);

void wlh_coproc_get_diagnostics(const wlh_coproc_t *coproc, wlh_coproc_diagnostics_t *diagnostics);
```

## 配置结构体

`wlh_coproc_config_t` 包含以下回调组：

### port

```c
typedef struct wlh_coproc_port {
    void *context;
    wlh_coproc_submit_tx_fn submit_tx;
    wlh_coproc_ethernet_rx_fn ethernet_rx;
} wlh_coproc_port_t;
```

- `submit_tx`：Core 把待发送帧交给 transport。
- `ethernet_rx`：Core 收到 Ethernet STA 帧后，通过该回调交给网络栈；必须是非阻塞的拷贝/入队。

### wifi

```c
typedef struct wlh_coproc_wifi_ops {
    void *context;
    wlh_wifi_initialize_fn initialize;
    wlh_wifi_scan_fn scan;
    wlh_wifi_connect_fn connect;
    wlh_wifi_disconnect_fn disconnect;
    wlh_wifi_start_ap_fn start_ap;
    wlh_wifi_stop_ap_fn stop_ap;
} wlh_coproc_wifi_ops_t;
```

所有回调都必须是非阻塞提交；结果通过后续注入 API 返回。

### device_info

```c
typedef struct wlh_coproc_device_info_ops {
    void *context;
    wlh_coproc_get_device_info_fn get_info;
} wlh_coproc_device_info_ops_t;
```

可选。`get_info` 运行在 Core task，需快速填充 `vendor`、`mcu_model`、`uid`、`board_profile` 并返回 0。

### user_passthrough

```c
typedef struct wlh_coproc_user_passthrough_ops {
    void *context;
    wlh_coproc_user_message_fn on_message;
} wlh_coproc_user_passthrough_ops_t;
```

可选。`on_message` 收到 Host 发送的用户消息，返回 0 表示接受。

### buffers / osal

与 Host Core 相同。参见 [host_core_integration.md](host_core_integration.md) 与 [osal.md](osal.md)。

### 数值参数

| 字段 | 建议值 | 说明 |
|---|---|---|
| `max_frame_size` | 4096 | 协商上限 |
| `heartbeat_interval_ms` | 3000 | Coprocessor 发送心跳周期 |
| `initial_credit` | 16 | 每个数据 Channel 初始 credit |
| `initial_session_id` | 非 0 | 第一个 session ID |
| `core_queue_depth` | ≤ 32 | 受 `WLH_COPROC_MAX_QUEUE_DEPTH` 限制 |
| `stop_timeout_ms` | 5000 | 等待 Core task 退出 |

## Hello 协商

Coprocessor 启动后进入 `WAITING_FOR_HELLO` 状态。收到 Host 的 Hello Request 后，Core 自动生成 Hello Response，选择 session ID、协议版本、校验模式、credit 等。

```mermaid
sequenceDiagram
    participant Host as Host Core
    participant Coproc as Coproc Core
    participant WiFi as 厂商 Wi-Fi

    Host ->> Coproc : Hello Request
    Coproc ->> Coproc : 选择 session / capability
    Coproc ->> Host : Hello Response
    Host ->> Coproc : Wi-Fi Initialize Request
    Coproc ->> WiFi : initialize(operation_id)
    WiFi -->> Coproc : wifi_initialized(operation_id, OK)
    Coproc ->> Host : Wi-Fi Initialize Response
```

## Wi-Fi 后端回调约定

### initialize

```c
int my_wifi_initialize(void *context, uint32_t operation_id);
```

- 收到后应尽快启动厂商 Wi-Fi 初始化。
- 初始化完成后调用 `wlh_coproc_wifi_initialized(coproc, operation_id, backend_status)`。
- `backend_status` 为 0 表示成功；非 0 会映射为错误响应。

### scan

```c
int my_wifi_scan(void *context, uint32_t scan_id);
```

- 启动扫描。
- 每个 BSS 通过 `wlh_coproc_wifi_scan_result()` 注入。
- 扫描结束通过 `wlh_coproc_wifi_scan_completed()` 注入。

### connect

```c
int my_wifi_connect(void *context, const wlh_coproc_wifi_connect_t *request);
```

- 提取 `ssid`、`credential`、`security`，启动连接。
- 连接成功调用 `wlh_coproc_wifi_connected()`；失败调用 `wlh_coproc_wifi_disconnected()`。

### disconnect

```c
int my_wifi_disconnect(void *context);
```

- 启动断开。
- 断开后调用 `wlh_coproc_wifi_disconnected(reason, locally_initiated)`。

### start_ap / stop_ap

与 connect/disconnect 类似，分别启动/停止 SoftAP。

## 事件注入 API

所有注入 API 都是非阻塞的，可以在任意任务上下文调用（但不能在 ISR 中调用）：

```c
wlh_coproc_wifi_scan_result(coproc, scan_id, &bss);
wlh_coproc_wifi_scan_completed(coproc, scan_id, result_count, false);
wlh_coproc_wifi_connected(coproc, &bss);
wlh_coproc_wifi_disconnected(coproc, reason, false);
wlh_coproc_wifi_ap_client_joined(coproc, mac, rssi, assoc_id);
wlh_coproc_wifi_ap_client_left(coproc, mac, assoc_id, ieee_reason);
wlh_coproc_ethernet_sta_send(coproc, frame, size);
wlh_coproc_user_message_result(coproc, endpoint, type, correlation_id, result, payload, size);
```

## Ethernet 数据面

Coprocessor 收到 Ethernet 帧后：

```c
wlh_coproc_ethernet_sta_send(coproc, ethernet_frame, size);
```

Core 会封装成 Frame 并通过 `submit_tx` 发送给 Host。Host 发送给 Coprocessor 的 Ethernet 帧则通过 `ethernet_rx` 回调交给后端网络栈。

## 用户透传

收到 Host 的用户消息后，`on_message` 被调用。如果需要异步返回结果：

```c
wlh_coproc_user_message_result(
    coproc,
    message->endpoint_id,
    message->message_type,
    message->request_id,   // 关联原请求
    result,
    payload, payload_size
);
```

## 线程安全规则

- 公共 API 可以在任意任务上下文中调用，但不得在中断上下文中调用。
- Wi-Fi 后端回调运行在 Core task，不能阻塞，不能调用可能长时间等待的 SDK API。
- 后端事件注入 API 是线程安全的，内部会复制数据。
- `wlh_coproc_on_frame()` 不能在 ISR 中调用。

## 诊断

```c
wlh_coproc_diagnostics_t diag;
wlh_coproc_get_diagnostics(coproc, &diag);
```

包含状态、session_id、tx/rx 帧数、checksum 错误、sequence gap、RPC 请求数、peer reset 等。

## 常见集成错误

- 在 `wifi.scan` 中同步等待扫描完成，阻塞 Core task。
- 忘记在初始化完成后调用 `wlh_coproc_wifi_initialized()`。
- 在 ISR 中调用 `wlh_coproc_wifi_connected()` 等注入 API。
- `submit_tx` 成功后未在 tx_complete 中释放 buffer。
- `ethernet_rx` 中直接处理完整网络栈路径，导致 Core task 阻塞。

下一篇推荐阅读：[lifecycle.md](lifecycle.md)。
