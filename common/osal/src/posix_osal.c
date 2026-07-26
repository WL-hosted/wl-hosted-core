/*
 * The build compiles with a strict -std=c11 (C_EXTENSIONS OFF), which defines
 * __STRICT_ANSI__ and suppresses glibc's _DEFAULT_SOURCE.  Without an explicit
 * feature macro, POSIX declarations this file relies on (clock_gettime,
 * CLOCK_MONOTONIC, nanosleep, pthread_condattr_setclock) stay hidden on Linux.
 *
 * Darwin is excluded deliberately: defining _POSIX_C_SOURCE there lowers
 * __DARWIN_C_LEVEL and hides the *_np extensions used below.
 */
#if !defined(__APPLE__) && !defined(_POSIX_C_SOURCE)
#define _POSIX_C_SOURCE 200809L
#endif

#include "wlh/posix_osal.h"

#include <errno.h>
#include <pthread.h>
#include <sched.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/*
 * Portable timed condition-variable helpers.
 *
 * pthread_cond_timedwait_relative_np() is an Apple/NetBSD extension and is
 * not available on glibc or musl Linux.  On those platforms we use
 * pthread_cond_timedwait() with an absolute CLOCK_MONOTONIC deadline instead,
 * which requires condition variables to be initialised with that clock.
 *
 * On Apple platforms we keep the _np path to avoid the macOS 10.15+
 * requirement imposed by pthread_condattr_setclock(CLOCK_MONOTONIC).
 */
#ifndef WLH_POSIX_USE_TIMEDWAIT_RELATIVE_NP
#if defined(__APPLE__) || defined(__NetBSD__)
#define WLH_POSIX_USE_TIMEDWAIT_RELATIVE_NP 1
#else
#define WLH_POSIX_USE_TIMEDWAIT_RELATIVE_NP 0
#endif
#endif

/* Initialise a condition variable backed by CLOCK_MONOTONIC where needed. */
static int cond_init_monotonic(pthread_cond_t *cond) {
#if WLH_POSIX_USE_TIMEDWAIT_RELATIVE_NP
    return pthread_cond_init(cond, NULL);
#else
    pthread_condattr_t attr;
    int rc;
    if (pthread_condattr_init(&attr) != 0)
        return -1;
    if (pthread_condattr_setclock(&attr, CLOCK_MONOTONIC) != 0) {
        pthread_condattr_destroy(&attr);
        return -1;
    }
    rc = pthread_cond_init(cond, &attr);
    pthread_condattr_destroy(&attr);
    return rc;
#endif
}

/* Timed wait using a relative duration in milliseconds.
 * On Apple/NetBSD: delegates to the relative_np extension.
 * Elsewhere:       computes an absolute CLOCK_MONOTONIC deadline. */
static int cond_timedwait_ms(
    pthread_cond_t *cond, pthread_mutex_t *mutex, uint32_t duration_ms
) {
#if WLH_POSIX_USE_TIMEDWAIT_RELATIVE_NP
    struct timespec rel;
    rel.tv_sec = (time_t)(duration_ms / 1000u);
    rel.tv_nsec = (long)(duration_ms % 1000u) * 1000000L;
    return pthread_cond_timedwait_relative_np(cond, mutex, &rel) == 0 ? 0 : -1;
#else
    struct timespec ts;
    uint64_t deadline_ms;
    struct timespec now;
    (void)clock_gettime(CLOCK_MONOTONIC, &now);
    deadline_ms = (uint64_t)now.tv_sec * 1000u +
                  (uint64_t)now.tv_nsec / 1000000u + duration_ms;
    ts.tv_sec = (time_t)(deadline_ms / 1000u);
    ts.tv_nsec = (long)(deadline_ms % 1000u) * 1000000L;
    return pthread_cond_timedwait(cond, mutex, &ts) == 0 ? 0 : -1;
#endif
}

typedef struct task_state {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    wlh_osal_task_fn entry;
    void *argument;
    bool done;
    bool created;
} task_state_t;

typedef struct mutex_state {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    bool locked;
} mutex_state_t;

typedef struct semaphore_state {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    uint32_t count;
    uint32_t maximum;
} semaphore_state_t;

typedef struct event_state {
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    uint32_t bits;
} event_state_t;

