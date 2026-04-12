/**-----------------------------------------------------------------------------

 @file    buddy.c
 @brief   Buddy allocator implementation for physical memory management
 @details
 @verbatim

  The buddy allocator is a memory allocation algorithm that divides memory
  into partitions to try to satisfy a memory request as suitably as possible.
  This system makes use of splitting memory into halves to give a best-fit.

  Key features:
  - O(log n) allocation and deallocation
  - Reduces external fragmentation
  - Efficient coalescing of free blocks
  - Power-of-2 block sizes

  Algorithm:
  - Memory is divided into blocks of size 2^k pages
  - When allocating, find smallest block >= requested size
  - Split larger blocks if needed
  - When freeing, coalesce with buddy if both are free

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <stdbool.h>

#include <kconfig.h>
#include <libc/string.h>
#include <sys/cpu.h>
#include <sys/panic.h>
#include <mm/mm.h>
#include <mm/pmm_ops.h>
#include <base/klog.h>
#include <base/klib.h>
#include <base/spinlock.h>

/* Maximum order (2^MAX_ORDER pages per block) */
#define MAX_ORDER           10
#define MIN_ORDER           0

/* Free list node structure */
typedef struct buddy_block {
    struct buddy_block *next;
    struct buddy_block *prev;
} buddy_block_t;

/* Buddy allocator state */
typedef struct {
    buddy_block_t *free_lists[MAX_ORDER + 1];
    uint64_t phys_limit;
    uint64_t total_size;
    uint64_t free_size;
    lock_t lock;
    uint8_t *block_order;  /* Track order of each block */
    uint64_t num_blocks;
} buddy_state_t;

static buddy_state_t buddy_state = { 0 };

/* Forward declarations */
void buddy_init(struct limine_memmap_response *map, uint64_t higher_half);
uint64_t buddy_get(uint64_t numpages, uint64_t baseaddr,
                   const char *func, int64_t line);
bool buddy_alloc(uint64_t addr, uint64_t numpages);
void buddy_free(uint64_t addr, uint64_t numpages,
                const char *func, int64_t line);
uint64_t buddy_get_total_memory(void);
void buddy_dump_usage(void);

/* Helper functions */
static inline uint64_t block_to_addr(uint64_t block_idx)
{
    return block_idx * PAGE_SIZE;
}

static inline uint64_t addr_to_block(uint64_t addr)
{
    return addr / PAGE_SIZE;
}

static inline uint64_t buddy_index(uint64_t block_idx, uint64_t order)
{
    return block_idx ^ (1ULL << order);
}

static inline uint64_t parent_index(uint64_t block_idx, uint64_t order)
{
    return block_idx & ~(1ULL << order);
}

static inline uint64_t order_size(uint64_t order)
{
    return 1ULL << order;
}

/* Find the order needed for numpages */
static uint64_t find_order(uint64_t numpages)
{
    uint64_t order = 0;
    uint64_t size = 1;

    while (size < numpages && order < MAX_ORDER) {
        size <<= 1;
        order++;
    }

    return order;
}

/* Add block to free list */
static void add_to_free_list(uint64_t block_idx, uint64_t order)
{
    buddy_block_t *block = (buddy_block_t *)PHYS_TO_VIRT(block_to_addr(block_idx));

    block->next = buddy_state.free_lists[order];
    block->prev = NULL;

    if (buddy_state.free_lists[order]) {
        buddy_state.free_lists[order]->prev = block;
    }

    buddy_state.free_lists[order] = block;
    buddy_state.block_order[block_idx] = order;
}

/* Remove block from free list */
static void remove_from_free_list(uint64_t block_idx, uint64_t order)
{
    buddy_block_t *block = (buddy_block_t *)PHYS_TO_VIRT(block_to_addr(block_idx));

    if (block->prev) {
        block->prev->next = block->next;
    } else {
        buddy_state.free_lists[order] = block->next;
    }

    if (block->next) {
        block->next->prev = block->prev;
    }

    buddy_state.block_order[block_idx] = 0xFF;  /* Mark as allocated */
}

/* Split a block into two buddies */
static void split_block(uint64_t block_idx, uint64_t order)
{
    if (order == 0) return;

    uint64_t buddy_idx = block_idx + order_size(order - 1);
    add_to_free_list(buddy_idx, order - 1);
    add_to_free_list(block_idx, order - 1);
}

