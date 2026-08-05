#include "wlh/rtt_osal.h"

#include <stdlib.h>
#include <string.h>

#include <rtthread.h>

#define RTT_OSAL_DEFAULT_STACK_SIZE 4096u
#define RTT_OSAL_DEFAULT_PRIORITY 5u
#define RTT_OSAL_TIME_SLICE_TICKS 10u

typedef struct rtt_osal_task_state {
    rt_sem_t done;
    wlh_osal_task_fn entry;
    void *argument;
} rtt_osal_task_state_t;

typedef struct rtt_osal_timer_state {
    rt_timer_t handle;
    wlh_osal_timer_fn callback;
    void *argument;
} rtt_osal_timer_state_t;

typedef struct rtt_osal_queue_state {
    rt_mq_t handle;
    size_t item_size;
} rtt_osal_queue_state_t;

static rt_int32_t timeout_ticks(uint32_t timeout_ms) {
    if (timeout_ms == WLH_OSAL_WAIT_FOREVER)
        return RT_WAITING_FOREVER;
    if (timeout_ms == WLH_OSAL_NO_WAIT)
        return 0;
    return rt_tick_from_millisecond((rt_int32_t)timeout_ms);
}

static void task_trampoline(void *parameter) {
    rtt_osal_task_state_t *state = parameter;
    state->entry(state->argument);
    /* The task is finished: a successful join frees the state. The kernel
     * reclaims the thread control block and stack through the defunct queue. */
    rt_sem_release(state->done);
}

static int rtt_task_create(
    void *context,
    wlh_osal_task_t *task,
    const wlh_osal_task_attributes_t *attributes,
    wlh_osal_task_fn entry,
    void *argument
) {
    rtt_osal_task_state_t *state;
    rt_thread_t thread;
    rt_uint8_t priority;
    int32_t osal_priority;
    (void)context;

    if (task == NULL || entry == NULL)
        return -1;
    osal_priority =
        attributes != NULL ? attributes->priority : RTT_OSAL_DEFAULT_PRIORITY;
    /* RT-Thread numbers priorities the other way round; invert so that a
     * larger OSAL value means higher priority, like the FreeRTOS adapter.
     * Out-of-range values fail creation instead of tripping a kernel
     * assertion on an invalid inverted priority. */
    if (osal_priority < 0 || osal_priority >= RT_THREAD_PRIORITY_MAX)
        return -1;
    priority = (rt_uint8_t)(RT_THREAD_PRIORITY_MAX - 1 - osal_priority);

    state = calloc(1u, sizeof(*state));
    if (state == NULL)
        return -1;
    state->done = rt_sem_create("wlh", 0, RT_IPC_FLAG_FIFO);
    if (state->done == NULL) {
        free(state);
        return -1;
    }
    state->entry = entry;
    state->argument = argument;

    thread = rt_thread_create(
        attributes != NULL && attributes->name != NULL ? attributes->name
                                                       : "wlh",
        task_trampoline,
        state,
        attributes != NULL && attributes->stack_size != 0u
            ? (rt_uint32_t)attributes->stack_size
            : RTT_OSAL_DEFAULT_STACK_SIZE,
        priority,
        RTT_OSAL_TIME_SLICE_TICKS
    );
    if (thread == NULL) {
        rt_sem_delete(state->done);
        free(state);
        return -1;
    }
    if (rt_thread_startup(thread) != RT_EOK) {
        rt_thread_delete(thread);
        rt_sem_delete(state->done);
        free(state);
        return -1;
    }
    memset(task, 0, sizeof(*task));
    task->opaque[0] = (uintptr_t)state;
    return 0;
}

static int rtt_task_join(
    void *context, wlh_osal_task_t *task, uint32_t timeout_ms
) {
    rtt_osal_task_state_t *state;
    (void)context;

    if (task == NULL || task->opaque[0] == 0u)
        return -1;
    state = (rtt_osal_task_state_t *)task->opaque[0];
    if (rt_sem_take(state->done, timeout_ticks(timeout_ms)) != RT_EOK) {
        /* The task is still running and owns the state; do not free it. */
        return -1;
    }
    rt_sem_delete(state->done);
    free(state);
    task->opaque[0] = 0u;
    return 0;
}

