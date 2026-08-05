# WL-hosted OSAL 规范

WL-hosted Core 通过 `wlh/osal.h` 中的 `wlh_osal_ops_t` 抽象操作系统能力。本文档说明每个 ops 的语义、ISR 安全要求，以及 POSIX 与 FreeRTOS 适配的启用方式。

## 设计原则

- OS 对象 opaque：所有对象（task、mutex、semaphore、event、queue、timer）都是固定大小的 opaque struct，平台可以在其中嵌入原生对象或指针。
- 原生 handle 不跨边界：Core 不识别 `pthread_t`、`TaskHandle_t` 等类型。
- 上下文隔离：每个 ops 都有 `void *context`，便于 Adapter 传入全局状态。
- 时间统一使用毫秒：`uint32_t`；`WLH_OSAL_NO_WAIT = 0`，`WLH_OSAL_WAIT_FOREVER = UINT32_MAX`。

## 任务管理

```c
int (*task_create)(
    void *context,
    wlh_osal_task_t *task,
    const wlh_osal_task_attributes_t *attributes,
    wlh_osal_task_fn entry,
    void *argument
);
int (*task_join)(void *context, wlh_osal_task_t *task, uint32_t timeout_ms);
```

- `task_create` 创建并启动一个任务，入口为 `entry(argument)`。
- `attributes` 包含 `name`、`stack_size`、`priority`；`stack_size` 的单位统一为字节。
- `task_join` 等待任务结束；Core 在 `wlh_host_stop()` / `wlh_coproc_stop()` 中用于等待 Core task 退出。
- 任务入口返回后，该任务即结束，不允许在 join 后继续引用。

## 互斥锁

```c
int (*mutex_create)(void *context, wlh_osal_mutex_t *mutex);
void (*mutex_destroy)(void *context, wlh_osal_mutex_t *mutex);
int (*mutex_lock)(void *context, wlh_osal_mutex_t *mutex, uint32_t timeout_ms);
void (*mutex_unlock)(void *context, wlh_osal_mutex_t *mutex);
```

- 使用非递归互斥锁。调用者必须由成功加锁的同一任务配对解锁；重复解锁、跨任务解锁和持锁销毁均不属于有效调用。
- `mutex_lock` 在 `WLH_OSAL_NO_WAIT` 时立即返回；成功返回 0，失败返回非 0。
- `mutex_unlock` 的返回类型为 void，但这不表示误用必须幂等；Core 只在持锁状态调用。

## 信号量

```c
int (*semaphore_create)(
    void *context,
    wlh_osal_semaphore_t *semaphore,
    uint32_t initial_count,
    uint32_t maximum_count
);
void (*semaphore_destroy)(void *context, wlh_osal_semaphore_t *semaphore);
int (*semaphore_take)(void *context, wlh_osal_semaphore_t *semaphore, uint32_t timeout_ms);
int (*semaphore_give)(void *context, wlh_osal_semaphore_t *semaphore);
int (*semaphore_give_from_isr)(
    void *context,
    wlh_osal_semaphore_t *semaphore,
    bool *higher_priority_task_woken
);
```

- 计数信号量，`give` 递增，`take` 递减。
- `give_from_isr` 必须可在中断上下文安全调用；`higher_priority_task_woken` 用于指示是否需要上下文切换（可为 NULL）。
- `maximum_count` 由 Core 配置决定，实现应拒绝溢出。

## 事件组

```c
int (*event_create)(void *context, wlh_osal_event_t *event);
void (*event_destroy)(void *context, wlh_osal_event_t *event);
int (*event_wait)(
    void *context,
    wlh_osal_event_t *event,
    uint32_t bits,
    bool wait_all,
    bool clear_on_exit,
    uint32_t timeout_ms,
    uint32_t *observed_bits
);
int (*event_set)(void *context, wlh_osal_event_t *event, uint32_t bits);
int (*event_set_from_isr)(
    void *context,
    wlh_osal_event_t *event,
    uint32_t bits,
    bool *higher_priority_task_woken
);
```

- 为兼容 FreeRTOS event group 的内部控制位，可移植事件掩码限定为低 24 位（`WLH_OSAL_EVENT_BITS_MASK`）；实现必须拒绝其他位。
- `event_wait` 支持 `wait_all`（全部置位才返回）和 `clear_on_exit`（返回前清除已满足位）。
- `observed_bits` 可为 NULL；非 NULL 时返回请求掩码内实际满足的位。timeout 返回非 0 时其值不作保证。
- `set_from_isr` 必须中断安全。

