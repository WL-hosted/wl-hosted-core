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
- `attributes` 包含 `name`、`stack_size`、`priority`。
- `task_join` 等待任务结束；Core 在 `wlh_host_stop()` / `wlh_coproc_stop()` 中用于等待 Core task 退出。
- 任务入口返回后，该任务即结束，不允许在 join 后继续引用。

## 互斥锁

```c
int (*mutex_create)(void *context, wlh_osal_mutex_t *mutex);
void (*mutex_destroy)(void *context, wlh_osal_mutex_t *mutex);
int (*mutex_lock)(void *context, wlh_osal_mutex_t *mutex, uint32_t timeout_ms);
void (*mutex_unlock)(void *context, wlh_osal_mutex_t *mutex);
```

- 普通递归式互斥锁即可，Core 不会在持有同一把锁时再次加锁。
- `mutex_lock` 在 `WLH_OSAL_NO_WAIT` 时立即返回；成功返回 0，失败返回非 0。
- `mutex_unlock` 不能失败，必须保持幂等。

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

- 32-bit 事件位掩码。
- `event_wait` 支持 `wait_all`（全部置位才返回）和 `clear_on_exit`（返回前清除已满足位）。
- `observed_bits` 返回实际观测到的位；如果 timeout 返回非 0，可以返回 0 位。
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
- `timer_stop` 必须保证停止后不再触发回调；如果回调正在运行，应等待其完成。

## 时间与调度

```c
uint64_t (*monotonic_time_ms)(void *context);
void (*sleep_ms)(void *context, uint32_t duration_ms);
void (*yield)(void *context);
bool (*in_isr)(void *context);
```

- `monotonic_time_ms` 必须单调递增，不受系统时间调整影响；挂起/唤醒后仍应递增。
- `sleep_ms` 至少睡眠指定毫秒，允许因调度稍有延迟。
- `yield` 提示调度器切换同优先级任务。
- `in_isr` 返回当前是否处于中断上下文。

## POSIX 适配

在 POSIX 桌面或 Linux/macOS 上：

```cmake
wlh_common_enable_posix_osal(BUILD_TESTING "${BUILD_TESTING}")
target_link_libraries(your_adapter PRIVATE wlh::posix_osal)
```

POSIX adapter 使用 pthread、condition variable、bounded queue 和 monotonic timer。它会自动处理 `BUILD_TESTING` 所需的额外同步。注意：Core 本身不依赖 POSIX；只有 Adapter 和目标平台选择链接 POSIX adapter。

## FreeRTOS 适配

在 ESP-IDF 或其他 FreeRTOS 环境：

```cmake
wlh_common_enable_freertos_osal()
target_link_libraries(your_adapter PRIVATE wlh::freertos_osal)
```

FreeRTOS adapter 使用 native tasks、semaphores、event groups、queues 和 software timers。需要 FreeRTOS 头文件在 include path 中。

## 自定义 OSAL 实现检查清单

- [ ] `wlh_osal_ops_valid()` 对自定义 ops 返回 true。
- [ ] 所有 create 函数在失败时清理已分配资源。
- [ ] `*_from_isr` 函数不在任务上下文被调用时产生错误（最好也能工作）。
- [ ] `monotonic_time_ms` 在挂起/唤醒后继续递增。
- [ ] `queue_send`/`receive` 在 `WLH_OSAL_NO_WAIT` 时绝不阻塞。
- [ ] `event_wait` 的 `clear_on_exit` 只清除 `bits` 中实际满足的子集。
- [ ] `timer_stop` 保证后续不再调用 callback。
- [ ] 对象大小不超过 `wlh_osal_xxx_t` 的 opaque 数组容量。

## 常见陷阱

1. **在 ISR 中调用非 `*_from_isr` 版本**：会导致死锁或断言失败。
2. **mutex_lock 不处理 `WLH_OSAL_WAIT_FOREVER`**：timeout 为 UINT32_MAX 时必须无限等待。
3. **monotonic_time_ms 使用 wall clock**：系统时间被 NTP 调整后会导致心跳误判。
4. **queue storage 未对齐**：某些平台要求队列存储按最大基本类型对齐。
5. **task_join 不清理资源**：创建的任务结束后，相关栈或 TCB 应被回收。

下一篇推荐阅读：[host_core_integration.md](host_core_integration.md) 或 [coproc_core_integration.md](coproc_core_integration.md)。
