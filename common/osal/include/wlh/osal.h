#ifndef WLH_OSAL_H
#define WLH_OSAL_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define WLH_OSAL_NO_WAIT 0u
#define WLH_OSAL_WAIT_FOREVER UINT32_MAX
/* FreeRTOS reserves the high byte of a 32-bit event group for control bits.
 * Keeping the portable contract to the low 24 bits lets every adapter expose
 * identical event semantics. */
#define WLH_OSAL_EVENT_BITS_MASK UINT32_C(0x00ffffff)

/*
 * OS objects are deliberately opaque.  Adapter implementations may place a
 * native object in this storage or store a pointer to statically provisioned
 * state, but native RTOS/POSIX handle types never cross the Core boundary.
 */
typedef struct wlh_osal_task {
    uintptr_t opaque[16];
} wlh_osal_task_t;
typedef struct wlh_osal_mutex {
    uintptr_t opaque[8];
} wlh_osal_mutex_t;
typedef struct wlh_osal_semaphore {
    uintptr_t opaque[16];
} wlh_osal_semaphore_t;
typedef struct wlh_osal_event {
    uintptr_t opaque[24];
} wlh_osal_event_t;
typedef struct wlh_osal_queue {
    uintptr_t opaque[32];
} wlh_osal_queue_t;
typedef struct wlh_osal_timer {
    uintptr_t opaque[32];
} wlh_osal_timer_t;

typedef void (*wlh_osal_task_fn)(void *argument);
typedef void (*wlh_osal_timer_fn)(void *argument);

typedef struct wlh_osal_task_attributes {
    const char *name;
    size_t stack_size;
    int32_t priority;
} wlh_osal_task_attributes_t;

typedef struct wlh_osal_ops {
    void *context;

    int (*task_create)(
        void *context,
        wlh_osal_task_t *task,
        const wlh_osal_task_attributes_t *attributes,
        wlh_osal_task_fn entry,
        void *argument
    );
    int (*task_join)(void *context, wlh_osal_task_t *task, uint32_t timeout_ms);

    int (*mutex_create)(void *context, wlh_osal_mutex_t *mutex);
    void (*mutex_destroy)(void *context, wlh_osal_mutex_t *mutex);
    int (*mutex_lock)(
        void *context, wlh_osal_mutex_t *mutex, uint32_t timeout_ms
    );
    void (*mutex_unlock)(void *context, wlh_osal_mutex_t *mutex);

    int (*semaphore_create)(
        void *context,
        wlh_osal_semaphore_t *semaphore,
        uint32_t initial_count,
        uint32_t maximum_count
    );
    void (*semaphore_destroy)(void *context, wlh_osal_semaphore_t *semaphore);
    int (*semaphore_take)(
        void *context, wlh_osal_semaphore_t *semaphore, uint32_t timeout_ms
    );
    int (*semaphore_give)(void *context, wlh_osal_semaphore_t *semaphore);
    int (*semaphore_give_from_isr)(
        void *context,
        wlh_osal_semaphore_t *semaphore,
        bool *higher_priority_task_woken
    );

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

    int (*queue_create)(
        void *context,
        wlh_osal_queue_t *queue,
        void *storage,
        size_t item_size,
        size_t capacity
    );
    void (*queue_destroy)(void *context, wlh_osal_queue_t *queue);
    int (*queue_send)(
        void *context,
        wlh_osal_queue_t *queue,
        const void *item,
        uint32_t timeout_ms
    );
    int (*queue_send_from_isr)(
        void *context,
        wlh_osal_queue_t *queue,
        const void *item,
        bool *higher_priority_task_woken
    );
    int (*queue_receive)(
        void *context, wlh_osal_queue_t *queue, void *item, uint32_t timeout_ms
    );

    int (*timer_create)(
        void *context,
        wlh_osal_timer_t *timer,
        wlh_osal_timer_fn callback,
        void *argument
    );
    void (*timer_destroy)(void *context, wlh_osal_timer_t *timer);
    int (*timer_start)(
        void *context,
        wlh_osal_timer_t *timer,
        uint32_t period_ms,
        bool periodic
    );
    int (*timer_stop)(void *context, wlh_osal_timer_t *timer);

    uint64_t (*monotonic_time_ms)(void *context);
    void (*sleep_ms)(void *context, uint32_t duration_ms);
    void (*yield)(void *context);
    bool (*in_isr)(void *context);
} wlh_osal_ops_t;

static inline bool wlh_osal_ops_valid(const wlh_osal_ops_t *ops) {
    return ops != NULL && ops->task_create != NULL && ops->task_join != NULL &&
           ops->mutex_create != NULL && ops->mutex_destroy != NULL &&
           ops->mutex_lock != NULL && ops->mutex_unlock != NULL &&
           ops->semaphore_create != NULL && ops->semaphore_destroy != NULL &&
           ops->semaphore_take != NULL && ops->semaphore_give != NULL &&
           ops->semaphore_give_from_isr != NULL && ops->event_create != NULL &&
           ops->event_destroy != NULL && ops->event_wait != NULL &&
           ops->event_set != NULL && ops->event_set_from_isr != NULL &&
           ops->queue_create != NULL && ops->queue_destroy != NULL &&
           ops->queue_send != NULL && ops->queue_send_from_isr != NULL &&
           ops->queue_receive != NULL && ops->timer_create != NULL &&
           ops->timer_destroy != NULL && ops->timer_start != NULL &&
           ops->timer_stop != NULL && ops->monotonic_time_ms != NULL &&
           ops->sleep_ms != NULL && ops->yield != NULL && ops->in_isr != NULL;
}

#ifdef __cplusplus
}
#endif

#endif
