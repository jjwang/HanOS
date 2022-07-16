/**-----------------------------------------------------------------------------

 @file    mm.h
 @brief   Definition of memory management related data structures
 @details
 @verbatim

  Memory management is a critical part of any operating system kernel.
  Providing a quick way for programs to allocate and free memory on a
  regular basis is a major responsibility of the kernel.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdarg.h>
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <3rd-party/boot/limine.h>

#define MM_SIZE                 (256 * MB)

#define PAGE_SIZE               4096
#define BMP_PAGES_PER_BYTE      8

#define USERSPACE_OFFSET        0x1000
#define HIGHERHALF_OFFSET       0xffffffff80000000

/* This address should be same with linker.ld */
#define KERNEL_OFFSET           0xffffffff80200000

#define NUM_PAGES(num)          (((num) + PAGE_SIZE - 1) / PAGE_SIZE)
#define PAGE_ALIGN_UP(num)      (NUM_PAGES(num) * PAGE_SIZE)

typedef struct {
    uint64_t phys_limit;
    uint64_t total_size;
    uint64_t free_size;

    uint8_t* bitmap;
} mem_info_t;

void pmm_init(struct limine_memmap_response* map);
uint64_t pmm_get(uint64_t numpages, uint64_t baseaddr);
bool pmm_alloc(uint64_t addr, uint64_t numpages);
void pmm_free(uint64_t addr, uint64_t numpages);
void pmm_dump_usage(void);

#define VMM_FLAG_PRESENT        (1 << 0)
#define VMM_FLAG_READWRITE      (1 << 1)
#define VMM_FLAG_USER           (1 << 2)
#define VMM_FLAG_WRITETHROUGH   (1 << 3)
#define VMM_FLAG_CACHE_DISABLE  (1 << 4)
#define VMM_FLAG_WRITECOMBINE   (1 << 7)

#define VMM_FLAGS_DEFAULT       (VMM_FLAG_PRESENT | VMM_FLAG_READWRITE)
#define VMM_FLAGS_MMIO          (VMM_FLAGS_DEFAULT | VMM_FLAG_CACHE_DISABLE)
#define VMM_FLAGS_USERMODE      (VMM_FLAGS_DEFAULT | VMM_FLAG_USER)

#define MEM_VIRT_OFFSET         0xffff800000000000

#define VIRT_TO_PHYS(a)         (((uint64_t)(a)) - MEM_VIRT_OFFSET)
#define PHYS_TO_VIRT(a)         (((uint64_t)(a)) + MEM_VIRT_OFFSET)

typedef struct {
    uint64_t* PML4;
} addrspace_t;

void vmm_init(
    struct limine_memmap_response* map,
    struct limine_kernel_address_response* kernel);

void vmm_map(uint64_t vaddr, uint64_t paddr, uint64_t np, uint64_t flags);
void vmm_unmap(uint64_t vaddr, uint64_t np);