typedef struct queue_state {
    pthread_mutex_t mutex;
    pthread_cond_t not_empty;
    pthread_cond_t not_full;
    uint8_t *storage;
    size_t item_size;
    size_t capacity;
    size_t head;
    size_t count;
} queue_state_t;

typedef struct timer_state {
    pthread_t thread;
    pthread_mutex_t mutex;
    pthread_cond_t condition;
    wlh_osal_timer_fn callback;
    void *argument;
    uint64_t deadline_ms;
    uint32_t period_ms;
    bool periodic;
    bool armed;
    bool stopping;
    bool created;
} timer_state_t;

_Static_assert(
    sizeof(uintptr_t) <= sizeof(wlh_osal_task_t), "task pointer storage"
);
_Static_assert(
    sizeof(uintptr_t) <= sizeof(wlh_osal_mutex_t), "mutex pointer storage"
);
_Static_assert(
    sizeof(semaphore_state_t) <= sizeof(wlh_osal_semaphore_t),
    "semaphore opaque storage"
);
_Static_assert(
    sizeof(event_state_t) <= sizeof(wlh_osal_event_t), "event opaque storage"
);
_Static_assert(
    sizeof(queue_state_t) <= sizeof(wlh_osal_queue_t), "queue opaque storage"
);
_Static_assert(
    sizeof(timer_state_t) <= sizeof(wlh_osal_timer_t), "timer opaque storage"
);

static uint64_t clock_ms(void) {
    struct timespec value;
    (void)clock_gettime(CLOCK_MONOTONIC, &value);
    return (uint64_t)value.tv_sec * 1000u + (uint64_t)value.tv_nsec / 1000000u;
}

static void sleep_duration(uint32_t duration_ms) {
    struct timespec request = {
        (time_t)(duration_ms / 1000u), (long)(duration_ms % 1000u) * 1000000L
    };
    while (nanosleep(&request, &request) != 0 && errno == EINTR) {
    }
}

static int wait_condition(
    pthread_cond_t *condition, pthread_mutex_t *mutex, uint32_t timeout_ms
) {
    if (timeout_ms == WLH_OSAL_WAIT_FOREVER)
        return pthread_cond_wait(condition, mutex) == 0 ? 0 : -1;
    if (timeout_ms == WLH_OSAL_NO_WAIT)
        return -1;
    return cond_timedwait_ms(condition, mutex, timeout_ms);
}

static void *task_trampoline(void *argument) {
    task_state_t *state = argument;
    state->entry(state->argument);
    pthread_mutex_lock(&state->mutex);
    state->done = true;
    pthread_cond_broadcast(&state->condition);
    pthread_mutex_unlock(&state->mutex);
    return NULL;
}

static int os_task_create(
    void *context,
    wlh_osal_task_t *task,
    const wlh_osal_task_attributes_t *attributes,
    wlh_osal_task_fn entry,
    void *argument
) {
    task_state_t *state;
    pthread_attr_t native_attributes;
    int result;
    (void)context;
    if (task == NULL || entry == NULL)
        return -1;
    memset(task, 0, sizeof(*task));
    state = calloc(1u, sizeof(*state));
    if (state == NULL)
        return -1;
    if (pthread_mutex_init(&state->mutex, NULL) != 0) {
        free(state);
        return -1;
    }
    if (cond_init_monotonic(&state->condition) != 0) {
        pthread_mutex_destroy(&state->mutex);
        free(state);
        return -1;
    }
    state->entry = entry;
    state->argument = argument;
    if (pthread_attr_init(&native_attributes) != 0) {
        pthread_cond_destroy(&state->condition);
        pthread_mutex_destroy(&state->mutex);
        free(state);
        return -1;
    }
    if (attributes != NULL && attributes->stack_size != 0u &&
        pthread_attr_setstacksize(&native_attributes, attributes->stack_size) !=
            0) {
        pthread_attr_destroy(&native_attributes);
        pthread_cond_destroy(&state->condition);
        pthread_mutex_destroy(&state->mutex);
        free(state);
        return -1;
    }
    result = pthread_create(
        &state->thread, &native_attributes, task_trampoline, state
    );
    pthread_attr_destroy(&native_attributes);
    state->created = result == 0;
    if (result != 0) {
        pthread_cond_destroy(&state->condition);
        pthread_mutex_destroy(&state->mutex);
        free(state);
        return -1;
    }
    task->opaque[0] = (uintptr_t)state;
    return result == 0 ? 0 : -1;
}

