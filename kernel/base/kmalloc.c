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
#include <base/klib.h>
#include <sys/panic.h>
#include <mm/mm.h>
#include <mm/alloc.h>

#define SLAB_ALLOCATOR_USED         1

uint64_t kmalloc_checkno = 0;

void *kmalloc_core(uint64_t size, const char *func, uint64_t line)
{
#if SLAB_ALLOCATOR_USED != 0
    if (size >= ALLOC_MAX_SIZE) {
        klogd("kmalloc: %s:%d needs %d bytes memory (>= %d)\n",
              func, line, size, ALLOC_MAX_SIZE);
        return kmalloc_chunk(size, func, line);
    }

    void *buf = alloc(size);
    return buf;
#else
    return kmalloc_chunk(size, func, line);
#endif
}

void *kmalloc_chunk(uint64_t size, const char *func, uint64_t line)
{
    memory_metadata_t *alloc = (memory_metadata_t*)
        PHYS_TO_VIRT(pmm_get(NUM_PAGES(size) + 1, 0x0, func, line));

    if (alloc == NULL) {
        kpanic("Out of memory when allocating %d bytes in %s:%d\n",
               size, func, line);
    }

    /* TODO: for final release, this should be removed to improve speed */
    memset(alloc, size + PAGE_SIZE, 0);

    alloc->magic = MEM_MAGIC_NUM;
    alloc->checkno = kmalloc_checkno;
    alloc->numpages = NUM_PAGES(size);
    alloc->size = size;

    char *fn_tail = strncpy(alloc->filename, func, sizeof(alloc->filename) - 1);
    *fn_tail = '\0';

    alloc->lineno = line;

    uint64_t *buf = (uint64_t*)(((uint8_t*)alloc) + PAGE_SIZE);
    *(buf - 1) = size;

    return ((uint8_t*)alloc) + PAGE_SIZE;
}

void kmfree_core(void *addr, const char *func, uint64_t line)
{
#if SLAB_ALLOCATOR_USED != 0
    uint64_t *buf = (uint64_t*)addr;
    if (*(buf - 1) >= ALLOC_MAX_SIZE) {
        klogd("kmfree: %s:%d will free %d bytes memory (>= %d)\n",
              func, line, ALLOC_MAX_SIZE);
        return kmfree_chunk(addr, func, line);
    }
 
    free(addr);
#else
    return kmfree_chunk(addr, func, line);
#endif
}

void kmfree_chunk(void *addr, const char *func, uint64_t line)
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

void *kmrealloc_core(void *addr, uint64_t newsize, const char *func, uint64_t line)
{
    if (newsize >= ALLOC_MAX_SIZE) {
        klogd("kmalloc: realloc %d bytes (>= %d)\n",
              newsize, ALLOC_MAX_SIZE);
    }

#if SLAB_ALLOCATOR_USED != 0
    if (!addr)
        return kmalloc_core(newsize, func, line);

    uint64_t *buf = (uint64_t*)addr;
    void *newaddr = kmalloc_core(newsize, func, line);
    if (newaddr != NULL) {
        memcpy(newaddr, addr, MIN(*(buf - 1), newsize));
    }
    kmfree_core(addr, func, line);
    return newaddr;
#else
    return kmrealloc_chunk(addr, newsize, func, line);
#endif
}

void *kmrealloc_chunk(void *addr, uint64_t newsize, const char *func, uint64_t line)
{
    void *ret_addr = NULL;

    if (!addr) {
        ret_addr = kmalloc_chunk(newsize, func, line);
        goto normal_exit;
    }

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

        ret_addr = addr;
        goto normal_exit;
    }

    void *new = kmalloc_chunk(newsize, func, line);
    memset(new, 0, newsize);

    if (d->size > newsize)
        memcpy(new, addr, newsize);
    else
        memcpy(new, addr, d->size);

    kmfree_chunk(addr, func, line);
    ret_addr = new;

normal_exit:
    return ret_addr;
}

