/**-----------------------------------------------------------------------------

 @file    memobj.c
 @brief   Implementation of shareable memory objects
 @details
 @verbatim

  A memory object is a page-backed, refcounted unit shared between processes;
  it backs IPC bulk transfers and user mmap, and is destroyed when its last
  reference goes away.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <string.h>

#include <lib/kmalloc.h>
#include <lib/klog.h>
#include <mm/memobj.h>
#include <proc/syscall.h>

static void memobj_destroy(kernel_object_t *o)
{
    memobj_t *m = (memobj_t *) o;

    for (uint64_t i = 0; i < vec_length(&m->pages); i++)
        pmm_free(vec_at(&m->pages, i), 1, __func__, __LINE__);

    if (m->pages.data != NULL)
        kmfree(m->pages.data);

    kmfree(m);
}

memobj_t *memobj_create(uint64_t size)
{
    if (size == 0)
        return NULL;

    size = (size + PAGE_SIZE - 1) & ~(uint64_t) (PAGE_SIZE - 1);

    memobj_t *m = kmalloc(sizeof(memobj_t));
    if (m == NULL)
        return NULL;

    memset(m, 0, sizeof(*m));
    spinlock_init(&m->lock);
    object_init(&m->obj, OBJ_MEMORY, m, memobj_destroy);
    m->size = size;

    for (uint64_t off = 0; off < size; off += PAGE_SIZE) {
        uint64_t phys = pmm_get(1, 0x0, __func__, __LINE__);
        if (phys == 0) {
            object_unref(&m->obj);      /* destroy frees the pages so far */
            return NULL;
        }
        memset((void *) PHYS_TO_VIRT(phys), 0, PAGE_SIZE);
        vec_push_back(&m->pages, phys);
    }

    return m;
}

kernel_object_t *memobj_object(memobj_t * m)
{
    return &m->obj;
}

int memobj_map(memobj_t * m, addrspace_t * as, uint64_t vaddr, uint32_t prot)
{
    if (m == NULL || as == NULL || (vaddr & (PAGE_SIZE - 1)))
        return -1;

    uint64_t flags = VMM_FLAG_PRESENT | VMM_FLAG_USER;

    if (prot & PROT_WRITE)
        flags |= VMM_FLAG_READWRITE;

    for (uint64_t i = 0; i < vec_length(&m->pages); i++)
        vmm_map(as, vaddr + i * PAGE_SIZE, vec_at(&m->pages, i), 1, flags);

    return 0;
}

int memobj_unmap(memobj_t * m, addrspace_t * as, uint64_t vaddr)
{
    if (m == NULL || as == NULL || (vaddr & (PAGE_SIZE - 1)))
        return -1;

    vmm_unmap(as, vaddr, vec_length(&m->pages));
    return 0;
}

uint64_t memobj_size(memobj_t * m)
{
    return m->size;
}

uint64_t memobj_page_count(memobj_t * m)
{
    return vec_length(&m->pages);
}

uint64_t memobj_page(memobj_t * m, uint64_t index)
{
    if (m == NULL || index >= vec_length(&m->pages))
        return 0;

    return vec_at(&m->pages, index);
}

void memobj_unref(memobj_t * m)
{
    if (m != NULL)
        object_unref(&m->obj);
}