static int os_task_join(
    void *context, wlh_osal_task_t *task, uint32_t timeout_ms
) {
    task_state_t *state = task == NULL ? NULL : (task_state_t *)task->opaque[0];
    int result = 0;
    (void)context;
    if (state == NULL || !state->created)
        return -1;
    pthread_mutex_lock(&state->mutex);
    if (timeout_ms == WLH_OSAL_WAIT_FOREVER) {
        while (!state->done && result == 0)
            result = pthread_cond_wait(&state->condition, &state->mutex);
    } else if (timeout_ms == WLH_OSAL_NO_WAIT) {
        if (!state->done)
            result = ETIMEDOUT;
    } else {
        uint64_t deadline = clock_ms() + timeout_ms;
        while (!state->done && result == 0) {
            uint64_t now = clock_ms();
            if (now >= deadline) {
                result = ETIMEDOUT;
                break;
            }
            result = cond_timedwait_ms(
                &state->condition, &state->mutex, (uint32_t)(deadline - now)
            );
        }
    }
    pthread_mutex_unlock(&state->mutex);
    if (result != 0 || pthread_join(state->thread, NULL) != 0)
        return -1;
    state->created = false;
    pthread_cond_destroy(&state->condition);
    pthread_mutex_destroy(&state->mutex);
    free(state);
    task->opaque[0] = 0u;
    return 0;
}

static int os_mutex_create(void *context, wlh_osal_mutex_t *mutex) {
    mutex_state_t *state;
    (void)context;
    if (mutex == NULL)
        return -1;
    memset(mutex, 0, sizeof(*mutex));
    state = calloc(1u, sizeof(*state));
    if (state == NULL)
        return -1;
    if (pthread_mutex_init(&state->mutex, NULL) != 0) {
        free(state);
        return -1;
    }
    if (cond_init_monotonic(&state->condition) != 0) {
        pthread_mutex_destroy(&state->mutex);
        free(state);
        return -1;
    }
    mutex->opaque[0] = (uintptr_t)state;
    return 0;
}
static void os_mutex_destroy(void *context, wlh_osal_mutex_t *mutex) {
    mutex_state_t *state =
        mutex == NULL ? NULL : (mutex_state_t *)mutex->opaque[0];
    (void)context;
    if (state == NULL)
        return;
    (void)pthread_cond_destroy(&state->condition);
    (void)pthread_mutex_destroy(&state->mutex);
    free(state);
    mutex->opaque[0] = 0u;
}
static int os_mutex_lock(
    void *context, wlh_osal_mutex_t *mutex, uint32_t timeout_ms
) {
    mutex_state_t *state =
        mutex == NULL ? NULL : (mutex_state_t *)mutex->opaque[0];
    int result = 0;
    (void)context;
    if (state == NULL || pthread_mutex_lock(&state->mutex) != 0)
        return -1;
    if (timeout_ms == WLH_OSAL_WAIT_FOREVER) {
        while (state->locked && result == 0)
            result = pthread_cond_wait(&state->condition, &state->mutex);
    } else if (timeout_ms == WLH_OSAL_NO_WAIT) {
        if (state->locked)
            result = ETIMEDOUT;
    } else {
        uint64_t deadline = clock_ms() + timeout_ms;
        while (state->locked && result == 0) {
            uint64_t now = clock_ms();
            if (now >= deadline) {
                result = ETIMEDOUT;
                break;
            }
            result = cond_timedwait_ms(
                &state->condition, &state->mutex, (uint32_t)(deadline - now)
            );
        }
    }
    if (result == 0)
        state->locked = true;
    pthread_mutex_unlock(&state->mutex);
    return result == 0 ? 0 : -1;
}
static void os_mutex_unlock(void *context, wlh_osal_mutex_t *mutex) {
    mutex_state_t *state =
        mutex == NULL ? NULL : (mutex_state_t *)mutex->opaque[0];
    (void)context;
    if (state == NULL || pthread_mutex_lock(&state->mutex) != 0)
        return;
    state->locked = false;
    pthread_cond_signal(&state->condition);
    pthread_mutex_unlock(&state->mutex);
}

