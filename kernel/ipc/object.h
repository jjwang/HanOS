/**-----------------------------------------------------------------------------

 @file    object.h
 @brief   Kernel object and per-task handle model
 @details
 @verbatim

   Every kernel object that can be referenced from user space (endpoints,
   memory objects, IRQs, ...) is wrapped in a kernel_object_t and only reachable
   through a handle stored in the owning task's handle table. A handle encodes
   the slot index and a generation so a stale handle fails after its slot is
   reused. This is the basis for capability passing in later phases.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <base/spinlock.h>

typedef enum {
    OBJ_ENDPOINT,
    OBJ_MEMORY,
    OBJ_IRQ,
    OBJ_TASK
} obj_type_t;

typedef struct kernel_object {
    obj_type_t type;
    volatile uint32_t refcnt;
    void *impl;                 /* endpoint_t*, memobj_t*, irq_t*, ... */
    void (*destroy) (struct kernel_object *);
} kernel_object_t;

typedef uint64_t handle_t;

#define HANDLE_INVALID          0

#define HANDLE_RIGHT_READ       (1u << 0)
#define HANDLE_RIGHT_WRITE      (1u << 1)
#define HANDLE_RIGHT_SEND       (1u << 2)
#define HANDLE_RIGHT_RECV       (1u << 3)
#define HANDLE_RIGHT_MAP        (1u << 4)
#define HANDLE_RIGHT_TRANSFER   (1u << 5)

typedef struct {
    kernel_object_t *obj;       /* NULL == free slot */
    uint32_t rights;
    uint16_t generation;
} handle_slot_t;

typedef struct handle_table {
    handle_slot_t *slots;
    uint32_t count;
    uint32_t capacity;
    spinlock_t lock;
} handle_table_t;

void object_init(kernel_object_t *o, obj_type_t type, void *impl,
                 void (*destroy) (kernel_object_t *));
void object_ref(kernel_object_t *o);
void object_unref(kernel_object_t *o);

void handle_table_init(handle_table_t *ht);
void handle_table_destroy(handle_table_t *ht);

handle_t handle_alloc(handle_table_t *ht, kernel_object_t *o,
                      uint32_t rights);
kernel_object_t *handle_get(handle_table_t *ht, handle_t h, uint32_t rights);
int handle_close(handle_table_t *ht, handle_t h);
