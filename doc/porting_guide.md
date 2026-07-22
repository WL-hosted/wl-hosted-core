# WL-hosted Core 移植指南

本文档说明如何为一个新的 MCU 或 RTOS 平台创建 WL-hosted Core 的 Adapter。Adapter 的责任是把平台相关的 transport、buffer、clock、executor 与 OSAL 注入 Core，并在硬件事件与 Core 入口之间做线程安全桥接。

## 前置条件

- 已能独立编译目标平台的 C99 项目。
- 已阅读 `architecture.md`，理解 Core 的分层与状态机。
- 已阅读 `protocol/spec/wire-format.md`，理解 Frame/RPC 结构。
- 目标平台具备可用的 heap 或静态 buffer 分配方案。

## 创建 Adapter 的步骤

```mermaid
flowchart LR
    A["1. 添加 core submodule"] --> B["2. 选择 OSAL 适配"]
    B --> C["3. 实现 transport ops"]
    C --> D["4. 实现 buffer ops"]
    D --> E["5. 实现 executor ops"]
    E --> F["6. 初始化并启动 Core"]
    F --> G["7. 桥接 RX 与状态事件"]
    G --> H["8. 集成 Wi-Fi/网络后端"]
```

### 1. 添加 core submodule

把 `wl-hosted-core` 作为 `core/` submodule 加入父仓库，然后在 CMakeLists.txt 中：

```cmake
add_subdirectory(core)
target_link_libraries(your_adapter PRIVATE wlh::host_core)   # 或 wlh::coproc_core
```

Host Core 会自动拉取 sibling `protocol/` 与 `common/`；如果父项目已经 add_subdirectory 了它们，Core 会避免重复。

### 2. 选择 OSAL 适配

Core 只依赖 `wlh/osal.h` 定义的 ops，不直接调用 pthread 或 FreeRTOS API。

#### 方式 A：复用已有适配

如果目标是 POSIX 环境或 FreeRTOS（ESP-IDF），可直接启用：

```cmake
# POSIX
wlh_common_enable_posix_osal(BUILD_TESTING "${BUILD_TESTING}")
target_link_libraries(your_adapter PRIVATE wlh::posix_osal)

# FreeRTOS
wlh_common_enable_freertos_osal()
target_link_libraries(your_adapter PRIVATE wlh::freertos_osal)
```

#### 方式 B：自定义 OSAL

如果平台 RTOS 不是上述两者，需要自行实现 `wlh_osal_ops_t` 的全部字段：

- task_create / task_join
- mutex_create / destroy / lock / unlock
- semaphore_create / destroy / take / give / give_from_isr
- event_create / destroy / wait / set / set_from_isr
- queue_create / destroy / send / send_from_isr / receive
- timer_create / destroy / start / stop
- monotonic_time_ms、sleep_ms、yield、in_isr

实现要求参见 [osal.md](osal.md)。

### 3. 实现 transport ops

Host 与 Coprocessor 都需要实现三个 transport 回调：

```c
typedef int (*wlh_transport_start_fn)(
    void *context,
    wlh_transport_lifecycle_complete_fn completion,
    void *completion_context
);

typedef int (*wlh_transport_stop_fn)(
    void *context,
    wlh_transport_lifecycle_complete_fn completion,
    void *completion_context
);

typedef int (*wlh_transport_submit_tx_fn)(
    void *context,
    uint8_t *frame,
    size_t size,
    wlh_transport_tx_complete_fn completion,
    void *completion_context
);
```

约束：

- `start`/`stop` 必须是非阻塞提交，完成时调用 `completion(status)` 恰好一次。
- `completion` 不能在 `start`/`stop` 内部同步调用，必须 defer 到 Adapter task 或中断后任务上下文。
- `submit_tx` 成功返回后，Core 放弃 `frame` 所有权；transport 负责最终调用 `completion(completion_context, frame, size, status)` 并释放 buffer。
- `submit_tx` 失败时，Core 仍持有 `frame` 所有权，通常会释放或重试。

### 4. 实现 buffer ops

最简单的方式是直接映射到平台 malloc/free：

```c
static uint8_t *my_alloc(void *context, size_t size) {
    (void)context;
    return (uint8_t *)malloc(size);
}

static void my_free(void *context, uint8_t *buffer) {
    (void)context;
    free(buffer);
}
```

在 MCU 上建议使用静态 pool，并保证 alloc/free 是线程安全的。Core 会在 ISR 上下文之外调用 buffer ops。

### 5. 实现 executor ops

Host Core 使用 executor 把事件与 completion 投递到应用线程：

```c
typedef int (*wlh_executor_post_fn)(
    void *context, wlh_task_fn task, void *task_context
);
```

- 必须是非阻塞入队，不能 inline 运行 task。
- 常见实现：事件队列、RTOS 消息队列、主循环投递。

Coprocessor Core 不需要 executor，因为事件直接通过 `wlh_coproc_xxx` 注入回 Core 队列。

### 6. 初始化并启动 Core

#### Host 示例