## 队列

```c
int (*queue_create)(
    void *context,
    wlh_osal_queue_t *queue,
    void *storage,
    size_t item_size,
    size_t capacity
);
void (*queue_destroy)(void *context, wlh_osal_queue_t *queue);
int (*queue_send)(void *context, wlh_osal_queue_t *queue, const void *item, uint32_t timeout_ms);
int (*queue_send_from_isr)(
    void *context,
    wlh_osal_queue_t *queue,
    const void *item,
    bool *higher_priority_task_woken
);
int (*queue_receive)(void *context, wlh_osal_queue_t *queue, void *item, uint32_t timeout_ms);
```

- `storage` 由调用者提供，大小至少为 `item_size * capacity`。
- 队列项按字节拷贝。
- `queue_send` 在队列满且 timeout 为 0 时返回非 0；`queue_receive` 在队列空且 timeout 为 0 时返回非 0。
- Core 内部使用队列条目为 `wlh_host_job_t` / `coproc_job_t`（指针大小）。

## 定时器

```c
int (*timer_create)(
    void *context,
    wlh_osal_timer_t *timer,
    wlh_osal_timer_fn callback,
    void *argument
);
void (*timer_destroy)(void *context, wlh_osal_timer_t *timer);
int (*timer_start)(void *context, wlh_osal_timer_t *timer, uint32_t period_ms, bool periodic);
int (*timer_stop)(void *context, wlh_osal_timer_t *timer);
```

- 单次或周期定时器。
- 回调运行在定时器上下文，不能阻塞，不能调用需要长时间持有的 Core API。
- `timer_stop` 阻止尚未开始的后续回调，但不等待已经进入的回调完成。
- `timer_destroy` 必须等待在途回调完成后再释放状态；不能从该 timer 自身的 callback 中调用。

## 时间与调度

```c
uint64_t (*monotonic_time_ms)(void *context);
void (*sleep_ms)(void *context, uint32_t duration_ms);
void (*yield)(void *context);
bool (*in_isr)(void *context);
```

- `monotonic_time_ms` 必须单调递增，不受系统时间调整影响；挂起/唤醒后仍应递增。
- `sleep_ms` 至少睡眠指定毫秒，允许因调度稍有延迟。
- 除 `WLH_OSAL_NO_WAIT` 外，正数毫秒必须向上换算到至少一个 OS tick；有限等待的总时长按单一 deadline 计算，虚假唤醒不能重置 timeout。
- `yield` 提示调度器切换同优先级任务。
- `in_isr` 返回当前是否处于中断上下文。

## POSIX 适配

在 POSIX 桌面或 Linux/macOS 上：

```cmake
wlh_common_enable_posix_osal(BUILD_TESTING "${BUILD_TESTING}")
target_link_libraries(your_adapter PRIVATE wlh::posix_osal)
```

POSIX adapter 使用 pthread、condition variable、bounded queue 和 monotonic timer。无限等待与无等待 mutex 使用原生 `pthread_mutex_lock`/`pthread_mutex_trylock` 快路径；有限等待使用单调时钟 deadline。POSIX 不存在硬件 ISR 上下文，因此所有 `*_from_isr` 的 `higher_priority_task_woken` 均返回 false。注意：Core 本身不依赖 POSIX；只有 Adapter 和目标平台选择链接 POSIX adapter。

## FreeRTOS 适配

在 ESP-IDF 或其他 FreeRTOS 环境：

```cmake
wlh_common_enable_freertos_osal()
target_link_libraries(your_adapter PRIVATE wlh::freertos_osal)
```

FreeRTOS adapter 使用 native tasks、semaphores、event groups、queues 和 software timers。timer handle 在 `timer_create` 时建立，后续 start 通过 change-period/reload-mode 复用，不产生重复 create/delete。需要 FreeRTOS 头文件在 include path 中，并要求 32-bit tick、`configUSE_TIMERS=1` 和 `INCLUDE_xTimerPendFunctionCall=1`；后者用于 destroy 时与 timer daemon 同步。ESP-IDF 的 task stack 参数原生以字节计，其他 FreeRTOS 端口由 adapter 换算为 `StackType_t` 数量。

