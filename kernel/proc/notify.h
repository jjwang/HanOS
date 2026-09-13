/**-----------------------------------------------------------------------------

 @file    notify.h
 @brief   Generic notification primitive built on IPC endpoints
 @details
 @verbatim

   A notify_t is an endpoint plus a pending-type mask. It is the mechanism that
   will replace the ad-hoc event bus once servers move out of the kernel: an
   interrupt handler or a driver publishes a typed notification and a sleeping
   task subscribes to it.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <base/spinlock.h>
#include <ipc/ipc.h>

typedef struct {
    endpoint_t *ep;
    uint64_t pending;
    spinlock_t lock;
} notify_t;

void notify_init(notify_t *n);
void notify_publish(notify_t *n, uint64_t type, uint64_t para);
bool notify_subscribe(notify_t *n, uint64_t type, uint64_t *para);