static int os_semaphore_create(
    void *context,
    wlh_osal_semaphore_t *semaphore,
    uint32_t initial_count,
    uint32_t maximum_count
) {
    semaphore_state_t *state = (semaphore_state_t *)semaphore;
    (void)context;
    if (state == NULL || maximum_count == 0u || initial_count > maximum_count)
        return -1;
    memset(semaphore, 0, sizeof(*semaphore));
    if (pthread_mutex_init(&state->mutex, NULL) != 0)
        return -1;
    if (cond_init_monotonic(&state->condition) != 0) {
        pthread_mutex_destroy(&state->mutex);
        return -1;
    }
    state->count = initial_count;
    state->maximum = maximum_count;
    return 0;
}
static void os_semaphore_destroy(
    void *context, wlh_osal_semaphore_t *semaphore
) {
    semaphore_state_t *state = (semaphore_state_t *)semaphore;
    (void)context;
    if (state == NULL)
        return;
    (void)pthread_cond_destroy(&state->condition);
    (void)pthread_mutex_destroy(&state->mutex);
}
static int os_semaphore_take(
    void *context, wlh_osal_semaphore_t *semaphore, uint32_t timeout_ms
) {
    semaphore_state_t *state = (semaphore_state_t *)semaphore;
    int result = 0;
    (void)context;
    if (state == NULL || pthread_mutex_lock(&state->mutex) != 0)
        return -1;
    while (state->count == 0u) {
        if (wait_condition(&state->condition, &state->mutex, timeout_ms) != 0) {
            result = -1;
            break;
        }
    }
    if (result == 0)
        state->count--;
    pthread_mutex_unlock(&state->mutex);
    return result;
}
static int semaphore_give_common(semaphore_state_t *state) {
    int result = 0;
    if (state == NULL || pthread_mutex_lock(&state->mutex) != 0)
        return -1;
    if (state->count == state->maximum)
        result = -1;
    else {
        state->count++;
        pthread_cond_signal(&state->condition);
    }
    pthread_mutex_unlock(&state->mutex);
    return result;
}
static int os_semaphore_give(void *context, wlh_osal_semaphore_t *semaphore) {
    (void)context;
    return semaphore_give_common((semaphore_state_t *)semaphore);
}
static int os_semaphore_give_from_isr(
    void *context,
    wlh_osal_semaphore_t *semaphore,
    bool *higher_priority_task_woken
) {
    if (higher_priority_task_woken != NULL)
        *higher_priority_task_woken = true;
    return os_semaphore_give(context, semaphore);
}

static int os_event_create(void *context, wlh_osal_event_t *event) {
    event_state_t *state = (event_state_t *)event;
    (void)context;
    if (state == NULL)
        return -1;
    memset(event, 0, sizeof(*event));
    if (pthread_mutex_init(&state->mutex, NULL) != 0)
        return -1;
    if (cond_init_monotonic(&state->condition) != 0) {
        pthread_mutex_destroy(&state->mutex);
        return -1;
    }
    return 0;
}
static void os_event_destroy(void *context, wlh_osal_event_t *event) {
    event_state_t *state = (event_state_t *)event;
    (void)context;
    if (state == NULL)
        return;
    (void)pthread_cond_destroy(&state->condition);
    (void)pthread_mutex_destroy(&state->mutex);
}
static bool event_matches(uint32_t value, uint32_t bits, bool wait_all) {
    return wait_all ? (value & bits) == bits : (value & bits) != 0u;
}
static int os_event_wait(
    void *context,
    wlh_osal_event_t *event,
    uint32_t bits,
    bool wait_all,
    bool clear_on_exit,
    uint32_t timeout_ms,
    uint32_t *observed_bits
) {
    event_state_t *state = (event_state_t *)event;
    int result = 0;
    (void)context;
    if (state == NULL || bits == 0u || observed_bits == NULL ||
        pthread_mutex_lock(&state->mutex) != 0)
        return -1;
    while (!event_matches(state->bits, bits, wait_all)) {
        if (wait_condition(&state->condition, &state->mutex, timeout_ms) != 0) {
            result = -1;
            break;
        }
    }
    if (result == 0) {
        *observed_bits = state->bits & bits;
        if (clear_on_exit)
            state->bits &= ~bits;
    }
    pthread_mutex_unlock(&state->mutex);
    return result;
}
static int os_event_set(void *context, wlh_osal_event_t *event, uint32_t bits) {
    event_state_t *state = (event_state_t *)event;
    (void)context;
    if (state == NULL || pthread_mutex_lock(&state->mutex) != 0)
        return -1;
    state->bits |= bits;
    pthread_cond_broadcast(&state->condition);
    pthread_mutex_unlock(&state->mutex);
    return 0;
}
static int os_event_set_from_isr(
    void *context,
    wlh_osal_event_t *event,
    uint32_t bits,
    bool *higher_priority_task_woken
) {
    if (higher_priority_task_woken != NULL)
        *higher_priority_task_woken = true;
    return os_event_set(context, event, bits);
}

