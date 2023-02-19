/**-----------------------------------------------------------------------------

 @file    kmalloc.c
 @brief   Implementation of memory allocation related functions
 @details
 @verbatim

  e.g., malloc, free and realloc.

 @endverbatim
 @todo    Memory allocation should be improved for better efficiency.

 **-----------------------------------------------------------------------------
 */
#include <stddef.h>
#include <lib/kmalloc.h>
#include <lib/klog.h>
#include <lib/memutils.h>
#include <sys/mm.h>

void *kmalloc(uint64_t size)
{
    memory_metadata_t *alloc =
        (memory_metadata_t*)PHYS_TO_VIRT(pmm_get(NUM_PAGES(size) + 1, 0x0));

    alloc->numpages = NUM_PAGES(size);
    alloc->size = size;

    return ((uint8_t*)alloc) + PAGE_SIZE;
}

void kmfree(void *addr)
{
    memory_metadata_t *d =
        (memory_metadata_t*)((uint8_t*)addr - PAGE_SIZE);

    pmm_free(VIRT_TO_PHYS(d), d->numpages + 1);
}

void* kmrealloc(void *addr, size_t newsize)
{
    if (!addr)
        return kmalloc(newsize);

    memory_metadata_t *d =
        (memory_metadata_t*)((uint8_t*)addr - PAGE_SIZE);

    if (NUM_PAGES(d->size) == NUM_PAGES(newsize)) {
        d->size = newsize;
        d->numpages = NUM_PAGES(newsize);
        return addr;
    }

    void *new = kmalloc(newsize);
    if (d->size > newsize)
        memcpy(new, addr, newsize);
    else
        memcpy(new, addr, d->size);

    kmfree(addr);
    return new;
}

