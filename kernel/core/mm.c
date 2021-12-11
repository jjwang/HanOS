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
///   High Half Kernel: To setup a higher half kernel, you have to map your
///   kernel to the appropriate virtual address. Without a boot loader help,
///   you'll need a small trampoline code which runs in lower half, sets up
///   higher half paging and jumps.
///
///   If page protection is not enabled, virtual address is equal with physical
///   address. The highest bit of CR0 indicates whether paging is enabled or
///   not: mov cr0,8000000 can enable paging.
///
///   PMM: The method behind PMM is very simple. The memories with type -
///   STIVALE2_MMAP_USABLE are devided into 4K-size pages. A bitmap array is
///   used for indicated whether it is free or not. One bit for one page in
///   bitmap array.
///
/// @author  JW
/// @date    Nov 27, 2021
///
///-----------------------------------------------------------------------------
#include <stdint.h>
#include <core/cpu.h>
#include <core/mm.h>
#include <core/panic.h>
#include <lib/memutils.h>
#include <lib/klog.h>
#include <lib/kmalloc.h>

static mem_info_t* kmem_info = NULL;
static addrspace_t* kaddrspace = NULL;

static void bitmap_markused(mem_info_t* mem, uint64_t addr, uint64_t numpages)
{
    if (mem == NULL) mem = kmem_info;   
    for (uint64_t i = addr; i < addr + (numpages * PAGE_SIZE); i += PAGE_SIZE) { 
        mem->bitmap[i / (PAGE_SIZE * BMP_PAGES_PER_BYTE)] &= ~((1 << ((i / PAGE_SIZE) % BMP_PAGES_PER_BYTE)));
    }
}

static bool bitmap_isfree(mem_info_t* mem, uint64_t addr, uint64_t numpages)
{
    if (mem == NULL) mem = kmem_info;

    bool free = true;
    
    for (uint64_t i = addr; i < addr + (numpages * PAGE_SIZE); i += PAGE_SIZE) {
        free = mem->bitmap[i / (PAGE_SIZE * BMP_PAGES_PER_BYTE)] & (1 << ((i / PAGE_SIZE) % BMP_PAGES_PER_BYTE));
        if (!free)
            break;
    }
    return free;
}

// marks pages as free
void pmm_free(mem_info_t* mem, uint64_t addr, uint64_t numpages)
{
    if (mem == NULL) mem = kmem_info;
  
    for (uint64_t i = addr; i < addr + (numpages * PAGE_SIZE); i += PAGE_SIZE) {
        if (!bitmap_isfree(mem, i, 1))
            mem->free_size += PAGE_SIZE;
        
        mem->bitmap[i / (PAGE_SIZE * BMP_PAGES_PER_BYTE)] |= 1 << ((i / PAGE_SIZE) % BMP_PAGES_PER_BYTE);
    }
}

// marks pages as used, returns true if success, false otherwise
bool pmm_alloc(mem_info_t* mem, uint64_t addr, uint64_t numpages)
{
    if (mem == NULL) mem = kmem_info;

    if (!bitmap_isfree(mem, addr, numpages))
        return false;

    bitmap_markused(mem, addr, numpages);
    mem->free_size -= numpages * PAGE_SIZE;
    return true;
}

uint64_t pmm_get(mem_info_t* mem, uint64_t numpages)
{
    if (mem == NULL) mem = kmem_info;

    static uint64_t lastusedpage = 0;

    for (uint64_t i = lastusedpage; i < mem->phys_limit; i += PAGE_SIZE) {
        if (pmm_alloc(mem, i, numpages))
            return i;
    }

    for (uint64_t i = 0; i < lastusedpage; i += PAGE_SIZE) {
        if (pmm_alloc(mem, i, numpages))
            return i;
    }

    kpanic("Out of Physical Memory");
    return 0;
}

void pmm_init(mem_info_t* mem, struct stivale2_struct_tag_memmap* map)
{
    kmem_info = mem;

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

    // look for a good place to keep our bitmap
    uint64_t bm_size = mem->phys_limit / (PAGE_SIZE * BMP_PAGES_PER_BYTE);
    for (size_t i = 0; i < map->entries; i++) {
        struct stivale2_mmap_entry entry = map->memmap[i];

        if (entry.base + entry.length <= 0x100000)
            continue;

        if (entry.length >= bm_size && entry.type == STIVALE2_MMAP_USABLE) {
            mem->bitmap = (uint8_t*)PHYS_TO_VIRT(entry.base);
            break;
        }
    }

    // zero it out
    memset(mem->bitmap, 0, bm_size);
    klog_printf("Memory bitmap address: %x\n", mem->bitmap);

    // now populate the bitmap
    for (size_t i = 0; i < map->entries; i++) {
        struct stivale2_mmap_entry entry = map->memmap[i];

        if (entry.base + entry.length <= 0x100000)
            continue;

        if (entry.type == STIVALE2_MMAP_USABLE)
            pmm_free(mem, entry.base, NUM_PAGES(entry.length));
    }

    // mark the bitmap as used
    pmm_alloc(mem, VIRT_TO_PHYS(mem->bitmap), NUM_PAGES(bm_size));

    klog_printf("PMM initialization finished.\n");   
}

//------------------------------------------------------------------------------
// Below is virtual memory management related part

#define MAKE_TABLE_ENTRY(address, flags)    ((address & ~(0xfff)) | flags)

