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

#include <base/kmalloc.h>
#include <base/klog.h>
#include <sys/mm.h>
#include <sys/panic.h>

size_t kmalloc_checkno = 0;

void *kmalloc_core(uint64_t size, const char *func, size_t line)
{
    memory_metadata_t *alloc = (memory_metadata_t*)
        PHYS_TO_VIRT(pmm_get(NUM_PAGES(size) + 1, 0x0, func, line));

    if (alloc == NULL) {
        kpanic("Out of memory when allocating %d bytes in %s:%d\n",
               size, func, line);
    }

    alloc->magic = MEM_MAGIC_NUM;
    alloc->checkno = kmalloc_checkno;
    alloc->numpages = NUM_PAGES(size);
    alloc->size = size;

    char *fn_tail = strncpy(alloc->filename, func, sizeof(alloc->filename) - 1);
    *fn_tail = '\0';

    alloc->lineno = line;

    return ((uint8_t*)alloc) + PAGE_SIZE;
}

void kmfree_core(void *addr, const char *func, size_t line)
{
    (void)func;

    memory_metadata_t *d =
        (memory_metadata_t*)((uint8_t*)addr - PAGE_SIZE);

    /* Only free when magic number is correct */
    if (d->magic == MEM_MAGIC_NUM) {
        pmm_free(VIRT_TO_PHYS(d), d->numpages + 1, func, line);
        d->magic = 0;
    }
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

        d->magic = MEM_MAGIC_NUM;
        /* Do not modify d->checkno */

        char *fn_tail = strncpy(d->filename, func, sizeof(d->filename) - 1); 
        *fn_tail = '\0';

        d->lineno = line;

        return addr;
    }

    void *new = kmalloc_core(newsize, func, line);
    memset(new, 0, newsize);
    if (d->size > newsize)
        memcpy(new, addr, newsize);
    else
        memcpy(new, addr, d->size);

    kmfree_core(addr, func, line);
    return new;
}

