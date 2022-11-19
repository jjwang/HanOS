/**-----------------------------------------------------------------------------
 @file    mm.c
 @brief   Implementation of memory management functions
 @details
 @verbatim
  Memory management is a critical part of any operating system kernel.
  Providing a quick way for programs to allocate and free memory on a
  regular basis is a major responsibility of the kernel.
  High Half Kernel: To setup a higher half kernel, you have to map your
  kernel to the appropriate virtual address. Without a boot loader help,
  you'll need a small trampoline code which runs in lower half, sets up
  higher half paging and jumps.
  If page protection is not enabled, virtual address is equal with physical
  address. The highest bit of CR0 indicates whether paging is enabled or
  not: mov cr0,8000000 can enable paging.
  PMM: The method behind PMM is very simple. The memories with type -
  STIVALE2_MMAP_USABLE are devided into 4K-size pages. A bitmap array is
  used for indicated whether it is free or not. One bit for one page in
  bitmap array.
 @endverbatim
 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <core/cpu.h>
#include <core/mm.h>
#include <core/panic.h>
#include <lib/memutils.h>
#include <lib/klog.h>
#include <lib/kmalloc.h>
#include <lib/klib.h>

static mem_info_t kmem_info = {0};
static addrspace_t kaddrspace = {0};

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

void pmm_free(uint64_t addr, uint64_t numpages)
{
    for (uint64_t i = addr; i < addr + (numpages * PAGE_SIZE); i += PAGE_SIZE) {
        if (!bitmap_isfree(i, 1))
            kmem_info.free_size += PAGE_SIZE;
        
        kmem_info.bitmap[i / (PAGE_SIZE * BMP_PAGES_PER_BYTE)]
            |= 1 << ((i / PAGE_SIZE) % BMP_PAGES_PER_BYTE);
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

uint64_t pmm_get(uint64_t numpages, uint64_t baseaddr)
{
    for (uint64_t i = baseaddr; i < kmem_info.phys_limit; i += PAGE_SIZE) {
        if (pmm_alloc(i, numpages))
            return i;
    }

    kpanic("Out of Physical Memory");
    return 0;
}

void pmm_init(struct limine_memmap_response* map)
{
    kmem_info.phys_limit = 0;
    kmem_info.total_size = 0;
    kmem_info.free_size = 0;

    klogv("Physical memory's entry number: %d\n", map->entry_count);

    for (size_t i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry* entry = map->entries[i];
        uint64_t new_limit = entry->base + entry->length;

        if (new_limit > kmem_info.phys_limit) {
            kmem_info.phys_limit = new_limit;
        }

        if (entry->type == LIMINE_MEMMAP_USABLE
            || entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE
            || entry->type == LIMINE_MEMMAP_ACPI_RECLAIMABLE
            || entry->type == LIMINE_MEMMAP_KERNEL_AND_MODULES) {
            kmem_info.total_size += entry->length;
        }   
    }

    /* look for a good place to keep our bitmap */
    uint64_t bm_size = kmem_info.phys_limit / (PAGE_SIZE * BMP_PAGES_PER_BYTE);
    bool gotit = false;
    for (size_t i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry* entry = map->entries[i];

        if (entry->base + entry->length <= 0x100000)
            continue;

        if (entry->length >= bm_size && entry->type == LIMINE_MEMMAP_USABLE) {
            if (!gotit) kmem_info.bitmap = (uint8_t*)PHYS_TO_VIRT(entry->base);
            gotit = true;
        }
    }

    memset(kmem_info.bitmap, 0, bm_size);
    klogi("Memory bitmap address: 0x%x\n", kmem_info.bitmap);

    /* now populate the bitmap */
    for (size_t i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry* entry = map->entries[i];

        if (entry->base + entry->length <= 0x100000)
            continue;

        if (entry->type == LIMINE_MEMMAP_USABLE)
            pmm_free(entry->base, NUM_PAGES(entry->length));
    }

    /* mark the bitmap as used */
    pmm_alloc(VIRT_TO_PHYS(kmem_info.bitmap), NUM_PAGES(bm_size));

    klogi("PMM initialization finished\n");   
    klogi("Memory total: %d, phys limit: %d, free: %d, used: %d\n",
          kmem_info.total_size, kmem_info.phys_limit, kmem_info.free_size,
          kmem_info.total_size - kmem_info.free_size);
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
}

