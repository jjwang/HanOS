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

#include <base/spinlock.h>
#include <base/time.h>
#include <ipc/object.h>

#define IPC_WORDS       6
#define IPC_QUEUE_LEN   64

typedef struct {
    uint64_t tag;
    uint64_t words[IPC_WORDS];
    handle_t xfer[2];
    uint8_t xfer_count;
} ipc_msg_t;

typedef struct endpoint {
    kernel_object_t obj;        /* must stay first */
    spinlock_t lock;
    ipc_msg_t msgs[IPC_QUEUE_LEN];
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

/* The reply endpoint pointer of a call is carried in the last inline word. */
static inline endpoint_t *ipc_reply_ep(const ipc_msg_t *msg)
{
    return (endpoint_t *) msg->words[IPC_WORDS - 1];
}
