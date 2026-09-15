/**-----------------------------------------------------------------------------

 @file    irq.h
 @brief   IRQ objects and delivery to a userspace driver
 @details
 @verbatim

   An irq_obj_t represents one hardware interrupt line. A driver process binds it
   to an endpoint; when the interrupt fires, the kernel sends a short IPC
   message to that endpoint and the driver acknowledges it with irq_ack().
   Until a line is bound to an endpoint, the in-kernel handler runs as before.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <base/spinlock.h>
#include <ipc/object.h>
#include <ipc/ipc.h>

#define IRQ_TABLE_SIZE      32

/* Message tag used when an IRQ fires. */
#define IRQ_NOTIFY_TAG      0x01

typedef struct {
    kernel_object_t obj;        /* must stay first */
    uint32_t irq;
    endpoint_t *ep;
    spinlock_t lock;
    uint64_t count;
    bool bound;
} irq_obj_t;

irq_obj_t *irq_create(uint32_t irq);
kernel_object_t *irq_object(irq_obj_t *io);
void irq_bind(irq_obj_t *io, endpoint_t *ep);
void irq_ack(irq_obj_t *io);

/* Notify the endpoint bound to an IRQ line. Returns true when the line was
 * handled by a bound object, in which case the caller must still send EOI. */
bool irq_deliver(uint32_t irq);
