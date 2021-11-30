///-----------------------------------------------------------------------------
///
/// @file    mm.h
/// @brief   Definition of memory management related data structures
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
#pragma once

#include <3rd-party/boot/stivale2.h>

typedef struct {
    uint64_t phys_limit;
    uint64_t total_size;
    uint64_t free_size;
} mem_info_t;

void mem_init(mem_info_t* mem, struct stivale2_struct_tag_memmap* map);