```c
wlh_host_t host;
wlh_host_config_t config = {0};

config.transport.context = &my_transport;
config.transport.start = my_transport_start;
config.transport.stop = my_transport_stop;
config.transport.submit_tx = my_transport_submit_tx;

config.buffers.context = NULL;
config.buffers.alloc = my_alloc;
config.buffers.free = my_free;

config.osal = my_osal_ops;
config.osal.context = NULL;

config.executor.context = &my_executor;
config.executor.post = my_executor_post;

config.on_event = my_on_event;
config.event_context = &my_app;

config.max_frame_size = 2048;
config.rpc_timeout_ms = 30000;
config.heartbeat_timeout_ms = 10000;
config.max_pending_rpc = 16;
config.core_queue_depth = 32;
config.stop_timeout_ms = 5000;
config.core_task.name = "wlh_host";
config.core_task.stack_size = 8192;
config.core_task.priority = 5;

wlh_host_init(&host, &config);
wlh_host_start(&host);
```

#### Coprocessor 示例

```c
wlh_coproc_t coproc;
wlh_coproc_config_t config = {0};

config.port.context = &my_port;
config.port.submit_tx = my_coproc_submit_tx;
config.port.ethernet_rx = my_ethernet_rx;

config.wifi.context = &my_wifi;
config.wifi.initialize = my_wifi_initialize;
config.wifi.scan = my_wifi_scan;
config.wifi.connect = my_wifi_connect;
config.wifi.disconnect = my_wifi_disconnect;
config.wifi.start_ap = my_wifi_start_ap;
config.wifi.stop_ap = my_wifi_stop_ap;

config.buffers = my_buffer_ops;
config.osal = my_osal_ops;

config.max_frame_size = 4096;
config.heartbeat_interval_ms = 3000;
config.initial_credit = 16;
config.initial_session_id = 1;
config.core_queue_depth = 32;
config.stop_timeout_ms = 5000;
config.core_task.name = "wlh_coproc";
config.core_task.stack_size = 8192;
config.core_task.priority = 5;

wlh_coproc_init(&coproc, &config);
wlh_coproc_start(&coproc);
```

### 7. 桥接 RX 与状态事件

当 transport 收到一帧时，必须在不阻塞、不重入 Core 状态机的前提下调用：

```c
wlh_host_on_frame(&host, frame, size);   // Host
wlh_coproc_on_frame(&coproc, frame, size); // Coprocessor
```

推荐做法：

- 在中断里只做最小拷贝，然后通过 OSAL queue/semaphore 通知 Adapter task。
- Adapter task 中调用 `wlh_host_on_frame()` / `wlh_coproc_on_frame()`。
- transport 连接断开时调用 `wlh_host_transport_lost(&host)`。

### 8. 集成 Wi-Fi/网络后端

Coprocessor 需要把厂商 Wi-Fi SDK 的事件映射到 Core 注入 API：

| 厂商事件 | Core 注入 API |
|---|---|
| scan result | `wlh_coproc_wifi_scan_result()` |
| scan done | `wlh_coproc_wifi_scan_completed()` |
| connected | `wlh_coproc_wifi_connected()` |
| disconnected | `wlh_coproc_wifi_disconnected()` |
| AP client joined | `wlh_coproc_wifi_ap_client_joined()` |
| AP client left | `wlh_coproc_wifi_ap_client_left()` |
| Ethernet RX (STA) | `wlh_coproc_ethernet_sta_send()` |

Host 则在 `on_event` 回调中处理 `WLH_HOST_EVENT_WIFI_SCAN_RESULT`、`WLH_HOST_EVENT_WIFI_CONNECTED` 等事件。

## 可移植性检查清单

在把 Adapter 提交到工作区之前，请确认以下事项：

- [ ] Core 构建时未隐式链接 pthread、Unix socket、poll/kqueue（`-DBUILD_TESTING=OFF` 验证）。
- [ ] `wlh_osal_ops_valid()` 返回 true。
- [ ] transport start/stop completion 恰好调用一次，且不 inline。
- [ ] `submit_tx` 成功后 transport 最终调用 tx_complete，并释放 buffer。
- [ ] RX 路径不在 ISR 中直接调用 `wlh_host_on_frame()` / `wlh_coproc_on_frame()`。
- [ ] 所有队列深度、buffer size、pending table 大小均已显式配置，无依赖默认无限值。
- [ ] 单调时钟在系统挂起/唤醒后仍然单调递增。
- [ ] Wi-Fi 后端回调不阻塞，不持有 Core buffer 引用。
- [ ] 公共 API 返回值均已被 Adapter 处理，尤其是 `NO_MEMORY`、`NO_CREDIT`、`PENDING_FULL`。

## 最小验证流程

1. 编译 Adapter 与 Core（Debug + Release）。
2. 运行 loopback 测试：Adapter 把本地生成的帧送回 `wlh_host_on_frame()`，验证 Hello 协商进入 READY。
3. 运行 Wi-Fi scan/connect 端到端测试，确认事件能到达 Host 应用。
4. 拔掉 transport 模拟断线，验证 Core 进入 RECOVERING 并自动恢复。

下一篇推荐阅读：[osal.md](osal.md)。
