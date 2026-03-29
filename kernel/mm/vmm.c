/**-----------------------------------------------------------------------------

 @file    vmm.c
 @brief   Implementation of virtual memory management functions
 @details
 @verbatim

  High Half Kernel: To setup a higher half kernel, you have to map your
  kernel to the appropriate virtual address. Without a boot loader help,
  you'll need a small trampoline code which runs in lower half, sets up
  higher half paging and jumps.

  If page protection is not enabled, virtual address is equal with physical
  address. The highest bit of CR0 indicates whether paging is enabled or
  not: mov cr0,8000000 can enable paging.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>

#include <kconfig.h>
#include <libc/string.h>
#include <sys/cpu.h>
#include <sys/panic.h>
#include <mm/mm.h>
#include <base/klog.h>
#include <base/kmalloc.h>
#include <base/klib.h>
#include <base/vector.h>
#include <base/spinlock.h>

#define MAKE_TABLE_ENTRY(address, flags)    ((address & ~(0xfff)) | flags)

extern mem_info_t kmem_info;

addrspace_t kaddrspace = { 0 };

static bool debug_info = false;

vec_new_static(mem_map_t, global_mmap_list);
static lock_t global_mmap_lock = lock_new();

static void map_page(addrspace_t * addrspace, uint64_t vaddr,
                     uint64_t paddr, uint64_t flags)
{
    addrspace_t *as = (addrspace == NULL ? &kaddrspace : addrspace);

    uint16_t pte = (vaddr >> 12) & 0x1ff;
    uint16_t pde = (vaddr >> 21) & 0x1ff;
    uint16_t pdpe = (vaddr >> 30) & 0x1ff;
    uint16_t pml4e = (vaddr >> 39) & 0x1ff;

    uint64_t *pml4 = as->PML4;
    uint64_t *pdpt;
    uint64_t *pd;
    uint64_t *pt;

    pdpt = (uint64_t *) PHYS_TO_VIRT(pml4[pml4e] & ~(0xfff));
    if (!(pml4[pml4e] & VMM_FLAG_PRESENT)) {
        void *buf = (void *) pmm_get(8, 0x0, __func__, __LINE__);
        if (buf == NULL) {
            kpanic("VMM: out of memory for PDPT of PML4 0x%x\n", pml4);
        }
        pdpt = (uint64_t *) PHYS_TO_VIRT(buf);
        memset(pdpt, 0, PAGE_SIZE * 8);
        pml4[pml4e] =
            MAKE_TABLE_ENTRY(VIRT_TO_PHYS(pdpt), VMM_FLAGS_USERMODE);
        vec_push_back(&as->mem_list, VIRT_TO_PHYS(pdpt));
    }

    pd = (uint64_t *) PHYS_TO_VIRT(pdpt[pdpe] & ~(0xfff));
    if (!(pdpt[pdpe] & VMM_FLAG_PRESENT)) {
        void *buf = (void *) pmm_get(8, 0x0, __func__, __LINE__);
        if (buf == NULL) {
            kpanic("VMM: out of memory for PD of PML4 0x%x\n", pml4);
        }
        pd = (uint64_t *) PHYS_TO_VIRT(buf);
        memset(pd, 0, PAGE_SIZE * 8);
        pdpt[pdpe] =
            MAKE_TABLE_ENTRY(VIRT_TO_PHYS(pd), VMM_FLAGS_USERMODE);
        vec_push_back(&as->mem_list, VIRT_TO_PHYS(pd));
    }

    pt = (uint64_t *) PHYS_TO_VIRT(pd[pde] & ~(0xfff));
    if (!(pd[pde] & VMM_FLAG_PRESENT)) {
        void *buf = (void *) pmm_get(8, 0x0, __func__, __LINE__);
        if (buf == NULL) {
            kpanic("VMM: out of memory for PT of PML4 0x%x\n", pml4);
        }
        pt = (uint64_t *) PHYS_TO_VIRT(buf);
        memset(pt, 0, PAGE_SIZE * 8);
        pd[pde] = MAKE_TABLE_ENTRY(VIRT_TO_PHYS(pt), VMM_FLAGS_USERMODE);
        vec_push_back(&as->mem_list, VIRT_TO_PHYS(pt));
    }

    pt[pte] = MAKE_TABLE_ENTRY(paddr & ~(0xfff), flags);

    if (!as->initialized)
        return;

    uint64_t cr3val;
    read_cr("cr3", &cr3val);
    if (cr3val == (uint64_t) (VIRT_TO_PHYS(as->PML4)))
        asm volatile ("invlpg (%0)"::"r" (vaddr));
}

static void unmap_page(addrspace_t * addrspace, uint64_t vaddr)
{
    addrspace_t *as = (addrspace == NULL ? &kaddrspace : addrspace);

    uint16_t pte = (vaddr >> 12) & 0x1ff;
    uint16_t pde = (vaddr >> 21) & 0x1ff;
    uint16_t pdpe = (vaddr >> 30) & 0x1ff;
    uint16_t pml4e = (vaddr >> 39) & 0x1ff;

    uint64_t *pml4 = as->PML4;
    if (!(pml4[pml4e] & VMM_FLAG_PRESENT))
        goto done;

    uint64_t *pdpt = (uint64_t *) PHYS_TO_VIRT(pml4[pml4e] & ~(0x1ff));
    if (!(pdpt[pdpe] & VMM_FLAG_PRESENT))
        goto done;

    uint64_t *pd = (uint64_t *) PHYS_TO_VIRT(pdpt[pdpe] & ~(0x1ff));
    if (!(pd[pde] & VMM_FLAG_PRESENT))
        goto done;

    uint64_t *pt = (uint64_t *) PHYS_TO_VIRT(pd[pde] & ~(0x1ff));
    if (!(pt[pte] & VMM_FLAG_PRESENT))
        goto done;

    pt[pte] = 0;

    if (as->initialized) {
        uint64_t cr3val;
        read_cr("cr3", &cr3val);
        if (cr3val == (uint64_t) (VIRT_TO_PHYS(as->PML4)))
            asm volatile ("invlpg (%0)"::"r" (vaddr));
    }

    for (int i = 0; i < 512 * 8; i++)
        if (pt[i] != 0)
            goto done;

    pd[pde] = 0;
    pmm_free(VIRT_TO_PHYS(pt), 8, __func__, __LINE__);

    int64_t mem_num, i;
    mem_num = vec_length(&as->mem_list);
    for (i = 0; i < mem_num; i++) {
        uint64_t m = vec_at(&as->mem_list, i);
        if (m == VIRT_TO_PHYS(pt)) {
            vec_erase(&as->mem_list, i);
            break;
        }
    }

    for (int i = 0; i < 512 * 8; i++)
        if (pd[i] != 0)
            goto done;

    pdpt[pdpe] = 0;
    pmm_free(VIRT_TO_PHYS(pd), 8, __func__, __LINE__);

    mem_num = vec_length(&as->mem_list);
    for (i = 0; i < mem_num; i++) {
        uint64_t m = vec_at(&as->mem_list, i);
        if (m == VIRT_TO_PHYS(pd)) {
            vec_erase(&as->mem_list, i);
            break;
        }
    }
    for (int i = 0; i < 512 * 8; i++)
        if (pdpt[i] != 0)
            goto done;

    pml4[pml4e] = 0;
    pmm_free(VIRT_TO_PHYS(pdpt), 8, __func__, __LINE__);

    mem_num = vec_length(&as->mem_list);
    for (i = 0; i < mem_num; i++) {
        uint64_t m = vec_at(&as->mem_list, i);
        if (m == VIRT_TO_PHYS(pdpt)) {
            vec_erase(&as->mem_list, i);
            break;
        }
    }

  done:
    return;
}

uint64_t vmm_get_paddr(addrspace_t * addrspace, uint64_t vaddr)
{
    addrspace_t *as = (addrspace == NULL ? &kaddrspace : addrspace);

    uint16_t pte = (vaddr >> 12) & 0x1ff;
    uint16_t pde = (vaddr >> 21) & 0x1ff;
    uint16_t pdpe = (vaddr >> 30) & 0x1ff;
    uint16_t pml4e = (vaddr >> 39) & 0x1ff;

    uint64_t *pml4 = as->PML4;
    if (!(pml4[pml4e] & VMM_FLAG_PRESENT))
        return (uint64_t) NULL;

    uint64_t *pdpt = (uint64_t *) PHYS_TO_VIRT(pml4[pml4e] & ~(0x1ff));
    if (!(pdpt[pdpe] & VMM_FLAG_PRESENT))
        return (uint64_t) NULL;

    uint64_t *pd = (uint64_t *) PHYS_TO_VIRT(pdpt[pdpe] & ~(0x1ff));
    if (!(pd[pde] & VMM_FLAG_PRESENT))
        return (uint64_t) NULL;

    uint64_t *pt = (uint64_t *) PHYS_TO_VIRT(pd[pde] & ~(0x1ff));
    if (!(pt[pte] & VMM_FLAG_PRESENT))
        return (uint64_t) NULL;

    return (pt[pte] & 0xFFFFFFFFFFFFF000);
}

void vmm_unmap(addrspace_t * addrspace, uint64_t vaddr, uint64_t np)
{
    if (addrspace == NULL) {
        /* We must unmap the corresponding vaddr in vmm_map() function */
        lock_lock(&global_mmap_lock);
        int64_t len = vec_length(&global_mmap_list);
        for (int64_t i = 0; i < len; i++) {
            mem_map_t m = vec_at(&global_mmap_list, i);
            if (m.vaddr == vaddr) {
                vec_erase(&global_mmap_list, i);
                break;
            }
        }
        lock_release(&global_mmap_lock);
    }

    for (uint64_t i = 0; i < np * PAGE_SIZE; i += PAGE_SIZE)
        unmap_page(addrspace, vaddr + i);

    if (debug_info) {
        klogd("VMM: PML4 0x%x un-mapped virt 0x%x (%d pages)\n",
              (addrspace == NULL ? kaddrspace.PML4 : addrspace->PML4),
              vaddr, np);
    }
}

