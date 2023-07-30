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

#include <sys/cpu.h>
#include <sys/mm.h>
#include <sys/panic.h>
#include <lib/memutils.h>
#include <lib/klog.h>
#include <lib/kmalloc.h>
#include <lib/klib.h>
#include <lib/vector.h>

static mem_info_t kmem_info = {0};
static addrspace_t kaddrspace = {0};
static bool debug_info = false;

vec_new_static(mem_map_t, mmap_list);

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
    const char *func, size_t line)
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
    const char *func, size_t line)
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

void pmm_init(struct limine_memmap_response* map)
{
    kmem_info.phys_limit = 0;
    kmem_info.total_size = 0;
    kmem_info.free_size = 0;

    klogv("Physical memory's entry number: %d\n", map->entry_count);

    for (size_t i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry* entry = map->entries[i];

        if (entry->type == LIMINE_MEMMAP_RESERVED) {
            /* Skip this type of memory and maybe it will be corrupt */
            continue;
        }

        if (entry->type == LIMINE_MEMMAP_USABLE
            || entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE
            || entry->type == LIMINE_MEMMAP_ACPI_RECLAIMABLE
            || entry->type == LIMINE_MEMMAP_KERNEL_AND_MODULES) {
            kmem_info.total_size += entry->length;
        }

        uint64_t new_limit = entry->base + entry->length;

        if (new_limit > kmem_info.phys_limit) {
            kmem_info.phys_limit = new_limit;
            klogd("PMM: entry base 0x%x, length %d, type %d\n",
                  entry->base, entry->length, entry->type);
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
            pmm_free(entry->base, NUM_PAGES(entry->length), __func__, __LINE__);
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
        pdpt = (uint64_t*)PHYS_TO_VIRT(pmm_get(8, 0x0, __func__, __LINE__));
        memset(pdpt, 0, PAGE_SIZE * 8);
        pml4[pml4e] = MAKE_TABLE_ENTRY(VIRT_TO_PHYS(pdpt), VMM_FLAGS_USERMODE);
        vec_push_back(&as->mem_list, VIRT_TO_PHYS(pdpt));
    }   

    pd = (uint64_t*)PHYS_TO_VIRT(pdpt[pdpe] & ~(0xfff));
    if (!(pdpt[pdpe] & VMM_FLAG_PRESENT)) {
        pd = (uint64_t*)PHYS_TO_VIRT(pmm_get(8, 0x0, __func__, __LINE__));
        memset(pd, 0, PAGE_SIZE * 8);
        pdpt[pdpe] = MAKE_TABLE_ENTRY(VIRT_TO_PHYS(pd), VMM_FLAGS_USERMODE);
        vec_push_back(&as->mem_list, VIRT_TO_PHYS(pd));
    }

    pt = (uint64_t*)PHYS_TO_VIRT(pd[pde] & ~(0xfff));
    if (!(pd[pde] & VMM_FLAG_PRESENT)) {
        pt = (uint64_t*)PHYS_TO_VIRT(pmm_get(8, 0x0, __func__, __LINE__));
        memset(pt, 0, PAGE_SIZE * 8);
        pd[pde] = MAKE_TABLE_ENTRY(VIRT_TO_PHYS(pt), VMM_FLAGS_USERMODE);
        vec_push_back(&as->mem_list, VIRT_TO_PHYS(pt));
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

    for (int i = 0; i < 512 * 8; i++)
        if (pt[i] != 0)
            goto done;

    pd[pde] = 0;
    pmm_free(VIRT_TO_PHYS(pt), 8, __func__, __LINE__);

    for (int i = 0; i < 512 * 8; i++)
        if (pd[i] != 0)
            goto done;

    pdpt[pdpe] = 0;
    pmm_free(VIRT_TO_PHYS(pd), 8, __func__, __LINE__);

    for (int i = 0; i < 512 * 8; i++)
        if (pdpt[i] != 0)
            goto done;

    pml4[pml4e] = 0;
    pmm_free(VIRT_TO_PHYS(pdpt), 8, __func__, __LINE__);

done:
    return;
}

uint64_t vmm_get_paddr(addrspace_t *addrspace, uint64_t vaddr)
{
    addrspace_t *as = (addrspace == NULL ? &kaddrspace : addrspace);

    uint16_t pte = (vaddr >> 12) & 0x1ff;
    uint16_t pde = (vaddr >> 21) & 0x1ff;
    uint16_t pdpe = (vaddr >> 30) & 0x1ff;
    uint16_t pml4e = (vaddr >> 39) & 0x1ff;

    uint64_t* pml4 = as->PML4;
    if (!(pml4[pml4e] & VMM_FLAG_PRESENT))
        return (uint64_t)NULL;

    uint64_t* pdpt = (uint64_t*)PHYS_TO_VIRT(pml4[pml4e] & ~(0x1ff));
    if (!(pdpt[pdpe] & VMM_FLAG_PRESENT))
        return (uint64_t)NULL;

    uint64_t* pd = (uint64_t*)PHYS_TO_VIRT(pdpt[pdpe] & ~(0x1ff));
    if (!(pd[pde] & VMM_FLAG_PRESENT))
        return (uint64_t)NULL;

    uint64_t* pt = (uint64_t*)PHYS_TO_VIRT(pd[pde] & ~(0x1ff));
    if (!(pt[pte] & VMM_FLAG_PRESENT))
        return (uint64_t)NULL;

    return (pt[pte] & 0xFFFFFFFFFFFFF000);
}
                    
void vmm_unmap(addrspace_t *addrspace, uint64_t vaddr, uint64_t np, bool us) 
{
    if (us && (addrspace == NULL)) {
        /* We must unmap the corresponding vaddr in vmm_map() function */
        size_t len = vec_length(&mmap_list);
        for (size_t i = 0; i < len; i++) {
            mem_map_t m = vec_at(&mmap_list, i); 
            if (m.vaddr != vaddr) {
                vec_erase(&mmap_list, i); 
                break;
            }   
        }   
    }

    for (size_t i = 0; i < np * PAGE_SIZE; i += PAGE_SIZE)
        unmap_page(addrspace, vaddr + i); 

    if (debug_info) {
        klogd("VMM: PML4 0x%x un-mapped virt 0x%x (%d pages)\n",
              (addrspace == NULL ? kaddrspace.PML4 : addrspace->PML4),
              vaddr, np);
    }
}

void vmm_map(addrspace_t *addrspace, uint64_t vaddr, uint64_t paddr,
    uint64_t np, uint64_t flags, bool us)
{
    if (us && (addrspace == NULL)) {
        mem_map_t mm = {
            .vaddr = vaddr,
            .paddr = paddr,
            .flags= flags,
            .np = np
        };
        vec_push_back(&mmap_list, mm);
    }

    for (size_t i = 0; i < np * PAGE_SIZE; i += PAGE_SIZE) {
        map_page(addrspace, vaddr + i, paddr + i, flags);
    }

    if (debug_info) {
        klogd("VMM: PML4 0x%x mapped phys 0x%x to virt 0x%x (%d pages)\n",
              (addrspace == NULL ? kaddrspace.PML4 : addrspace->PML4),
              paddr, vaddr, np);
    }
}

void vmm_init(
    struct limine_memmap_response* map,
    struct limine_kernel_address_response* kernel)
{
    size_t i;

    kaddrspace.PML4 = kmalloc(PAGE_SIZE * 8);
    memset(kaddrspace.PML4, 0, PAGE_SIZE * 8);

    /*
     * Since we share the same virtual memory manager with user mode, here
     * we must set flags with USERMODE.
     */
    vmm_map(NULL, MEM_VIRT_OFFSET, 0, NUM_PAGES(kmem_info.phys_limit),
            VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE, true);
    klogd("Mapped %d bytes memory to 0x%x\n",
            kmem_info.phys_limit, MEM_VIRT_OFFSET);

    for (i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry* entry = map->entries[i];

        if (entry->type == LIMINE_MEMMAP_KERNEL_AND_MODULES) {
            uint64_t vaddr = kernel->virtual_base
                             + entry->base - kernel->physical_base;
            vmm_map(NULL, vaddr, entry->base, NUM_PAGES(entry->length),
                    VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE, true);
            klogd("Mapped kernel 0x%9x to 0x%x (len: %d)\n",
                  entry->base, vaddr, entry->length);
        } else if (entry->type == LIMINE_MEMMAP_FRAMEBUFFER) {
            vmm_map(NULL, PHYS_TO_VIRT(entry->base), entry->base,
                    NUM_PAGES(entry->length),
                    VMM_FLAGS_DEFAULT | VMM_FLAG_WRITECOMBINE
                    | VMM_FLAGS_USERMODE, true);
            klogd("Mapped framebuffer 0x%9x to 0x%x (len: %d)\n",
                  entry->base, PHYS_TO_VIRT(entry->base), entry->length);

#if !LAUNCHER_GRAPHICS
            /* This is for Limine terminal usage */
            vmm_map(NULL, entry->base, entry->base,
                    NUM_PAGES(entry->length),
                    VMM_FLAGS_DEFAULT | VMM_FLAG_WRITECOMBINE
                    | VMM_FLAGS_USERMODE, false);
            klogd("Mapped framebuffer 0x%9x to 0x%x (len: %d)\n",
                  entry->base, entry->base, entry->length);
#endif
        } else if (entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
#if !LAUNCHER_GRAPHICS
            /* This is for Limine terminal usage */
            vmm_map(NULL, entry->base, entry->base,
                    NUM_PAGES(entry->length),
                    VMM_FLAGS_DEFAULT | VMM_FLAG_WRITECOMBINE
                    | VMM_FLAGS_USERMODE, false);
            klogd("Mapped bootloader reclaimable 0x%9x to 0x%x (len: %d)\n",
                  entry->base, entry->base, entry->length);
#endif
        } else {
            vmm_map(NULL, PHYS_TO_VIRT(entry->base), entry->base,
                    NUM_PAGES(entry->length),
                    VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE, true);
            klogd("Mapped 0x%9x to 0x%x(len: %d)\n",
                  entry->base, PHYS_TO_VIRT(entry->base), entry->length);
        }
    }

    write_cr("cr3", VIRT_TO_PHYS(kaddrspace.PML4));
    klogi("VMM initialization finished\n");
}

addrspace_t *create_addrspace(void)
{
    addrspace_t *as = kmalloc(sizeof(addrspace_t));
    if (!as)
        return NULL;
    memset(as, 0, sizeof(addrspace_t));
    as->PML4 = kmalloc(PAGE_SIZE * 8);
    if (!as->PML4) {
        kmfree(as);
        return NULL;
    } 
    memset(as->PML4, 0, PAGE_SIZE * 8); 
    as->lock = lock_new();

    size_t len = vec_length(&mmap_list);
    for (size_t i = 0; i < len; i++) {
        mem_map_t m = vec_at(&mmap_list, i); 
        vmm_map(as, m.vaddr, m.paddr, m.np, m.flags, false);
    }   

    return as; 
}