static int rtt_mutex_create(void *context, wlh_osal_mutex_t *mutex) {
    rt_mutex_t handle;
    (void)context;
    if (mutex == NULL)
        return -1;
    handle = rt_mutex_create("wlh", RT_IPC_FLAG_PRIO);
    if (handle == NULL)
        return -1;
    memset(mutex, 0, sizeof(*mutex));
    mutex->opaque[0] = (uintptr_t)handle;
    return 0;
}

static void rtt_mutex_destroy(void *context, wlh_osal_mutex_t *mutex) {
    (void)context;
    if (mutex != NULL && mutex->opaque[0] != 0u) {
        rt_mutex_delete((rt_mutex_t)mutex->opaque[0]);
        mutex->opaque[0] = 0u;
    }
}

static int rtt_mutex_lock(
    void *context, wlh_osal_mutex_t *mutex, uint32_t timeout_ms
) {
    (void)context;
    if (mutex == NULL || mutex->opaque[0] == 0u)
        return -1;
    return rt_mutex_take(
               (rt_mutex_t)mutex->opaque[0], timeout_ticks(timeout_ms)
           ) == RT_EOK
               ? 0
               : -1;
}

static void rtt_mutex_unlock(void *context, wlh_osal_mutex_t *mutex) {
    (void)context;
    if (mutex != NULL && mutex->opaque[0] != 0u)
        (void)rt_mutex_release((rt_mutex_t)mutex->opaque[0]);
}

static int rtt_semaphore_create(
    void *context,
    wlh_osal_semaphore_t *semaphore,
    uint32_t initial_count,
    uint32_t maximum_count
) {
    rt_sem_t handle;
    (void)context;
    if (semaphore == NULL || maximum_count == 0u ||
        maximum_count > (uint32_t)RT_SEM_VALUE_MAX ||
        initial_count > maximum_count)
        return -1;
    handle = rt_sem_create("wlh", (rt_uint32_t)initial_count, RT_IPC_FLAG_FIFO);
    if (handle == NULL)
        return -1;
    /* RT-Thread semaphores start with RT_SEM_VALUE_MAX as the limit; clamp to
     * the OSAL maximum so release past it fails natively. */
    if (rt_sem_control(
            handle, RT_IPC_CMD_SET_VLIMIT, (void *)(rt_uintptr_t)maximum_count
        ) != RT_EOK) {
        rt_sem_delete(handle);
        return -1;
    }
    memset(semaphore, 0, sizeof(*semaphore));
    semaphore->opaque[0] = (uintptr_t)handle;
    return 0;
}

static void rtt_semaphore_destroy(
    void *context, wlh_osal_semaphore_t *semaphore
) {
    (void)context;
    if (semaphore != NULL && semaphore->opaque[0] != 0u) {
        rt_sem_delete((rt_sem_t)semaphore->opaque[0]);
        semaphore->opaque[0] = 0u;
    }
}

static int rtt_semaphore_take(
    void *context, wlh_osal_semaphore_t *semaphore, uint32_t timeout_ms
) {
    (void)context;
    if (semaphore == NULL || semaphore->opaque[0] == 0u)
        return -1;
    return rt_sem_take(
               (rt_sem_t)semaphore->opaque[0], timeout_ticks(timeout_ms)
           ) == RT_EOK
               ? 0
               : -1;
}

static int rtt_semaphore_give(void *context, wlh_osal_semaphore_t *semaphore) {
    (void)context;
    if (semaphore == NULL || semaphore->opaque[0] == 0u)
        return -1;
    return rt_sem_release((rt_sem_t)semaphore->opaque[0]) == RT_EOK ? 0 : -1;
}

