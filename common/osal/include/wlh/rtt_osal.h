#ifndef WLH_RTT_OSAL_H
#define WLH_RTT_OSAL_H

#include "wlh/osal.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * RT-Thread OSAL adapter for the WL-hosted portable OSAL contract. Objects keep
 * native handles behind the opaque OSAL storage; all waits are bounded except
 * the explicit WLH_OSAL_WAIT_FOREVER.
 *
 * Adapter semantics that differ from the native OSAL contract:
 * - Priorities follow the FreeRTOS/POSIX direction: a larger OSAL priority
 *   value means higher priority.  RT-Thread numbers priorities the other way
 *   round, so the adapter maps
 *   rt_prio = RT_THREAD_PRIORITY_MAX - 1 - osal_prio.  OSAL priorities
 *   outside [0, RT_THREAD_PRIORITY_MAX - 1] are rejected at task_create.
 * - queue_create() ignores the caller-provided `storage`: RT-Thread message
 *   queues cannot be built on an exact item_size * capacity pool, so the
 *   adapter allocates the message pool internally via rt_mq_create().
 * - semaphore_create() enforces maximum_count through the native
 *   RT_IPC_CMD_SET_VLIMIT semaphore limit (RT-Thread >= 5.0); release past
 *   the limit fails with -RT_EFULL.
 * - *_from_isr() always reports higher_priority_task_woken == false: RT-Thread
 *   runs the scheduler on ISR exit and exposes no API to query whether a
 *   higher-priority task was woken.
 * - Waits of >= 2^31 ms map to WAIT_FOREVER (rt_tick_from_millisecond() takes
 *   an int32 timeout); timer periods in that range are rejected.
 */

typedef struct wlh_rtt_osal {
    uint32_t reserved;
} wlh_rtt_osal_t;

void wlh_rtt_osal_init(wlh_rtt_osal_t *osal);
wlh_osal_ops_t wlh_rtt_osal_ops(wlh_rtt_osal_t *osal);

#ifdef __cplusplus
}
#endif
#endif
