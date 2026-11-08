/**-----------------------------------------------------------------------------

 @file    vma.h
 @brief   Virtual memory areas
 @details
 @verbatim

  One sorted area per user mapping. A VMA owns the physical allocation behind
  its pages; kernel buffers stay in the address space owned list.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>

/**
 * @brief One user virtual memory area
 */
typedef struct {
    uint64_t start;
    uint64_t end;
    uint64_t paddr;
    uint64_t flags;
} vma_t;

/**
 * @brief Sorted VMA table
 */
typedef struct {
    vma_t *tab;
    uint32_t len;
    uint32_t cap;
} vma_map_t;

struct addrspace;
struct mem_map;

void vma_init(vma_map_t * map);
void vma_destroy(struct addrspace *as, vma_map_t * map);
uint32_t vma_count(const vma_map_t * map);
int vma_add(vma_map_t * map, uint64_t start, uint64_t end, uint64_t paddr,
            uint64_t flags);
void vma_remove_range(struct addrspace *as, vma_map_t * map, uint64_t start,
                      uint64_t end);
int vma_clone(struct addrspace *dst_as, const vma_map_t * src,
              vma_map_t * dst);