/* Try to coalesce block with its buddy */
static uint64_t coalesce(uint64_t block_idx, uint64_t order)
{
    if (order >= MAX_ORDER) return block_idx;

    uint64_t buddy_idx = buddy_index(block_idx, order);

    /* Check if buddy is free and same order */
    if (buddy_idx >= buddy_state.num_blocks) return block_idx;
    if (buddy_state.block_order[buddy_idx] != order) return block_idx;

    /* Remove both blocks from free list */
    remove_from_free_list(block_idx, order);
    remove_from_free_list(buddy_idx, order);

    /* Get parent block index */
    uint64_t parent_idx = parent_index(block_idx, order);

    /* Recursively coalesce */
    return coalesce(parent_idx, order + 1);
}

/* Allocate a block of given order */
static uint64_t buddy_alloc_order(uint64_t order)
{
    if (order > MAX_ORDER) return 0;

    /* Find free block of this order */
    if (buddy_state.free_lists[order]) {
        buddy_block_t *block = buddy_state.free_lists[order];
        uint64_t block_idx = addr_to_block(VIRT_TO_PHYS((uint64_t)block));

        remove_from_free_list(block_idx, order);
        buddy_state.free_size -= order_size(order) * PAGE_SIZE;

        return block_idx;
    }

    /* No free block, try larger order */
    uint64_t block_idx = buddy_alloc_order(order + 1);
    if (block_idx == 0) return 0;

    /* Split the larger block */
    split_block(block_idx, order + 1);

    /* Allocate first half */
    remove_from_free_list(block_idx, order);
    buddy_state.free_size -= order_size(order) * PAGE_SIZE;

    return block_idx;
}

/* Buddy allocator interface implementation */

void buddy_init(struct limine_memmap_response *map, uint64_t higher_half)
{
    if (higher_half != PHYS_TO_VIRT(0x0)) {
        kpanic("buddy_init: cannot handle high half region 0x%016lx\n",
               higher_half);
    }

    buddy_state.phys_limit = 0;
    buddy_state.total_size = 0;
    buddy_state.free_size = 0;
    buddy_state.lock = lock_new();

    /* Initialize free lists */
    for (int i = 0; i <= MAX_ORDER; i++) {
        buddy_state.free_lists[i] = NULL;
    }

    klogv("Physical memory's entry number: %ld\n", map->entry_count);

    /* Calculate physical memory limit */
    for (uint64_t i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry *entry = map->entries[i];

        if (entry->type == LIMINE_MEMMAP_USABLE
            || entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE
            || entry->type == LIMINE_MEMMAP_KERNEL_AND_MODULES) {
            buddy_state.total_size += entry->length;
        }

        uint64_t new_limit = entry->base + entry->length;
        if (new_limit > buddy_state.phys_limit) {
            buddy_state.phys_limit = new_limit;
        }
    }

    /* Limit to MAX_MEM_USABLE_SIZE */
    if (buddy_state.phys_limit > MAX_MEM_USABLE_SIZE) {
        buddy_state.phys_limit = MAX_MEM_USABLE_SIZE;
    }

    buddy_state.num_blocks = buddy_state.phys_limit / PAGE_SIZE;

    /* Allocate block order tracking array */
    uint64_t order_array_size = buddy_state.num_blocks;
    bool gotit = false;

    for (uint64_t i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry *entry = map->entries[i];

        if (entry->type != LIMINE_MEMMAP_USABLE) continue;
        if (entry->base + entry->length <= 0x100000) continue;

        if (entry->length >= order_array_size) {
            if (!gotit) {
                buddy_state.block_order = (uint8_t *)PHYS_TO_VIRT(entry->base);
            }
            gotit = true;
        }
    }

    if (!gotit) {
        kpanic("buddy_init: cannot find space for block order array\n");
    }

    memset(buddy_state.block_order, 0xFF, order_array_size);
    klogi("Buddy block order array: 0x%016lx, size: %ld\n",
          buddy_state.block_order, order_array_size);

    /* Add free memory regions to buddy system */
    for (uint64_t i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry *entry = map->entries[i];

        if (entry->type != LIMINE_MEMMAP_USABLE) continue;
        if (entry->base >= MAX_MEM_USABLE_SIZE) continue;

        uint64_t start = entry->base;
        uint64_t end = entry->base + entry->length;

        if (end > MAX_MEM_USABLE_SIZE) {
            end = MAX_MEM_USABLE_SIZE;
        }

        /* Skip if this is where we put the order array */
        uint64_t order_array_phys = VIRT_TO_PHYS((uint64_t)buddy_state.block_order);
        if (start <= order_array_phys && end > order_array_phys) {
            /* Split around the order array */
            if (start < order_array_phys) {
                buddy_free(start, NUM_PAGES(order_array_phys - start),
                          __func__, __LINE__);
            }
            uint64_t after = order_array_phys + order_array_size;
            if (after < end) {
                buddy_free(after, NUM_PAGES(end - after),
                          __func__, __LINE__);
            }
        } else {
            buddy_free(start, NUM_PAGES(end - start), __func__, __LINE__);
        }
    }

    klogi("Buddy allocator initialization finished\n");
    klogi("Memory total: %ld, phys limit: %ld (0x%016lx), free: %ld, used: %ld\n",
          buddy_state.total_size, buddy_state.phys_limit,
          buddy_state.phys_limit, buddy_state.free_size,
          buddy_state.total_size - buddy_state.free_size);
}

