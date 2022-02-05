///-----------------------------------------------------------------------------
///
/// @file    gdt.c
/// @brief   Implementation of GDT related functions
/// @details
///
///   The Global Descriptor Table (GDT) contains entries telling the CPU about
///   memory segments.
///
///   In HanOS, GDT initialization is very simple. Only memory protection is
///   used. Two ring-0 and two ring-3 segment descriptor are defined. The
///   memory regions are from 0 to 4GB.
///
/// @author  JW
/// @date    Nov 27, 2021
///
///-----------------------------------------------------------------------------
#include <stdint.h>
#include <lib/klog.h>
#include <lib/kmalloc.h>
#include <core/gdt.h>
#include <core/panic.h>
#include <core/smp.h>

gdt_table_t* gdt = NULL;

static void gdt_make_entry(
    gdt_entry_t* gate,
    uint64_t base,
    uint64_t limit,
    uint8_t type)
{
    if (limit > 65536) {
        // Adjust granularity if required
        limit = limit >> 12;
        gate->granularity = 0xA0;
    } else {
        gate->granularity = 0x80;
    }
    // Encode the limit
    gate->limit = limit & 0xFFFF;
    gate->granularity|= (limit >> 16) & 0xF;
 
    // Encode the base 
    gate->base_low  = base & 0xFFFF;
    gate->base_mid  = (base >> 16) & 0xFF;
    gate->base_high = (base >> 24) & 0xFF;
 
    // Encode the type
    gate->access = type;
}

void gdt_init(cpu_t* cpuinfo)
{
    if(gdt == NULL) {
        gdt = (gdt_table_t*)kmalloc(sizeof(gdt_table_t));
    }

    gdt_make_entry(&(gdt->null), 0, 0, 0);
    gdt_make_entry(&(gdt->kcode), 0, 0xFFFFFFFF, 0x9A);
    gdt_make_entry(&(gdt->kdata), 0, 0xFFFFFFFF, 0x92);
    gdt_make_entry(&(gdt->ucode), 0, 0xFFFFFFFF, 0xFA);
    gdt_make_entry(&(gdt->udata), 0, 0xFFFFFFFF, 0xF2);

    gdt_register_t g = { .offset = (uint64_t)gdt, .size = sizeof(gdt_table_t) - 1 };
    asm volatile("lgdt %0;"
                 "pushq $0x08;"
                 "pushq $reload_sr;"
                 "lretq;"
                 "reload_sr:"
                 "movw $0x10, %%ax;"
                 "movw %%ax, %%ds;"
                 "movw %%ax, %%es;"
                 "movw %%ax, %%ss;"
                 "movw %%ax, %%fs;"
                 "movw %%ax, %%gs;"
                 :
                 : "g"(g)
                 :);

    if (cpuinfo != NULL) {
        klogi("GDT initialization finished for CPU %d\n", cpuinfo->cpu_id);
    } else {
        klogi("GDT initialization finished\n");
    }
}

void gdt_install_tss(cpu_t* cpuinfo)
{
    gdt_register_t gdtr;
    asm volatile("sgdt %0"
                 :
                 : "m"(gdtr)
                 : "memory");

    gdt_table_t* gdt = (gdt_table_t*)(gdtr.offset);
    uint64_t baseaddr = (uint64_t)(&cpuinfo->tss);
    uint64_t seglimit = sizeof(tss_t);

    gdt->tss.base_addr_1 = baseaddr & 0xFFFF;
    gdt->tss.base_addr_2 = (baseaddr >> 16) & 0xFF;
    gdt->tss.base_addr_3 = (baseaddr >> 24) & 0xFF;
    gdt->tss.base_addr_4 = baseaddr >> 32;
    gdt->tss.seg_limit_1 = seglimit & 0xFFFF;
    gdt->tss.flags_low = 0x89;
    gdt->tss.flags_high = 0;

    // Loading of TSS: The descriptor of the TSS in the GDT (e.g. 0x28 if
    // the sixths entry in your GDT describes your TSS)
    asm volatile("mov $0x28, %%ax;"
                 "ltr %%ax"
                 :
                 :
                 : "ax");

    if (cpuinfo != NULL) {
        klogi("Loading TSS finished for CPU %d, base address 0x%x\n", cpuinfo->cpu_id, baseaddr);
    } else {
        klogi("Loading TSS finished, base address 0x%x\n", baseaddr);
    } 
}