static int os_queue_create(
    void *context,
    wlh_osal_queue_t *queue,
    void *storage,
    size_t item_size,
    size_t capacity
) {
    queue_state_t *state = (queue_state_t *)queue;
    (void)context;
    if (state == NULL || storage == NULL || item_size == 0u || capacity == 0u)
        return -1;
    memset(queue, 0, sizeof(*queue));
    if (pthread_mutex_init(&state->mutex, NULL) != 0)
        return -1;
    if (cond_init_monotonic(&state->not_empty) != 0) {
        pthread_mutex_destroy(&state->mutex);
        return -1;
    }
    if (cond_init_monotonic(&state->not_full) != 0) {
        pthread_cond_destroy(&state->not_empty);
        pthread_mutex_destroy(&state->mutex);
        return -1;
    }
    state->storage = storage;
    state->item_size = item_size;
    state->capacity = capacity;
    return 0;
}
static void os_queue_destroy(void *context, wlh_osal_queue_t *queue) {
    queue_state_t *state = (queue_state_t *)queue;
    (void)context;
    if (state == NULL)
        return;
    (void)pthread_cond_destroy(&state->not_empty);
    (void)pthread_cond_destroy(&state->not_full);
    (void)pthread_mutex_destroy(&state->mutex);
}
static int os_queue_send(
    void *context,
    wlh_osal_queue_t *queue,
    const void *item,
    uint32_t timeout_ms
) {
    queue_state_t *state = (queue_state_t *)queue;
    size_t tail;
    (void)context;
    if (state == NULL || item == NULL || pthread_mutex_lock(&state->mutex) != 0)
        return -1;
    while (state->count == state->capacity) {
        if (wait_condition(&state->not_full, &state->mutex, timeout_ms) != 0) {
            pthread_mutex_unlock(&state->mutex);
            return -1;
        }
    }
    tail = (state->head + state->count) % state->capacity;
    memcpy(state->storage + tail * state->item_size, item, state->item_size);
    state->count++;
    pthread_cond_signal(&state->not_empty);
    pthread_mutex_unlock(&state->mutex);
    return 0;
}
static int os_queue_send_from_isr(
    void *context,
    wlh_osal_queue_t *queue,
    const void *item,
    bool *higher_priority_task_woken
) {
    if (higher_priority_task_woken != NULL)
        *higher_priority_task_woken = true;
    return os_queue_send(context, queue, item, WLH_OSAL_NO_WAIT);
}
static int os_queue_receive(
    void *context, wlh_osal_queue_t *queue, void *item, uint32_t timeout_ms
) {
    queue_state_t *state = (queue_state_t *)queue;
    (void)context;
    if (state == NULL || item == NULL || pthread_mutex_lock(&state->mutex) != 0)
        return -1;
    while (state->count == 0u) {
        if (wait_condition(&state->not_empty, &state->mutex, timeout_ms) != 0) {
            pthread_mutex_unlock(&state->mutex);
            return -1;
        }
    }
    memcpy(
        item, state->storage + state->head * state->item_size, state->item_size
    );
    state->head = (state->head + 1u) % state->capacity;
    state->count--;
    pthread_cond_signal(&state->not_full);
    pthread_mutex_unlock(&state->mutex);
    return 0;
}

