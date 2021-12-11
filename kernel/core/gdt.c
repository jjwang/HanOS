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
///   used. As the first step, only two ring-0 segment descriptor are defined.
///   The memory regions are all from 0 to 4GB.
///
///   Currently TSS is not initialized.
/// 
/// @author  JW
/// @date    Nov 27, 2021
///
///-----------------------------------------------------------------------------
#include <stdint.h>
#include <lib/klog.h>
#include <core/gdt.h>
#include <core/panic.h>

static gdt_entry_t gdt_make_entry(uint64_t base, uint64_t limit, uint8_t type)
{
    gdt_entry_t gate = 0;
    uint8_t* target = (uint8_t*)&gate;

    if (limit > 65536) {
        // Adjust granularity if required
        limit = limit >> 12;
        target[6] = 0xA0;
    } else {
        target[6] = 0x80;
    }
    // Encode the limit
    target[0] = limit & 0xFF;
    target[1] = (limit >> 8) & 0xFF;
    target[6] |= (limit >> 16) & 0xF;
 
    // Encode the base 
    target[2] = base & 0xFF;
    target[3] = (base >> 8) & 0xFF;
    target[4] = (base >> 16) & 0xFF;
    target[7] = (base >> 24) & 0xFF;
 
    // And... Type
    target[5] = type;

    return gate;
}

void gdt_init(gdt_table_t* gdt)
{
    gdt->null  = gdt_make_entry(0, 0, 0);
    gdt->kcode = gdt_make_entry(0, 0xFFFFFFFF, 0x9A);
    gdt->kdata = gdt_make_entry(0, 0xFFFFFFFF, 0x92);
    gdt->ucode = gdt_make_entry(0, 0xFFFFFFFF, 0xFA);
    gdt->udata = gdt_make_entry(0, 0xFFFFFFFF, 0xF2);

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

    klog_printf("GDT initialization finished.\n");
}