## RT-Thread 适配

在 RT-Thread (>= 5.0) 环境，例如 `wl-hosted-coproc-wch-rtt`：

```cmake
# RT_THREAD_INCLUDE_DIRS 需包含 BSP 根目录(rtconfig.h)与内核头文件目录
wlh_common_enable_rtt_osal(RT_THREAD_INCLUDE_DIRS
    "${WLH_WCH_RTT_BSP}"
    "${WLH_WCH_RTT_BSP}/rt-thread/include"
)
target_link_libraries(your_adapter PRIVATE wlh::rtt_osal)
```

RT-Thread adapter 使用 native threads、mutex、semaphore、event、message queue 和 soft timer。对象映射：

| OSAL 对象    | RT-Thread 原语                                  |
| ------------ | ----------------------------------------------- |
| task         | `rt_thread_create` + done 信号量模拟 join       |
| mutex        | `rt_mutex_create`(RT_IPC_FLAG_PRIO,可递归)      |
| semaphore    | `rt_sem_create` + `RT_IPC_CMD_SET_VLIMIT`       |
| event        | `rt_event_create`(RT_IPC_FLAG_FIFO)             |
| queue        | `rt_mq_create`(RT_IPC_FLAG_FIFO)                |
| timer        | `rt_timer_create` + `rt_timer_control` 复用 soft timer |

与通用契约的明示偏差：

1. **队列 storage 由适配器内部分配**：RT-Thread 消息队列无法构建在恰好 `item_size * capacity` 的调用者缓冲上（每消息需要对齐与头开销），因此 `queue_create` 的 `storage` 参数被忽略，内部经 `rt_mq_create` 分配。Core 中内联的 `core_queue_storage` 在 RT-Thread 上不生效。
2. **`*_from_isr` 的 `higher_priority_task_woken` 恒为 false**：RT-Thread 在中断退出时自动执行调度，无公开 API 可查询是否唤醒了更高优先级任务。
3. **优先级方向反转**：与 FreeRTOS/POSIX 一致采用"数值大 = 优先级高"，映射为 `rt_prio = RT_THREAD_PRIORITY_MAX - 1 - osal_prio`。

另外，普通等待的超时 >= 2^31 ms 视为永久等待（`rt_tick_from_millisecond` 的 int32 语义），timer period 则拒绝该范围；信号量 `maximum_count` 经 `RT_IPC_CMD_SET_VLIMIT` 由内核原生强制执行，超过上限的 give 返回失败。timer start 使用 `RT_TIMER_CTRL_SET_TIME` 与 `SET_ONESHOT`/`SET_PERIODIC` 原地更新，不重新分配对象。

## 自定义 OSAL 实现检查清单

- [ ] `wlh_osal_ops_valid()` 对自定义 ops 返回 true。
- [ ] 所有 create 函数在失败时清理已分配资源。
- [ ] `*_from_isr` 函数不在任务上下文被调用时产生错误（最好也能工作）。
- [ ] `monotonic_time_ms` 在挂起/唤醒后继续递增。
- [ ] `queue_send`/`receive` 在 `WLH_OSAL_NO_WAIT` 时绝不阻塞。
- [ ] event bits 不超出 `WLH_OSAL_EVENT_BITS_MASK`，且 `observed_bits == NULL` 可用。
- [ ] `timer_stop` 不再调度新 callback，`timer_destroy` 等待在途 callback。
- [ ] 对象大小不超过 `wlh_osal_xxx_t` 的 opaque 数组容量。

## 常见陷阱

1. **在 ISR 中调用非 `*_from_isr` 版本**：会导致死锁或断言失败。
2. **mutex_lock 不处理 `WLH_OSAL_WAIT_FOREVER`**：timeout 为 UINT32_MAX 时必须无限等待。
3. **monotonic_time_ms 使用 wall clock**：系统时间被 NTP 调整后会导致心跳误判。
4. **queue storage 未对齐**：某些平台要求队列存储按最大基本类型对齐。
5. **task_join 不清理资源**：创建的任务结束后，相关栈或 TCB 应被回收。

下一篇推荐阅读：[host_core_integration.md](host_core_integration.md) 或 [coproc_core_integration.md](coproc_core_integration.md)。