/*------------------------------------------------------------------------------
 * Below is virtual memory management related part
 */

#define MAKE_TABLE_ENTRY(address, flags)    ((address & ~(0xfff)) | flags)

static void map_page(addrspace_t *addrspace, uint64_t vaddr, uint64_t paddr,
    uint64_t flags)
{
    addrspace_t *as = (addrspace == NULL ? &kaddrspace : addrspace);

    uint16_t pte   = (vaddr >> 12) & 0x1ff;
    uint16_t pde   = (vaddr >> 21) & 0x1ff;
    uint16_t pdpe  = (vaddr >> 30) & 0x1ff;
    uint16_t pml4e = (vaddr >> 39) & 0x1ff;

    uint64_t* pml4 = as->PML4;
    uint64_t* pdpt;
    uint64_t* pd; 
    uint64_t* pt; 

    pdpt = (uint64_t*)PHYS_TO_VIRT(pml4[pml4e] & ~(0xfff));
    if (!(pml4[pml4e] & VMM_FLAG_PRESENT)) {
        pdpt = (uint64_t*)PHYS_TO_VIRT(pmm_get(1, 0x0));
        memset(pdpt, 0, PAGE_SIZE);
        pml4[pml4e] = MAKE_TABLE_ENTRY(VIRT_TO_PHYS(pdpt), VMM_FLAGS_USERMODE);
    }   

    pd = (uint64_t*)PHYS_TO_VIRT(pdpt[pdpe] & ~(0xfff));
    if (!(pdpt[pdpe] & VMM_FLAG_PRESENT)) {
        pd = (uint64_t*)PHYS_TO_VIRT(pmm_get(1, 0x0));
        memset(pd, 0, PAGE_SIZE);
        pdpt[pdpe] = MAKE_TABLE_ENTRY(VIRT_TO_PHYS(pd), VMM_FLAGS_USERMODE);
    }

    pt = (uint64_t*)PHYS_TO_VIRT(pd[pde] & ~(0xfff));
    if (!(pd[pde] & VMM_FLAG_PRESENT)) {
        pt = (uint64_t*)PHYS_TO_VIRT(pmm_get(1, 0x0));
        memset(pt, 0, PAGE_SIZE);
        pd[pde] = MAKE_TABLE_ENTRY(VIRT_TO_PHYS(pt), VMM_FLAGS_USERMODE);
    }

    pt[pte] = MAKE_TABLE_ENTRY(paddr & ~(0xfff), flags);

    uint64_t cr3val;
    read_cr("cr3", &cr3val);
    if (cr3val == (uint64_t)(VIRT_TO_PHYS(as->PML4)))
        asm volatile("invlpg (%0)" ::"r"(vaddr));
}

static void unmap_page(addrspace_t *addrspace, uint64_t vaddr)
{
    addrspace_t *as = (addrspace == NULL ? &kaddrspace : addrspace);

    uint16_t pte = (vaddr >> 12) & 0x1ff;
    uint16_t pde = (vaddr >> 21) & 0x1ff;
    uint16_t pdpe = (vaddr >> 30) & 0x1ff;
    uint16_t pml4e = (vaddr >> 39) & 0x1ff;

    uint64_t* pml4 = as->PML4;
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
    if (cr3val == (uint64_t)(VIRT_TO_PHYS(as->PML4)))
        asm volatile("invlpg (%0)" ::"r"(vaddr));

    for (int i = 0; i < 512; i++)
        if (pt[i] != 0)
            goto done;
    pd[pde] = 0;
    pmm_free(VIRT_TO_PHYS(pt), 1);

    for (int i = 0; i < 512; i++)
        if (pd[i] != 0)
            goto done;
    pdpt[pdpe] = 0;
    pmm_free(VIRT_TO_PHYS(pd), 1);

    for (int i = 0; i < 512; i++)
        if (pdpt[i] != 0)
            goto done;
    pml4[pml4e] = 0;
    pmm_free(VIRT_TO_PHYS(pdpt), 1);

done:
    return;
}

void vmm_unmap(addrspace_t *addrspace, uint64_t vaddr, uint64_t np) 
{
    for (size_t i = 0; i < np * PAGE_SIZE; i += PAGE_SIZE)
        unmap_page(addrspace, vaddr + i); 
}

