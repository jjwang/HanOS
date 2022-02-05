///-----------------------------------------------------------------------------
///
/// @file    gdt.h
/// @brief   Definition of GDT related data structures
/// @details
///
///   The Global Descriptor Table (GDT) contains entries telling the CPU about
///   memory segments.
///
/// @author  JW
/// @date    Nov 27, 2021
///
///-----------------------------------------------------------------------------
#pragma once

#include <stdint.h>
#include <core/smp.h>

typedef struct [[gnu::packed]] {
    uint16_t limit;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  granularity;
    uint8_t  base_high;
} gdt_entry_t;

typedef struct [[gnu::packed]] {
    uint16_t seg_limit_1;
    uint16_t base_addr_1;
    uint8_t  base_addr_2;
    uint8_t  flags_low;
    uint8_t  flags_high;
    uint8_t  base_addr_3;
    uint32_t base_addr_4;
    uint32_t reserved;
} sys_seg_desc_t;

typedef struct [[gnu::packed]] {
    gdt_entry_t null;
    gdt_entry_t kcode;
    gdt_entry_t kdata;
    gdt_entry_t ucode;
    gdt_entry_t udata;
    sys_seg_desc_t tss;
} gdt_table_t;

typedef struct [[gnu::packed]] {
    uint16_t size;
    uint64_t offset;
} gdt_register_t;

void gdt_init(cpu_t* cpuinfo);
void gdt_install_tss(cpu_t* cpuinfo);
