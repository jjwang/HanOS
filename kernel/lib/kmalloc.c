#include <stddef.h>
#include <lib/kmalloc.h>
#include <lib/memutils.h>
#include <core/mm.h>

typedef struct {
    size_t numpages;
    size_t size;
} metadata_t;

void* kmalloc(uint64_t size)
{
    metadata_t* alloc = (metadata_t*)PHYS_TO_VIRT(pmm_get(NULL, NUM_PAGES(size) + 1));
    alloc->numpages = NUM_PAGES(size);
    alloc->size = size;
    return ((uint8_t*)alloc) + PAGE_SIZE;
}

void kmfree(void* addr)
{
    metadata_t* d = (metadata_t*)((uint8_t*)addr - PAGE_SIZE);
    pmm_free(NULL, VIRT_TO_PHYS(d), d->numpages + 1);
}

void* kmrealloc(void* addr, size_t newsize)
{
    if (!addr)
        return kmalloc(newsize);

    metadata_t* d = (metadata_t*)((uint8_t*)addr - PAGE_SIZE);
    if (NUM_PAGES(d->size) == NUM_PAGES(newsize)) {
        d->size = newsize;
        d->numpages = NUM_PAGES(newsize);
        return addr;
    }

    void* new = kmalloc(newsize);
    if (d->size > newsize)
        memcpy(new, addr, newsize);
    else
        memcpy(new, addr, d->size);

    kmfree(addr);
    return new;
}
