/**-----------------------------------------------------------------------------

 @file    irq.c
 @brief   Implementation of IRQ objects and delivery

 **-----------------------------------------------------------------------------
 */
#include <libc/string.h>

#include <base/kmalloc.h>
#include <base/klog.h>
#include <ipc/irq.h>

static irq_obj_t *irq_table[IRQ_TABLE_SIZE];
static spinlock_t irq_table_lock;

static void irq_destroy_object(kernel_object_t *o)
{
    irq_obj_t *io = (irq_obj_t *) o;

    spinlock_acquire(&irq_table_lock);
    if (io->irq < IRQ_TABLE_SIZE && irq_table[io->irq] == io)
        irq_table[io->irq] = NULL;
    spinlock_release(&irq_table_lock);

    kmfree(io);
}

irq_obj_t *irq_create(uint32_t irq)
{
    if (irq >= IRQ_TABLE_SIZE)
        return NULL;

    irq_obj_t *io = kmalloc(sizeof(irq_obj_t));
    if (io == NULL)
        return NULL;

    memset(io, 0, sizeof(irq_obj_t));
    io->irq = irq;
    spinlock_init(&io->lock);
    object_init(&io->obj, OBJ_IRQ, io, irq_destroy_object);

    spinlock_acquire(&irq_table_lock);
    irq_table[irq] = io;
    spinlock_release(&irq_table_lock);

    return io;
}

kernel_object_t *irq_object(irq_obj_t *io)
{
    return &io->obj;
}

void irq_bind(irq_obj_t *io, endpoint_t *ep)
{
    if (io == NULL)
        return;

    spinlock_acquire(&io->lock);
    io->ep = ep;
    io->bound = (ep != NULL);
    spinlock_release(&io->lock);
}

void irq_ack(irq_obj_t *io)
{
    if (io == NULL)
        return;

    spinlock_acquire(&io->lock);
    io->count++;
    spinlock_release(&io->lock);
}

bool irq_deliver(uint32_t irq)
{
    if (irq >= IRQ_TABLE_SIZE)
        return false;

    irq_obj_t *io = irq_table[irq];
    if (io == NULL)
        return false;

    spinlock_acquire(&io->lock);
    endpoint_t *ep = io->bound ? io->ep : NULL;
    spinlock_release(&io->lock);

    if (ep == NULL)
        return false;

    ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.tag = IRQ_NOTIFY_TAG;
    m.words[0] = irq;
    ipc_send(ep, &m);
    return true;
}