static int rtt_semaphore_give_from_isr(
    void *context,
    wlh_osal_semaphore_t *semaphore,
    bool *higher_priority_task_woken
) {
    int result;
    (void)context;
    if (semaphore == NULL || semaphore->opaque[0] == 0u)
        return -1;
    result = rt_sem_release((rt_sem_t)semaphore->opaque[0]) == RT_EOK ? 0 : -1;
    if (higher_priority_task_woken != NULL)
        *higher_priority_task_woken = false;
    return result;
}

static int rtt_event_create(void *context, wlh_osal_event_t *event) {
    rt_event_t handle;
    (void)context;
    if (event == NULL)
        return -1;
    handle = rt_event_create("wlh", RT_IPC_FLAG_FIFO);
    if (handle == NULL)
        return -1;
    memset(event, 0, sizeof(*event));
    event->opaque[0] = (uintptr_t)handle;
    return 0;
}

static void rtt_event_destroy(void *context, wlh_osal_event_t *event) {
    (void)context;
    if (event != NULL && event->opaque[0] != 0u) {
        rt_event_delete((rt_event_t)event->opaque[0]);
        event->opaque[0] = 0u;
    }
}

static int rtt_event_wait(
    void *context,
    wlh_osal_event_t *event,
    uint32_t bits,
    bool wait_all,
    bool clear_on_exit,
    uint32_t timeout_ms,
    uint32_t *observed_bits
) {
    rt_uint32_t recved = 0u;
    rt_uint8_t option;
    rt_err_t result;
    (void)context;
    if (event == NULL || event->opaque[0] == 0u || bits == 0u ||
        (bits & ~WLH_OSAL_EVENT_BITS_MASK) != 0u)
        return -1;
    option = wait_all ? RT_EVENT_FLAG_AND : RT_EVENT_FLAG_OR;
    if (clear_on_exit)
        option |= RT_EVENT_FLAG_CLEAR;
    result = rt_event_recv(
        (rt_event_t)event->opaque[0],
        bits,
        option,
        timeout_ticks(timeout_ms),
        &recved
    );
    if (result != RT_EOK)
        return -1;
    if (observed_bits != NULL)
        *observed_bits = recved;
    return 0;
}

static int rtt_event_set(
    void *context, wlh_osal_event_t *event, uint32_t bits
) {
    (void)context;
    if (event == NULL || event->opaque[0] == 0u || bits == 0u ||
        (bits & ~WLH_OSAL_EVENT_BITS_MASK) != 0u)
        return -1;
    return rt_event_send((rt_event_t)event->opaque[0], bits) == RT_EOK ? 0 : -1;
}

static int rtt_event_set_from_isr(
    void *context,
    wlh_osal_event_t *event,
    uint32_t bits,
    bool *higher_priority_task_woken
) {
    int result;
    (void)context;
    if (event == NULL || event->opaque[0] == 0u || bits == 0u ||
        (bits & ~WLH_OSAL_EVENT_BITS_MASK) != 0u)
        return -1;
    result =
        rt_event_send((rt_event_t)event->opaque[0], bits) == RT_EOK ? 0 : -1;
    if (higher_priority_task_woken != NULL)
        *higher_priority_task_woken = false;
    return result;
}

static int rtt_queue_create(
    void *context,
    wlh_osal_queue_t *queue,
    void *storage,
    size_t item_size,
    size_t capacity
) {
    rtt_osal_queue_state_t *state;
    (void)context;
    (void)storage;
    if (queue == NULL || item_size == 0u || capacity == 0u)
        return -1;
    state = calloc(1u, sizeof(*state));
    if (state == NULL)
        return -1;
    /* RT-Thread message queues cannot be built on an exact item_size *
     * capacity pool, so the pool is allocated internally; `storage` is
     * ignored (see wlh/rtt_osal.h). */
    state->handle = rt_mq_create(
        "wlh", (rt_size_t)item_size, (rt_size_t)capacity, RT_IPC_FLAG_FIFO
    );
    if (state->handle == NULL) {
        free(state);
        return -1;
    }
    state->item_size = item_size;
    memset(queue, 0, sizeof(*queue));
    queue->opaque[0] = (uintptr_t)state;
    return 0;
}

