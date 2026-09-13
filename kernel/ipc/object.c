/**-----------------------------------------------------------------------------

 @file    object.c
 @brief   Implementation of the kernel object and handle model

 **-----------------------------------------------------------------------------
 */
#include <libc/string.h>

#include <base/kmalloc.h>
#include <base/spinlock.h>
#include <base/klog.h>
#include <ipc/object.h>

#define HANDLE_INDEX_BITS   16
#define HANDLE_INDEX_MASK   0xFFFFu

void object_init(kernel_object_t *o, obj_type_t type, void *impl,
                 void (*destroy) (kernel_object_t *))
{
    o->type = type;
    o->refcnt = 1;
    o->impl = impl;
    o->destroy = destroy;
}

void object_ref(kernel_object_t *o)
{
    if (o != NULL)
        __atomic_add_fetch(&o->refcnt, 1, __ATOMIC_RELAXED);
}

void object_unref(kernel_object_t *o)
{
    if (o == NULL)
        return;

    if (__atomic_sub_fetch(&o->refcnt, 1, __ATOMIC_ACQ_REL) == 0
        && o->destroy != NULL) {
        o->destroy(o);
    }
}

void handle_table_init(handle_table_t *ht)
{
    ht->slots = NULL;
    ht->count = 0;
    ht->capacity = 0;
    spinlock_init(&ht->lock);
}

void handle_table_destroy(handle_table_t *ht)
{
    for (uint32_t i = 0; i < ht->count; i++) {
        if (ht->slots[i].obj != NULL) {
            object_unref(ht->slots[i].obj);
            ht->slots[i].obj = NULL;
        }
    }
    if (ht->slots != NULL) {
        kmfree(ht->slots);
        ht->slots = NULL;
    }
    ht->count = 0;
    ht->capacity = 0;
}

static bool handle_table_grow(handle_table_t *ht)
{
    uint32_t newcap = (ht->capacity == 0) ? 16 : ht->capacity * 2;
    handle_slot_t *ns = kmalloc(sizeof(handle_slot_t) * newcap);
    if (ns == NULL)
        return false;

    memset(ns, 0, sizeof(handle_slot_t) * newcap);
    if (ht->slots != NULL) {
        memcpy(ns, ht->slots, sizeof(handle_slot_t) * ht->count);
        kmfree(ht->slots);
    }
    ht->slots = ns;
    ht->capacity = newcap;
    return true;
}

handle_t handle_alloc(handle_table_t *ht, kernel_object_t *o, uint32_t rights)
{
    if (ht == NULL || o == NULL)
        return HANDLE_INVALID;

    spinlock_acquire(&ht->lock);

    uint32_t idx = ht->count;
    for (uint32_t i = 0; i < ht->count; i++) {
        if (ht->slots[i].obj == NULL) {
            idx = i;
            break;
        }
    }

    if (idx == ht->count) {
        if (ht->count == ht->capacity && !handle_table_grow(ht)) {
            spinlock_release(&ht->lock);
            return HANDLE_INVALID;
        }
        ht->count++;
    }

    handle_slot_t *slot = &ht->slots[idx];
    slot->generation++;
    if (slot->generation == 0)
        slot->generation = 1;
    slot->obj = o;
    slot->rights = rights;
    object_ref(o);

    handle_t h = ((handle_t) slot->generation << HANDLE_INDEX_BITS) | idx;

    spinlock_release(&ht->lock);
    return h;
}

kernel_object_t *handle_get(handle_table_t *ht, handle_t h, uint32_t rights)
{
    if (ht == NULL || h == HANDLE_INVALID)
        return NULL;

    uint32_t idx = (uint32_t) (h & HANDLE_INDEX_MASK);
    uint16_t gen = (uint16_t) (h >> HANDLE_INDEX_BITS);

    spinlock_acquire(&ht->lock);

    if (idx >= ht->count || ht->slots[idx].obj == NULL
        || ht->slots[idx].generation != gen
        || (ht->slots[idx].rights & rights) != rights) {
        spinlock_release(&ht->lock);
        return NULL;
    }

    kernel_object_t *o = ht->slots[idx].obj;
    spinlock_release(&ht->lock);
    return o;
}

int handle_close(handle_table_t *ht, handle_t h)
{
    if (ht == NULL || h == HANDLE_INVALID)
        return -1;

    uint32_t idx = (uint32_t) (h & HANDLE_INDEX_MASK);
    uint16_t gen = (uint16_t) (h >> HANDLE_INDEX_BITS);

    spinlock_acquire(&ht->lock);

    if (idx >= ht->count || ht->slots[idx].obj == NULL
        || ht->slots[idx].generation != gen) {
        spinlock_release(&ht->lock);
        return -1;
    }

    kernel_object_t *o = ht->slots[idx].obj;
    ht->slots[idx].obj = NULL;
    ht->slots[idx].rights = 0;

    spinlock_release(&ht->lock);

    object_unref(o);
    return 0;
}