void vmm_map(addrspace_t *addrspace, uint64_t vaddr, uint64_t paddr,
    uint64_t np, uint64_t flags)
{
    for (size_t i = 0; i < np * PAGE_SIZE; i += PAGE_SIZE)
        map_page(addrspace, vaddr + i, paddr + i, flags);
}

void vmm_init(
    struct limine_memmap_response* map,
    struct limine_kernel_address_response* kernel)
{
    kaddrspace.PML4 = umalloc(PAGE_SIZE);
    memset(kaddrspace.PML4, 0, PAGE_SIZE);

    vmm_map(NULL, MEM_VIRT_OFFSET, 0,
            NUM_PAGES(kmem_info.phys_limit),
            VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE);
    klogd("Mapped %d bytes memory to 0x%x\n",
            kmem_info.phys_limit, MEM_VIRT_OFFSET);

    for (size_t i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry* entry = map->entries[i];

        if (entry->type == LIMINE_MEMMAP_KERNEL_AND_MODULES) {
            uint64_t vaddr = kernel->virtual_base
                             + entry->base - kernel->physical_base;
            vmm_map(NULL, vaddr, entry->base, NUM_PAGES(entry->length),
                    VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE);
            klogd("Mapped kernel 0x%9x to 0x%x (len: %d)\n",
                  entry->base, vaddr, entry->length);
        } else if (entry->type == LIMINE_MEMMAP_FRAMEBUFFER) {
            vmm_map(NULL, PHYS_TO_VIRT(entry->base), entry->base,
                    NUM_PAGES(entry->length),
                    VMM_FLAGS_DEFAULT | VMM_FLAG_WRITECOMBINE | VMM_FLAGS_USERMODE);
            klogd("Mapped framebuffer 0x%9x to 0x%x (len: %d)\n",
                  entry->base, PHYS_TO_VIRT(entry->base), entry->length);
        } else {
            vmm_map(NULL, PHYS_TO_VIRT(entry->base), entry->base,
                    NUM_PAGES(entry->length),
                    VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE);
            klogd("Mapped 0x%9x to 0x%x(len: %d)\n",
                  entry->base, PHYS_TO_VIRT(entry->base), entry->length);
        }
    }

    write_cr("cr3", VIRT_TO_PHYS(kaddrspace.PML4));
    klogi("VMM initialization finished\n");
}

addrspace_t *create_addrspace(void)
{
    addrspace_t *as = umalloc(sizeof(addrspace_t));
    if (!as)
        return NULL;
    as->PML4 = (uint64_t*)pmm_get(1, 0x0);
    if (!as->PML4) {
        kmfree(as);
        return NULL;
    }
    as->PML4 = (void *)PHYS_TO_VIRT(as->PML4);
    memset(as->PML4, 0, PAGE_SIZE);
    as->lock = lock_new();
    return as;
}

void destory_addrspace(addrspace_t *as)
{
    uint64_t *pdpt, *pd, *pt;

    for (size_t i = 0; i < PAGE_TABLE_ENTRIES / 2; i++) {
        if (as->PML4[i] & 1) {
            pdpt = (uint64_t *)PHYS_TO_VIRT(as->PML4[i] & 0xfffffffffffff000);
            for (size_t j = 0; j < PAGE_TABLE_ENTRIES; j++) {
                if (pdpt[j] & 1) {
                    pd = (uint64_t *)PHYS_TO_VIRT(pdpt[j] & 0xfffffffffffff000);
                    for (size_t k = 0; k < PAGE_TABLE_ENTRIES; k++) {
                        if (pd[k] & 1) {
                            pt = (uint64_t *)PHYS_TO_VIRT(pd[k] & 0xfffffffffffff000);
                            for (size_t l = 0; l < PAGE_TABLE_ENTRIES; l++) {
                                if (pt[l] & 1)
                                    pmm_free(VIRT_TO_PHYS(pt[l] & 0xfffffffffffff000), 1);
                            }
                            pmm_free(VIRT_TO_PHYS(pd[k] & 0xfffffffffffff000), 1);
                        }
                    }
                    pmm_free(VIRT_TO_PHYS(pdpt[j] & 0xfffffffffffff000), 1);
                }
            }
            pmm_free(VIRT_TO_PHYS(as->PML4[i] & 0xfffffffffffff000), 1);
        }
    }

    pmm_free(VIRT_TO_PHYS(as->PML4), 1);
    umfree(as);
}

