/**-----------------------------------------------------------------------------

 @file    kmalloc.c
 @brief   Implementation of kernel memory allocation functions
 @details
 @verbatim

  Kernel memory allocation function includes malloc, free and realloc.

 @endverbatim
 @todo    Memory allocation should be improved for better efficiency.

 **-----------------------------------------------------------------------------
 */
#include <stddef.h>

#include <libc/string.h>

#include <lib/kmalloc.h>
#include <lib/klog.h>
#include <sys/mm.h>

void *kmalloc_core(uint64_t size, const char *func, size_t line)
{
    memory_metadata_t *alloc = (memory_metadata_t*)
        PHYS_TO_VIRT(pmm_get(NUM_PAGES(size) + 1, 0x0, func, line));

    alloc->numpages = NUM_PAGES(size);
    alloc->size = size;

    return ((uint8_t*)alloc) + PAGE_SIZE;
}

void kmfree_core(void *addr, const char *func, size_t line)
{
    (void)func;

    memory_metadata_t *d =
        (memory_metadata_t*)((uint8_t*)addr - PAGE_SIZE);

    pmm_free(VIRT_TO_PHYS(d), d->numpages + 1, func, line);
}

void* kmrealloc_core(void *addr, size_t newsize, const char *func, size_t line)
{
    if (!addr)
        return kmalloc_core(newsize, func, line);

    memory_metadata_t *d =
        (memory_metadata_t*)((uint8_t*)addr - PAGE_SIZE);

    if (NUM_PAGES(d->size) == NUM_PAGES(newsize)) {
        d->size = newsize;
        d->numpages = NUM_PAGES(newsize);
        return addr;
    }

    void *new = kmalloc_core(newsize, func, line);
    if (d->size > newsize)
        memcpy(new, addr, newsize);
    else
        memcpy(new, addr, d->size);

    kmfree_core(addr, func, line);
    return new;
}