void vmm_map(addrspace_t * addrspace, uint64_t vaddr, uint64_t paddr,
             uint64_t np, uint64_t flags)
{
    if (addrspace == NULL) {
        mem_map_t mm = {
            .vaddr = vaddr,.paddr = paddr,.flags = flags,.np = np
        };
        lock_lock(&global_mmap_lock);
        vec_push_back(&global_mmap_list, mm);
        lock_release(&global_mmap_lock);
    }

    for (uint64_t i = 0; i < np * PAGE_SIZE; i += PAGE_SIZE) {
        map_page(addrspace, vaddr + i, paddr + i, flags);
    }

    if (debug_info) {
        klogd("VMM: PML4 0x%x mapped phys 0x%x to virt 0x%x (%d pages)\n",
              (addrspace == NULL ? kaddrspace.PML4 : addrspace->PML4),
              paddr, vaddr, np);
    }
}

void vmm_init(struct limine_memmap_response *map,
              struct limine_kernel_address_response *kernel)
{
    kaddrspace.PML4 =
        (void *) PHYS_TO_VIRT(pmm_get(8, 0x0, __func__, __LINE__));
    klogd("VMM: PML4 of kernel address space - 0x%x\n", kaddrspace.PML4);
    memset(kaddrspace.PML4, 0, PAGE_SIZE * 8);

