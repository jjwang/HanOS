/**-----------------------------------------------------------------------------

 @file    gdt.h
 @brief   Definition of GDT related data structures
 @details
 @verbatim

  The Global Descriptor Table (GDT) contains entries telling the CPU about
  memory segments.

 @endverbatim
 @author  JW
 @date    Nov 27, 2021

 **-----------------------------------------------------------------------------
 */
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
    uint32_t segment_limit_low  : 16; 
    uint32_t segment_base_low   : 16; 
    uint32_t segment_base_mid   : 8;
    uint32_t segment_type       : 4;
    uint32_t zero_1             : 1;
    uint32_t segment_dpl        : 2;
    uint32_t segment_present    : 1;
    uint32_t segment_limit_high : 4;
    uint32_t segment_avail      : 1;
    uint32_t zero_2             : 2;
    uint32_t segment_gran       : 1;
    uint32_t segment_base_mid2  : 8;
    uint32_t segment_base_high  : 32; 
    uint32_t reserved_1         : 8;
    uint32_t zero_3             : 5;
    uint32_t reserved_2         : 19; 
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