static void map_page(addrspace_t* addrspace, uint64_t vaddr, uint64_t paddr, uint64_t flags)
{
    uint16_t pte   = (vaddr >> 12) & 0x1ff;
    uint16_t pde   = (vaddr >> 21) & 0x1ff;
    uint16_t pdpe  = (vaddr >> 30) & 0x1ff;
    uint16_t pml4e = (vaddr >> 39) & 0x1ff;

    uint64_t* pml4 = addrspace->PML4;
    uint64_t* pdpt;
    uint64_t* pd; 
    uint64_t* pt; 

    pdpt = (uint64_t*)PHYS_TO_VIRT(pml4[pml4e] & ~(0xfff));
    if (!(pml4[pml4e] & VMM_FLAG_PRESENT)) {
        pdpt = (uint64_t*)PHYS_TO_VIRT(pmm_get(kmem_info, 1));
        memset(pdpt, 0, PAGE_SIZE);
        pml4[pml4e] = MAKE_TABLE_ENTRY(VIRT_TO_PHYS(pdpt), VMM_FLAGS_USERMODE);
    }   

    pd = (uint64_t*)PHYS_TO_VIRT(pdpt[pdpe] & ~(0xfff));
    if (!(pdpt[pdpe] & VMM_FLAG_PRESENT)) {
        pd = (uint64_t*)PHYS_TO_VIRT(pmm_get(kmem_info, 1));
        memset(pd, 0, PAGE_SIZE);
        pdpt[pdpe] = MAKE_TABLE_ENTRY(VIRT_TO_PHYS(pd), VMM_FLAGS_USERMODE);
    }

    pt = (uint64_t*)PHYS_TO_VIRT(pd[pde] & ~(0xfff));
    if (!(pd[pde] & VMM_FLAG_PRESENT)) {
        pt = (uint64_t*)PHYS_TO_VIRT(pmm_get(kmem_info, 1));
        memset(pt, 0, PAGE_SIZE);
        pd[pde] = MAKE_TABLE_ENTRY(VIRT_TO_PHYS(pt), VMM_FLAGS_USERMODE);
    }

    pt[pte] = MAKE_TABLE_ENTRY(paddr & ~(0xfff), flags);

    uint64_t cr3val;
    read_cr("cr3", &cr3val);
    if (cr3val == (uint64_t)(VIRT_TO_PHYS(addrspace->PML4)))
        asm volatile("invlpg (%0)" ::"r"(vaddr));
}

static void unmap_page(addrspace_t* addrspace, uint64_t vaddr)
{
    uint16_t pte = (vaddr >> 12) & 0x1ff;
    uint16_t pde = (vaddr >> 21) & 0x1ff;
    uint16_t pdpe = (vaddr >> 30) & 0x1ff;
    uint16_t pml4e = (vaddr >> 39) & 0x1ff;

    uint64_t* pml4 = addrspace->PML4;
    if (!(pml4[pml4e] & VMM_FLAG_PRESENT))
        return;

    uint64_t* pdpt = (uint64_t*)PHYS_TO_VIRT(pml4[pml4e] & ~(0x1ff));
    if (!(pdpt[pdpe] & VMM_FLAG_PRESENT))
        return;


    uint64_t* pd = (uint64_t*)PHYS_TO_VIRT(pdpt[pdpe] & ~(0x1ff));
    if (!(pd[pde] & VMM_FLAG_PRESENT))
        return;

    uint64_t* pt = (uint64_t*)PHYS_TO_VIRT(pd[pde] & ~(0x1ff));
    if (!(pt[pte] & VMM_FLAG_PRESENT))
        return;

    pt[pte] = 0;

    uint64_t cr3val;
    read_cr("cr3", &cr3val);
    if (cr3val == (uint64_t)(VIRT_TO_PHYS(addrspace->PML4)))
        asm volatile("invlpg (%0)" ::"r"(vaddr));

    for (int i = 0; i < 512; i++)
        if (pt[i] != 0)
            goto done;
    pd[pde] = 0;
    pmm_free(kmem_info, VIRT_TO_PHYS(pt), 1);

    for (int i = 0; i < 512; i++)
        if (pd[i] != 0)
            goto done;
    pdpt[pdpe] = 0;
    pmm_free(kmem_info, VIRT_TO_PHYS(pd), 1);

    for (int i = 0; i < 512; i++)
        if (pdpt[i] != 0)
            goto done;
    pml4[pml4e] = 0;
    pmm_free(kmem_info, VIRT_TO_PHYS(pdpt), 1);

done:
    return;
}

void vmm_unmap(addrspace_t* addrspace, uint64_t vaddr, uint64_t np) 
{
    addrspace_t* as = addrspace ? addrspace : kaddrspace;
    for (size_t i = 0; i < np * PAGE_SIZE; i += PAGE_SIZE)
        unmap_page(as, vaddr + i); 
}

void vmm_map(addrspace_t* addrspace, uint64_t vaddr, uint64_t paddr, uint64_t np, uint64_t flags)
{
    addrspace_t* as = addrspace ? addrspace : kaddrspace;
    for (size_t i = 0; i < np * PAGE_SIZE; i += PAGE_SIZE)
        map_page(as, vaddr + i, paddr + i, flags);
}

void vmm_init(addrspace_t* addrspace)
{
    kaddrspace = addrspace;

    // create the kernel address space
    addrspace->PML4 = kmalloc(PAGE_SIZE);
    memset(addrspace->PML4, 0, PAGE_SIZE);

    vmm_map(addrspace, 0xffffffff80000000, 0, NUM_PAGES(0x80000000), VMM_FLAGS_DEFAULT);
    klog_printf("Mapped lower 2GB to 0xFFFFFFFF80000000\n");

    vmm_map(addrspace, 0xffff800000000000, 0, NUM_PAGES(kmem_info->phys_limit), VMM_FLAGS_DEFAULT);
    klog_printf("Mapped all memory to 0xFFFF800000000000\n");

    write_cr("cr3", VIRT_TO_PHYS(addrspace->PML4));
    klog_printf("VMM Initialization finished.\n");
}

