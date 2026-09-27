/**-----------------------------------------------------------------------------

 @file    memobj.h
 @brief   Refcounted, shareable sets of physical pages
 @details
 @verbatim

   A memory object is the kernel primitive for bulk I/O: a server cannot read
   or write a client's buffer directly, so the kernel copies the payload into a
   memory object and passes its handle over IPC. The pages need not be
   physically contiguous; they are mapped with user permissions into whichever
   address space needs them.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <lib/spinlock.h>
#include <lib/vector.h>
#include <ipc/object.h>
#include <mm/mm.h>

typedef struct memobj {
    kernel_object_t obj;        /* must stay first */
    spinlock_t lock;
    uint64_t size;              /* bytes, page aligned */
    vec_struct(uint64_t) pages; /* physical page addresses */
} memobj_t;

/* Create a zero-filled object of the given size (rounded up to a page). */
memobj_t *memobj_create(uint64_t size);

kernel_object_t *memobj_object(memobj_t * m);

/* Map/unmap the whole object into an address space at a page-aligned vaddr. */
int memobj_map(memobj_t * m, addrspace_t * as, uint64_t vaddr, uint32_t prot);
int memobj_unmap(memobj_t * m, addrspace_t * as, uint64_t vaddr);

uint64_t memobj_size(memobj_t * m);
uint64_t memobj_page_count(memobj_t * m);

/* Physical address of one page (0 when the index is out of range). */
uint64_t memobj_page(memobj_t * m, uint64_t index);

void memobj_unref(memobj_t * m);