static void rtt_queue_destroy(void *context, wlh_osal_queue_t *queue) {
    rtt_osal_queue_state_t *state;
    (void)context;
    if (queue == NULL || queue->opaque[0] == 0u)
        return;
    state = (rtt_osal_queue_state_t *)queue->opaque[0];
    rt_mq_delete(state->handle);
    free(state);
    queue->opaque[0] = 0u;
}

static int rtt_queue_send(
    void *context,
    wlh_osal_queue_t *queue,
    const void *item,
    uint32_t timeout_ms
) {
    rtt_osal_queue_state_t *state;
    (void)context;
    if (queue == NULL || queue->opaque[0] == 0u)
        return -1;
    state = (rtt_osal_queue_state_t *)queue->opaque[0];
    return rt_mq_send_wait(
               state->handle,
               item,
               (rt_size_t)state->item_size,
               timeout_ticks(timeout_ms)
           ) == RT_EOK
               ? 0
               : -1;
}

static int rtt_queue_send_from_isr(
    void *context,
    wlh_osal_queue_t *queue,
    const void *item,
    bool *higher_priority_task_woken
) {
    rtt_osal_queue_state_t *state;
    int result;
    (void)context;
    if (queue == NULL || queue->opaque[0] == 0u)
        return -1;
    state = (rtt_osal_queue_state_t *)queue->opaque[0];
    /* rt_mq_send is non-blocking and safe in interrupt context. */
    result =
        rt_mq_send(state->handle, item, (rt_size_t)state->item_size) == RT_EOK
            ? 0
            : -1;
    if (higher_priority_task_woken != NULL)
        *higher_priority_task_woken = false;
    return result;
}

static int rtt_queue_receive(
    void *context, wlh_osal_queue_t *queue, void *item, uint32_t timeout_ms
) {
    rtt_osal_queue_state_t *state;
    rt_ssize_t result;
    (void)context;
    if (queue == NULL || queue->opaque[0] == 0u)
        return -1;
    state = (rtt_osal_queue_state_t *)queue->opaque[0];
    result = rt_mq_recv(
        state->handle,
        item,
        (rt_size_t)state->item_size,
        timeout_ticks(timeout_ms)
    );
    return result >= 0 ? 0 : -1;
}

static void timer_trampoline(void *parameter) {
    rtt_osal_timer_state_t *state = parameter;
    state->callback(state->argument);
}

static void timer_barrier_trampoline(void *parameter) {
    (void)rt_sem_release((rt_sem_t)parameter);
}

/* Soft-timer callbacks are serialized by RT-Thread's timer thread. Waiting
 * for a one-tick barrier therefore also waits for any callback which was
 * already running when the target timer was stopped. */
static void wait_for_timer_thread(void) {
    struct rt_semaphore done;
    struct rt_timer barrier;

    (void)rt_sem_init(&done, "wlhbar", 0u, RT_IPC_FLAG_FIFO);
    rt_timer_init(
        &barrier,
        "wlhbar",
        timer_barrier_trampoline,
        &done,
        1u,
        RT_TIMER_FLAG_SOFT_TIMER | RT_TIMER_FLAG_ONE_SHOT
    );
    if (rt_timer_start(&barrier) == RT_EOK)
        (void)rt_sem_take(&done, RT_WAITING_FOREVER);
    (void)rt_timer_detach(&barrier);
    (void)rt_sem_detach(&done);
}

static int rtt_timer_create(
    void *context,
    wlh_osal_timer_t *timer,
    wlh_osal_timer_fn callback,
    void *argument
) {
    rtt_osal_timer_state_t *state;
    (void)context;
    if (timer == NULL || callback == NULL)
        return -1;
    state = calloc(1u, sizeof(*state));
    if (state == NULL)
        return -1;
    state->callback = callback;
    state->argument = argument;
    state->handle = rt_timer_create(
        "wlh",
        timer_trampoline,
        state,
        1u,
        RT_TIMER_FLAG_SOFT_TIMER | RT_TIMER_FLAG_ONE_SHOT
    );
    if (state->handle == NULL) {
        free(state);
        return -1;
    }
    memset(timer, 0, sizeof(*timer));
    timer->opaque[0] = (uintptr_t)state;
    return 0;
}