uint64_t buddy_get(uint64_t numpages, uint64_t baseaddr,
                   const char *func, int64_t line)
{
    (void)baseaddr;  /* Buddy allocator doesn't support baseaddr */
    (void)func;
    (void)line;

    if (numpages == 0) return 0;

    uint64_t order = find_order(numpages);
    if (order > MAX_ORDER) {
        klogw("buddy_get: request too large (%ld pages, order %ld)\n",
              numpages, order);
        return 0;
    }

    lock_lock(&buddy_state.lock);
    uint64_t block_idx = buddy_alloc_order(order);
    lock_release(&buddy_state.lock);

    if (block_idx == 0) {
        kpanic("Out of Physical Memory");
        return 0;
    }

    return block_to_addr(block_idx);
}

bool buddy_alloc(uint64_t addr, uint64_t numpages)
{
    /* Not implemented for buddy allocator */
    /* This would require marking specific blocks as allocated */
    (void)addr;
    (void)numpages;
    return false;
}

void buddy_free(uint64_t addr, uint64_t numpages,
                const char *func, int64_t line)
{
    (void)func;
    (void)line;

    if (numpages == 0) return;

    uint64_t order = find_order(numpages);
    if (order > MAX_ORDER) {
        klogw("buddy_free: block too large (%ld pages)\n", numpages);
        return;
    }

    uint64_t block_idx = addr_to_block(addr);

    lock_lock(&buddy_state.lock);

    /* Add to free list */
    add_to_free_list(block_idx, order);
    buddy_state.free_size += order_size(order) * PAGE_SIZE;

    /* Try to coalesce */
    uint64_t coalesced_idx = coalesce(block_idx, order);
    if (coalesced_idx != block_idx) {
        /* Coalescing happened, add coalesced block */
        uint64_t new_order = order;
        while (coalesced_idx != block_idx && new_order < MAX_ORDER) {
            new_order++;
            block_idx = coalesced_idx;
            coalesced_idx = coalesce(block_idx, new_order);
        }
        add_to_free_list(coalesced_idx, new_order);
    }

    lock_release(&buddy_state.lock);
}

uint64_t buddy_get_total_memory(void)
{
    return buddy_state.total_size / (1024 * 1024);
}

void buddy_dump_usage(void)
{
    uint64_t t = buddy_state.total_size;
    uint64_t f = buddy_state.free_size;
    uint64_t u = t - f;

    kprintf("Physical memory usage (Buddy Allocator):\n"
            "  Total: %8d KB (%4d MB)\n"
            "  Free : %8d KB (%4d MB)\n"
            "  Used : %8d KB (%4d MB)\n",
            t / 1024, t / (1024 * 1024),
            f / 1024, f / (1024 * 1024),
            u / 1024, u / (1024 * 1024));

    kprintf("  Free lists:\n");
    for (int order = 0; order <= MAX_ORDER; order++) {
        int count = 0;
        buddy_block_t *block = buddy_state.free_lists[order];
        while (block) {
            count++;
            block = block->next;
        }
        if (count > 0) {
            kprintf("    Order %2d (%4d pages): %ld blocks\n",
                   order, 1 << order, count);
        }
    }
}

/* Buddy allocator operations */
static const allocator_ops_t buddy_allocator = {
    .init = buddy_init,
    .get = buddy_get,
    .alloc = buddy_alloc,
    .free = buddy_free,
    .get_total_memory = buddy_get_total_memory,
    .dump_usage = buddy_dump_usage,
};

void pmm_register_buddy_allocator(void)
{
    pmm_register_allocator(&buddy_allocator);
}


