/**-----------------------------------------------------------------------------

 @file    pmm.c
 @brief   Implementation of physical memory management functions
 @details
 @verbatim

  Memory management is a critical part of any operating system kernel.
  Providing a quick way for programs to allocate and free memory on a
  regular basis is a major responsibility of the kernel.

  PMM: The method behind PMM is very simple. The memories with type -
  STIVALE2_MMAP_USABLE are devided into 4K-size pages. A bitmap array is
  used for indicated whether it is free or not. One bit for one page in
  bitmap array.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>

#include <kconfig.h>

#include <libc/string.h>

#include <sys/cpu.h>
#include <sys/panic.h>
#include <mm/mm.h>
#include <base/klog.h>
#include <base/kmalloc.h>
#include <base/klib.h>
#include <base/vector.h>

mem_info_t kmem_info = {0};
static bool debug_info = false;

static void bitmap_markused(uint64_t addr, uint64_t numpages)
{
    for (uint64_t i = addr; i < addr + (numpages * PAGE_SIZE); i += PAGE_SIZE) { 
        kmem_info.bitmap[i / (PAGE_SIZE * BMP_PAGES_PER_BYTE)]
            &= ~((1 << ((i / PAGE_SIZE) % BMP_PAGES_PER_BYTE)));
    }
}

static bool bitmap_isfree(uint64_t addr, uint64_t numpages)
{
    bool free = true;
    
    for (uint64_t i = addr; i < addr + (numpages * PAGE_SIZE); i += PAGE_SIZE) {
        free = kmem_info.bitmap[i / (PAGE_SIZE * BMP_PAGES_PER_BYTE)]
            & (1 << ((i / PAGE_SIZE) % BMP_PAGES_PER_BYTE));
        if (!free)
            break;
    }
    return free;
}

void pmm_free(uint64_t addr, uint64_t numpages,
    const char *func, int64_t line)
{
    for (uint64_t i = addr; i < addr + (numpages * PAGE_SIZE); i += PAGE_SIZE) {
        if (!bitmap_isfree(i, 1))
            kmem_info.free_size += PAGE_SIZE;
        
        kmem_info.bitmap[i / (PAGE_SIZE * BMP_PAGES_PER_BYTE)]
            |= 1 << ((i / PAGE_SIZE) % BMP_PAGES_PER_BYTE);
    }
    /* The below log is for debugging memory leaks */
    if (numpages > 8 && debug_info) {
        klogi("pmm_free: %s(%d) free 0x%11x %d pages and available memory are "
              "%d bytes\n", func, line, addr, numpages, kmem_info.free_size);
    }
}

bool pmm_alloc(uint64_t addr, uint64_t numpages)
{
    if (!bitmap_isfree(addr, numpages))
        return false;

    bitmap_markused(addr, numpages);
    kmem_info.free_size -= numpages * PAGE_SIZE;
    return true;
}

uint64_t pmm_get(uint64_t numpages, uint64_t baseaddr, 
    const char *func, int64_t line)
{
    for (uint64_t i = baseaddr; i < kmem_info.phys_limit; i += PAGE_SIZE) {
        if (pmm_alloc(i, numpages)) {
            if (numpages > 8 && debug_info) {
                klogi("pmm_get: %s(%d) gets 0x%11x with %d pages from memory "
                      "%d bytes\n", func, line, i, numpages, kmem_info.free_size);
            }
            return i;
        }
    }

    kpanic("Out of Physical Memory");
    return 0;
}

void pmm_init(struct limine_memmap_response* map, uint64_t higher_half)
{
    if (higher_half != PHYS_TO_VIRT(0x0)) {
        kpanic("pmm_init: cannot handle high half region 0x%x\n", higher_half);
    }

    kmem_info.phys_limit = 0;
    kmem_info.total_size = 0;
    kmem_info.free_size = 0;

    klogv("Physical memory's entry number: %d\n", map->entry_count);

    for (uint64_t i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry* entry = map->entries[i];

        if (entry->type == LIMINE_MEMMAP_USABLE
            || entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE
            || entry->type == LIMINE_MEMMAP_KERNEL_AND_MODULES) {
            kmem_info.total_size += entry->length;
        }

        uint64_t new_limit = entry->base + entry->length;

        if (new_limit > kmem_info.phys_limit) {
            kmem_info.phys_limit = new_limit;
        } 
    }

    /* look for a good place to keep our bitmap */
    uint64_t bm_size = kmem_info.phys_limit / (PAGE_SIZE * BMP_PAGES_PER_BYTE);
    bool gotit = false;
    for (uint64_t i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry* entry = map->entries[i];

        if (entry->type != LIMINE_MEMMAP_USABLE) {
            continue;
        }

        if (entry->base + entry->length <= 0x100000)
            continue;

        if (entry->length >= bm_size) {
            if (!gotit) kmem_info.bitmap = (uint8_t*)PHYS_TO_VIRT(entry->base);
            gotit = true;
        }
    }

    memset(kmem_info.bitmap, 0, bm_size);
    klogi("Memory bitmap address: 0x%x, size: %d\n", kmem_info.bitmap, bm_size);

    /* now populate the bitmap */
    for (uint64_t i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry* entry = map->entries[i];
        if (entry->type == LIMINE_MEMMAP_USABLE) {
            pmm_free(entry->base, NUM_PAGES(entry->length), __func__, __LINE__);
        }
    }

    /* mark the bitmap as used */
    pmm_alloc(VIRT_TO_PHYS(kmem_info.bitmap), NUM_PAGES(bm_size));

    klogi("PMM initialization finished\n");   
    klogi("Memory total: %d, phys limit: %d (0x%x), free: %d, used: %d\n",
          kmem_info.total_size, kmem_info.phys_limit, kmem_info.phys_limit,
          kmem_info.free_size, kmem_info.total_size - kmem_info.free_size);
}

uint64_t pmm_get_total_memory(void)
{
    return kmem_info.total_size / (1024 * 1024);
}

void pmm_dump_usage(void)
{
    uint64_t t = kmem_info.total_size, f = kmem_info.free_size,
             u = t - f;

    kprintf("Physical memory usage:\n"
            "  Total: %8d KB (%4d MB)\n"
            "  Free : %8d KB (%4d MB)\n"
            "  Used : %8d KB (%4d MB)\n",
            t / 1024, t / (1024 * 1024),
            f / 1024, f / (1024 * 1024),
            u / 1024, u / (1024 * 1024));

#ifdef ENABLE_MEM_DEBUG
    kprintf("Checking #%d\n", kmalloc_checkno);
    int64_t np = MIN(NUM_PAGES(kmem_info.phys_limit), 1024 * 256);
    for (uint64_t addr = 0; addr < np * PAGE_SIZE; addr += PAGE_SIZE) {
        if (bitmap_isfree(addr, 1)) continue;
        memory_metadata_t *alloc = (memory_metadata_t*)PHYS_TO_VIRT(addr);
        if (alloc->magic == MEM_MAGIC_NUM) {
            if (alloc->checkno == kmalloc_checkno && kmalloc_checkno > 0) {
                kprintf("0x%x %s():%d %d bytes\n", alloc, alloc->filename,
                        alloc->lineno, alloc->size);
            }
        }
    }
    kmalloc_checkno++;
    kprintf("Update checking point to #%d for kmalloc()\n", kmalloc_checkno);
#endif
}