    /* We only need to map all memories as below for kernel task, so we do not
     * call vmm_map() function.
     *
     * - For ENABLE_MEM_DEBUG definition
     *
     * For memory debuging purpose, we totally map 1GB memory for all tasks
     * to access these memories. If we map all physical memories, there will be
     * #PF (page fault) exception when forking 2 or more tasks.
     * 
     * - For UEFI mode
     *
     * But we also open this memory region map to resolve #PF exception when
     * booting from UEFI mode.
     *
     * TODO: need to locate the root cause of UEFI booting issue.
     *
     */
    uint64_t np = NUM_PAGES(kmem_info.phys_limit);
    for (uint64_t i = 0; i < np * PAGE_SIZE; i += PAGE_SIZE) {
        map_page(NULL, MEM_VIRT_OFFSET + i, i, VMM_FLAGS_DEFAULT);
    }
    klogi("Mapped %d bytes memory to 0x%x\n",
          kmem_info.phys_limit, MEM_VIRT_OFFSET);

    for (uint64_t i = 0; i < map->entry_count; i++) {
        struct limine_memmap_entry *entry = map->entries[i];

        if (entry->type == LIMINE_MEMMAP_KERNEL_AND_MODULES) {
            uint64_t vaddr = kernel->virtual_base
                + entry->base - kernel->physical_base;
            /* vmm_map: this should share for all tasks */
            vmm_map(NULL, vaddr, entry->base, NUM_PAGES(entry->length),
                    VMM_FLAGS_DEFAULT);
            klogi("[K] Mapped kernel 0x%9x to 0x%x (len: %d, #%d)\n",
                  entry->base, vaddr, entry->length, i);
        } else if (entry->type == LIMINE_MEMMAP_FRAMEBUFFER) {
            /* vmm_map: this should share for all tasks */
            vmm_map(NULL, PHYS_TO_VIRT(entry->base), entry->base,
                    NUM_PAGES(entry->length), VMM_FLAGS_DEFAULT);
            klogi("[F] Mapped framebuffer 0x%9x to 0x%x (len: %d, #%d)\n",
                  entry->base, PHYS_TO_VIRT(entry->base), entry->length,
                  i);
        } else if (entry->type == LIMINE_MEMMAP_BOOTLOADER_RECLAIMABLE) {
            /* vmm_map: do nothing */
        } else if (entry->type == LIMINE_MEMMAP_USABLE) {
            bool is_mem_bitmap_loc = false;
            if (VIRT_TO_PHYS(kmem_info.bitmap) >= entry->base
                && VIRT_TO_PHYS(kmem_info.bitmap) <
                entry->base + entry->length) {
                is_mem_bitmap_loc = true;
            }
            vmm_map(NULL, PHYS_TO_VIRT(entry->base), entry->base,
                    NUM_PAGES(entry->length), VMM_FLAGS_DEFAULT);
            klogi("[U] Mapped 0x%9x to 0x%x(len: %d, type %d, #%d, %s)\n",
                  entry->base, PHYS_TO_VIRT(entry->base), entry->length,
                  entry->type, i,
                  is_mem_bitmap_loc ? "all tasks [bitmap]" :
                  "kernel only");
        }
    }

    kaddrspace.initialized = true;
    write_cr("cr3", VIRT_TO_PHYS(kaddrspace.PML4));
    klogi("VMM initialization finished\n");
}

addrspace_t *create_addrspace(void)
{
    klogd("VMM: create a new address space\n");

    addrspace_t *as = kmalloc(sizeof(addrspace_t));

    if (!as) {
        kpanic("VMM: cannot allocate addrspace\n");
        return NULL;
    }

    memset(as, 0, sizeof(addrspace_t));

    as->PML4 = kmalloc_chunk(PAGE_SIZE * 8, __func__, __LINE__);
    if (!as->PML4) {
        kmfree(as);
        return NULL;
    }

    memset(as->PML4, 0, PAGE_SIZE * 8);

    as->lock = lock_new();

    lock_lock(&global_mmap_lock);
    uint64_t len = vec_length(&global_mmap_list);
    for (uint64_t i = 0; i < len; i++) {
        mem_map_t m = vec_at(&global_mmap_list, i);
        vmm_map(as, m.vaddr, m.paddr, m.np, m.flags);
    }
    lock_release(&global_mmap_lock);

    as->initialized = true;
    klogd("VMM: creating address space 0x%x finished\n", as);

    return as;
}
