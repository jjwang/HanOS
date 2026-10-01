/**-----------------------------------------------------------------------------

 @file    ipc.h
 @brief   Synchronous inter-process communication primitives
 @details
 @verbatim

   An endpoint is a kernel object holding a small message queue.
   ipc_send() enqueues a message and wakes a receiver; ipc_recv() blocks (with
   a timeout) until a message is available. ipc_call()/ipc_reply() build a
   request/reply protocol on top of an ephemeral reply endpoint.

   Messages carry a protocol tag, a few inline words, and up to two handles to
   be transferred to the receiver.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <lib/spinlock.h>
#include <lib/time.h>
#include <ipc/object.h>

#define IPC_WORDS       6
#define IPC_QUEUE_LEN   64

/**
 * @brief An IPC message: protocol tag, inline words and transferred handles
 */
typedef struct {
    uint64_t tag;
    uint64_t words[IPC_WORDS];
    handle_t xfer[2];
    uint8_t xfer_count;
} ipc_msg_t;

/**
 * @brief A queued message together with the objects whose handles it moves
 *
 * The references are owned by this entry until a receiver takes them.
 */
typedef struct {
    ipc_msg_t msg;
    kernel_object_t *xfer_obj[2];
    uint32_t xfer_rights[2];
    uint8_t xfer_count;
} ipc_queue_entry_t;

/**
 * @brief Kernel object holding a FIFO queue of IPC messages
 */
typedef struct {
    kernel_object_t obj;        /* must stay first */
    spinlock_t lock;
    ipc_queue_entry_t msgs[IPC_QUEUE_LEN];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
} endpoint_t;

endpoint_t *endpoint_create(void);
kernel_object_t *endpoint_object(endpoint_t *ep);

int ipc_send(endpoint_t *ep, const ipc_msg_t *msg);
int ipc_recv(endpoint_t *ep, ipc_msg_t *msg);
int ipc_recv_timeout(endpoint_t *ep, ipc_msg_t *msg, time_t timeout_ms);
int ipc_call(endpoint_t *ep, const ipc_msg_t *req, ipc_msg_t *rep);
int ipc_reply(endpoint_t *ep, const ipc_msg_t *rep);

/* Enqueue a message without waking the receiver. Used from contexts that hold
 * the run-queue lock and so cannot call sched_wake_key(); the receiver picks
 * the message up on its next receive, preserving FIFO order. */
int ipc_notify(endpoint_t *ep, const ipc_msg_t *msg);

/* Variants that move the objects staged on a message out to the caller (or, for
 * the receive side, hand them over). The caller owns the returned references;
 * the non-objs receive variants discard them. Used by the syscall layer to
 * implement handle transfer. */
int ipc_send_objs(endpoint_t *ep, const ipc_msg_t *msg,
                  kernel_object_t **objs, uint32_t *rights, uint8_t count);
int ipc_recv_objs(endpoint_t *ep, ipc_msg_t *msg,
                  kernel_object_t **objs, uint32_t *rights, uint8_t *count);
int ipc_recv_timeout_objs(endpoint_t *ep, ipc_msg_t *msg,
                          kernel_object_t **objs, uint32_t *rights,
                          uint8_t *count, time_t timeout_ms);

/* The reply endpoint pointer of a call is carried in the last inline word. */
static inline endpoint_t *ipc_reply_ep(const ipc_msg_t *msg)
{
    return (endpoint_t *) msg->words[IPC_WORDS - 1];
}
