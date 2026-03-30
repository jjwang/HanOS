/**-----------------------------------------------------------------------------

 @file    pmm_ops.h
 @brief   Pluggable physical memory allocator interface
 @details
 @verbatim

  This header defines a pluggable interface for physical memory allocators.
  Different allocation strategies (bitmap, buddy system, etc.) can be
  implemented and switched via compile-time configuration.

  The interface provides:
  - Page allocation and deallocation
  - Memory statistics and debugging
  - Initialization from bootloader memory map

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>
#include <stdbool.h>
#include <3rd-party/boot/limine.h>

/**
 * @brief Physical memory allocator operations
 *
 * This structure defines the interface that all physical memory
 * allocators must implement.
 */
typedef struct allocator_ops {
    /**
     * @brief Initialize the allocator
     * @param map Memory map from bootloader
     * @param higher_half Higher half kernel offset
     */
    void (*init)(struct limine_memmap_response *map, uint64_t higher_half);

    /**
     * @brief Allocate physical pages
     * @param numpages Number of contiguous pages to allocate
     * @param baseaddr Base address to start searching from
     * @param func Caller function name (for debugging)
     * @param line Caller line number (for debugging)
     * @return Physical address of allocated pages, or 0 on failure
     */
    uint64_t (*get)(uint64_t numpages, uint64_t baseaddr,
                    const char *func, int64_t line);

    /**
     * @brief Allocate pages at specific address
     * @param addr Physical address to allocate at
     * @param numpages Number of pages to allocate
     * @return true if successful, false if already allocated
     */
    bool (*alloc)(uint64_t addr, uint64_t numpages);

    /**
     * @brief Free physical pages
     * @param addr Physical address to free
     * @param numpages Number of pages to free
     * @param func Caller function name (for debugging)
     * @param line Caller line number (for debugging)
     */
    void (*free)(uint64_t addr, uint64_t numpages,
                 const char *func, int64_t line);

    /**
     * @brief Get total memory in MB
     * @return Total physical memory in megabytes
     */
    uint64_t (*get_total_memory)(void);

    /**
     * @brief Dump memory usage statistics
     */
    void (*dump_usage)(void);
} allocator_ops_t;

/**
 * @brief Register a physical memory allocator
 * @param ops Allocator operations structure
 */
void pmm_register_allocator(const allocator_ops_t *ops);

/**
 * @brief Get the current allocator
 * @return Pointer to current allocator operations
 */
const allocator_ops_t *pmm_get_allocator(void);