static void rtt_timer_destroy(void *context, wlh_osal_timer_t *timer) {
    rtt_osal_timer_state_t *state;
    (void)context;
    if (timer == NULL || timer->opaque[0] == 0u)
        return;
    state = (rtt_osal_timer_state_t *)timer->opaque[0];
    (void)rt_timer_stop(state->handle);
    wait_for_timer_thread();
    (void)rt_timer_delete(state->handle);
    free(state);
    timer->opaque[0] = 0u;
}

static int rtt_timer_start(
    void *context, wlh_osal_timer_t *timer, uint32_t period_ms, bool periodic
) {
    rtt_osal_timer_state_t *state;
    rt_tick_t ticks;
    rt_err_t result;
    (void)context;
    if (timer == NULL || timer->opaque[0] == 0u || period_ms == 0u)
        return -1;
    state = (rtt_osal_timer_state_t *)timer->opaque[0];
    if (period_ms >= UINT32_C(0x80000000))
        return -1;
    ticks = (rt_tick_t)timeout_ticks(period_ms);
    if (ticks == 0u)
        ticks = 1u;
    (void)rt_timer_stop(state->handle);
    result = rt_timer_control(state->handle, RT_TIMER_CTRL_SET_TIME, &ticks);
    if (result != RT_EOK)
        return -1;
    result = rt_timer_control(
        state->handle,
        periodic ? RT_TIMER_CTRL_SET_PERIODIC : RT_TIMER_CTRL_SET_ONESHOT,
        NULL
    );
    if (result != RT_EOK)
        return -1;
    return rt_timer_start(state->handle) == RT_EOK ? 0 : -1;
}

static int rtt_timer_stop(void *context, wlh_osal_timer_t *timer) {
    rtt_osal_timer_state_t *state;
    (void)context;
    if (timer == NULL || timer->opaque[0] == 0u)
        return -1;
    state = (rtt_osal_timer_state_t *)timer->opaque[0];
    return rt_timer_stop(state->handle) == RT_EOK ? 0 : -1;
}

static uint64_t rtt_monotonic_time_ms(void *context) {
    (void)context;
    return (uint64_t)rt_tick_get() * 1000u / (uint64_t)RT_TICK_PER_SECOND;
}

static void rtt_sleep_ms(void *context, uint32_t duration_ms) {
    (void)context;
    rt_thread_mdelay((rt_int32_t)duration_ms);
}

static void rtt_yield(void *context) {
    (void)context;
    (void)rt_thread_yield();
}

static bool rtt_in_isr(void *context) {
    (void)context;
    return rt_interrupt_get_nest() != 0u;
}

void wlh_rtt_osal_init(wlh_rtt_osal_t *osal) {
    if (osal != NULL)
        memset(osal, 0, sizeof(*osal));
}

// clang-format off
wlh_osal_ops_t wlh_rtt_osal_ops(wlh_rtt_osal_t *osal) {
    wlh_osal_ops_t ops = {
        osal,
        rtt_task_create,
        rtt_task_join,
        rtt_mutex_create,
        rtt_mutex_destroy,
        rtt_mutex_lock,
        rtt_mutex_unlock,
        rtt_semaphore_create,
        rtt_semaphore_destroy,
        rtt_semaphore_take,
        rtt_semaphore_give,
        rtt_semaphore_give_from_isr,
        rtt_event_create,
        rtt_event_destroy,
        rtt_event_wait,
        rtt_event_set,
        rtt_event_set_from_isr,
        rtt_queue_create,
        rtt_queue_destroy,
        rtt_queue_send,
        rtt_queue_send_from_isr,
        rtt_queue_receive,
        rtt_timer_create,
        rtt_timer_destroy,
        rtt_timer_start,
        rtt_timer_stop,
        rtt_monotonic_time_ms,
        rtt_sleep_ms,
        rtt_yield,
        rtt_in_isr
    };
    return ops;
}
// clang-format on