static void *timer_main(void *argument) {
    timer_state_t *state = argument;
    pthread_mutex_lock(&state->mutex);
    while (!state->stopping) {
        uint64_t now;
        uint32_t wait_ms;
        while (!state->armed && !state->stopping)
            pthread_cond_wait(&state->condition, &state->mutex);
        if (state->stopping)
            break;
        now = clock_ms();
        if (now < state->deadline_ms) {
            wait_ms = (uint32_t)(state->deadline_ms - now);
            (void)cond_timedwait_ms(&state->condition, &state->mutex, wait_ms);
            continue;
        }
        if (state->periodic)
            state->deadline_ms = now + state->period_ms;
        else
            state->armed = false;
        pthread_mutex_unlock(&state->mutex);
        state->callback(state->argument);
        pthread_mutex_lock(&state->mutex);
    }
    pthread_mutex_unlock(&state->mutex);
    return NULL;
}
static int os_timer_create(
    void *context,
    wlh_osal_timer_t *timer,
    wlh_osal_timer_fn callback,
    void *argument
) {
    timer_state_t *state = (timer_state_t *)timer;
    (void)context;
    if (state == NULL || callback == NULL)
        return -1;
    memset(timer, 0, sizeof(*timer));
    if (pthread_mutex_init(&state->mutex, NULL) != 0)
        return -1;
    if (cond_init_monotonic(&state->condition) != 0) {
        pthread_mutex_destroy(&state->mutex);
        return -1;
    }
    state->callback = callback;
    state->argument = argument;
    if (pthread_create(&state->thread, NULL, timer_main, state) != 0) {
        pthread_cond_destroy(&state->condition);
        pthread_mutex_destroy(&state->mutex);
        return -1;
    }
    state->created = true;
    return 0;
}
static void os_timer_destroy(void *context, wlh_osal_timer_t *timer) {
    timer_state_t *state = (timer_state_t *)timer;
    (void)context;
    if (state == NULL || !state->created)
        return;
    pthread_mutex_lock(&state->mutex);
    state->stopping = true;
    pthread_cond_signal(&state->condition);
    pthread_mutex_unlock(&state->mutex);
    (void)pthread_join(state->thread, NULL);
    (void)pthread_cond_destroy(&state->condition);
    (void)pthread_mutex_destroy(&state->mutex);
    state->created = false;
}
static int os_timer_start(
    void *context, wlh_osal_timer_t *timer, uint32_t period_ms, bool periodic
) {
    timer_state_t *state = (timer_state_t *)timer;
    (void)context;
    if (state == NULL || !state->created || period_ms == 0u)
        return -1;
    pthread_mutex_lock(&state->mutex);
    state->period_ms = period_ms;
    state->periodic = periodic;
    state->deadline_ms = clock_ms() + period_ms;
    state->armed = true;
    pthread_cond_signal(&state->condition);
    pthread_mutex_unlock(&state->mutex);
    return 0;
}
static int os_timer_stop(void *context, wlh_osal_timer_t *timer) {
    timer_state_t *state = (timer_state_t *)timer;
    (void)context;
    if (state == NULL || !state->created)
        return -1;
    pthread_mutex_lock(&state->mutex);
    state->armed = false;
    pthread_cond_signal(&state->condition);
    pthread_mutex_unlock(&state->mutex);
    return 0;
}

static uint64_t os_monotonic(void *context) {
    (void)context;
    return clock_ms();
}
static void os_sleep(void *context, uint32_t duration_ms) {
    (void)context;
    sleep_duration(duration_ms);
}
static void os_yield(void *context) {
    (void)context;
    sched_yield();
}
static bool os_in_isr(void *context) {
    (void)context;
    return false;
}

void wlh_posix_osal_init(wlh_posix_osal_t *osal) {
    if (osal != NULL)
        memset(osal, 0, sizeof(*osal));
}

wlh_osal_ops_t wlh_posix_osal_ops(wlh_posix_osal_t *osal) {
    wlh_osal_ops_t ops = {
        osal,
        os_task_create,
        os_task_join,
        os_mutex_create,
        os_mutex_destroy,
        os_mutex_lock,
        os_mutex_unlock,
        os_semaphore_create,
        os_semaphore_destroy,
        os_semaphore_take,
        os_semaphore_give,
        os_semaphore_give_from_isr,
        os_event_create,
        os_event_destroy,
        os_event_wait,
        os_event_set,
        os_event_set_from_isr,
        os_queue_create,
        os_queue_destroy,
        os_queue_send,
        os_queue_send_from_isr,
        os_queue_receive,
        os_timer_create,
        os_timer_destroy,
        os_timer_start,
        os_timer_stop,
        os_monotonic,
        os_sleep,
        os_yield,
        os_in_isr
    };
    return ops;
}
