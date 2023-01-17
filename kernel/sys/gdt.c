/**-----------------------------------------------------------------------------

 @file    gdt.c
 @brief   Implementation of GDT related functions
 @details
 @verbatim

  The Global Descriptor Table (GDT) contains entries telling the CPU about
  memory segments.

  In HanOS, GDT initialization is very simple. Only memory protection is
  used. Two ring-0 and two ring-3 segment descriptor are defined. The
  memory regions are from 0 to 4GB.

  Jan 14, 2023: To make sure that we can call write() in limine bootloader, the
                initializaton function is refined according to limine protocol.
 
 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <lib/klog.h>
#include <lib/memutils.h>
#include <sys/gdt.h>
#include <sys/panic.h>
#include <sys/smp.h>

static gdt_table_t gdt_list[CPU_MAX] = {0};
static size_t gdt_num = 0;

static void gdt_make_entry(
    gdt_entry_t* gate,
    uint64_t base,
    uint64_t limit,
    uint8_t type)
{
    if (limit > 65536) {
        /* Adjust granularity if required */
        limit = limit >> 12;
        gate->granularity = (GDT_GR | GDT_LM) << 4;
    } else {
        gate->granularity = GDT_GR << 4;
    }
    /* Encode the limit */
    gate->limit = limit & 0xFFFF;
    gate->granularity |= (limit >> 16) & 0xF;
 
    /* Encode the base */
    gate->base_low  = base & 0xFFFF;
    gate->base_mid  = (base >> 16) & 0xFF;
    gate->base_high = (base >> 24) & 0xFF;
 
    /* Encode the type */
    gate->access = type;
}

void gdt_init(cpu_t* cpuinfo)
{
    /* GDT table should be allocated for each CPU separately */
    gdt_table_t* gdt = (gdt_table_t*)&gdt_list[gdt_num++];
    memset(gdt, 0, sizeof(gdt_table_t));

    gdt_make_entry(&(gdt->null), 0, 0, 0);

    gdt_make_entry(&(gdt->kcode16), 0, 0xFFFF,
        AC_RW | AC_EX | AC_PR | AC_ST);
    gdt->kcode16.granularity = 0;

    gdt_make_entry(&(gdt->kdata16), 0, 0xFFFF,
        AC_RW | AC_PR | AC_ST);
    gdt->kdata16.granularity = 0;

    gdt_make_entry(&(gdt->kcode32), 0, 0xFFFFFFFF,
        AC_RW | AC_EX | AC_PR | AC_ST);
    gdt->kcode32.granularity &= 0x0F;
    gdt->kcode32.granularity |= (GDT_GR | GDT_SZ) << 4;

    gdt_make_entry(&(gdt->kdata32), 0, 0xFFFFFFFF,
        AC_RW | AC_PR | AC_ST);
    gdt->kdata32.granularity &= 0x0F;
    gdt->kdata32.granularity |= (GDT_GR | GDT_SZ) << 4;

    gdt_make_entry(&(gdt->kcode64), 0, 0xFFFFFFFF,
        AC_RW | AC_EX | AC_DPL_KERN | AC_PR | AC_ST);

    gdt_make_entry(&(gdt->kdata64), 0, 0xFFFFFFFF,
        AC_RW | AC_DPL_KERN | AC_PR | AC_ST);

    gdt_make_entry(&(gdt->ucode64), 0, 0xFFFFFFFF,
        AC_RW | AC_EX | AC_DPL_USER | AC_PR | AC_ST);

    gdt_make_entry(&(gdt->udata64), 0, 0xFFFFFFFF,
        AC_RW | AC_DPL_USER | AC_PR | AC_ST);

    gdt_register_t g = { .offset = (uint64_t)gdt, .size = sizeof(gdt_table_t) - 1 };

    asm volatile("lgdt %0;"
                 "push $0x28;"
                 "lea 1f(%%rip), %%rax;"
                 "push %%rax;"
                 "lretq;"
                 "1:"
                 "mov $0x30, %%eax;"
                 "mov %%eax, %%ds;"
                 "mov %%eax, %%es;"
                 "mov %%eax, %%ss;"
                 "mov %%eax, %%fs;"
                 "mov %%eax, %%gs;"
                 :   
                 : "g"(g)
                 :); 

    if (cpuinfo != NULL) {
        klogi("GDT: initialization finished for CPU %d\n", cpuinfo->cpu_id);
    } else {
        klogi("GDT 0x%x initialization finished\n", (uint64_t)gdt);
    }
}

void gdt_install_tss(cpu_t* cpuinfo)
{
    gdt_register_t gdtr;
    asm volatile("sgdt %0"
                 :
                 : "m"(gdtr)
                 : "memory");

    gdt_table_t* gt = (gdt_table_t*)(gdtr.offset);
    uint64_t baseaddr = (uint64_t)(&cpuinfo->tss);

    gt->tss.segment_base_low = baseaddr & 0xFFFF;
    gt->tss.segment_base_mid = (baseaddr >> 16) & 0xFF;
    gt->tss.segment_base_mid2 = (baseaddr >> 24) & 0xFF;
    gt->tss.segment_base_high = (baseaddr >> 32) & 0xFFFFFFFF;
    gt->tss.segment_limit_low = 0x67;
    gt->tss.segment_present = 1;
    gt->tss.segment_type = 0b1001;

    klogv("GDT: load TSS with base address 0x%x\n", baseaddr);

    /* Loading of TSS: The descriptor of the TSS in the GDT (e.g. 0x48 if
     * the tenths entry in your GDT describes your TSS)
     */
    asm volatile("ltr %%ax"
                 :
                 : "a"(0x48));

    if (cpuinfo != NULL) {
        klogi("GDT: finish loading TSS for CPU %d, base addr 0x%x\n",
              cpuinfo->cpu_id, baseaddr);
    } else {
        klogi("GDT: finish loading TSS, base addr 0x%x\n", baseaddr);
    } 
}

