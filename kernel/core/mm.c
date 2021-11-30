///-----------------------------------------------------------------------------
///
/// @file    mm.c
/// @brief   Implementation of memory management functions
/// @details
///
///   Memory management is a critical part of any operating system kernel.
///   Providing a quick way for programs to allocate and free memory on a
///   regular basis is a major responsibility of the kernel.
///
/// @author  JW
/// @date    Nov 27, 2021
///
///-----------------------------------------------------------------------------
#include <stdint.h>
#include <core/mm.h>

void mem_init(mem_info_t* mem, struct stivale2_struct_tag_memmap* map)
{
    mem->phys_limit = 0;
    mem->total_size = 0;
    mem->free_size = 0;

    for (uint64_t i = 0; i < map->entries; i++) {
        struct stivale2_mmap_entry entry = map->memmap[i];

        if (entry.base + entry.length <= 0x100000)
            continue;

        uint64_t new_limit = entry.base + entry.length;
        if (new_limit > mem->phys_limit) {
            mem->phys_limit = new_limit;
        }

        if (entry.type == STIVALE2_MMAP_USABLE
            || entry.type == STIVALE2_MMAP_BOOTLOADER_RECLAIMABLE
            || entry.type == STIVALE2_MMAP_ACPI_RECLAIMABLE
            || entry.type == STIVALE2_MMAP_KERNEL_AND_MODULES) {
            mem->total_size += entry.length;
        }
    }   
}

