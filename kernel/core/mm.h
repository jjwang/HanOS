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

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <3rd-party/boot/stivale2.h>

#define PAGE_SIZE               4096
#define BMP_PAGES_PER_BYTE      8

#define HIGHERHALF_OFFSET       0xffffffff80000000

#define NUM_PAGES(num)          (((num) + PAGE_SIZE - 1) / PAGE_SIZE)
#define PAGE_ALIGN_UP(num)      (NUM_PAGES(num) * PAGE_SIZE)

typedef struct {
    uint64_t phys_limit;
    uint64_t total_size;
    uint64_t free_size;

    uint8_t* bitmap;
} mem_info_t;

void pmm_init(mem_info_t* mem, struct stivale2_struct_tag_memmap* map);
uint64_t pmm_get(mem_info_t* mem, uint64_t numpages);
bool pmm_alloc(mem_info_t* mem, uint64_t addr, uint64_t numpages);
void pmm_free(mem_info_t* mem, uint64_t addr, uint64_t numpages);


#define MEM_VIRT_OFFSET         0xffff800000000000

#define VMM_FLAG_PRESENT        (1 << 0)
#define VMM_FLAG_READWRITE      (1 << 1)
#define VMM_FLAG_USER           (1 << 2)
#define VMM_FLAG_WRITETHROUGH   (1 << 3)
#define VMM_FLAG_CACHE_DISABLE  (1 << 4)
#define VMM_FLAG_WRITECOMBINE   (1 << 7)

#define VMM_FLAGS_DEFAULT       (VMM_FLAG_PRESENT | VMM_FLAG_READWRITE)
#define VMM_FLAGS_MMIO          (VMM_FLAGS_DEFAULT | VMM_FLAG_CACHE_DISABLE)
#define VMM_FLAGS_USERMODE      (VMM_FLAGS_DEFAULT | VMM_FLAG_USER)

#define VIRT_TO_PHYS(a)         (((uint64_t)(a)) - MEM_VIRT_OFFSET)
#define PHYS_TO_VIRT(a)         (((uint64_t)(a)) + MEM_VIRT_OFFSET)

typedef struct {
    uint64_t* PML4;
} addrspace_t;

void vmm_init(addrspace_t* addrspace);
void vmm_map(addrspace_t* addrspace, uint64_t vaddr, uint64_t paddr, uint64_t np, uint64_t flags);
void vmm_unmap(addrspace_t* addrspace, uint64_t vaddr, uint64_t np);

