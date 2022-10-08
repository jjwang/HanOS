/**-----------------------------------------------------------------------------

 @file    gdt.h
 @brief   Definition of GDT related data structures
 @details
 @verbatim

  The Global Descriptor Table (GDT) contains entries telling the CPU about
  memory segments.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>
#include <core/smp.h>

#define AC_AC       0x1     /* access */
#define AC_RW       0x2     /* readable for code & writeable for data selector */
#define AC_DC       0x4     /* direction */
#define AC_EX       0x8     /* executable, code segment */
#define AC_ST       0x10    /* Descriptor type bit. If clear (0) the descriptor
                             * defines a system segment (eg. a Task State Segment).
                             * If set (1) it defines a code or data segment
                             */
#define AC_PR       0x80    /* persent in memory, Must be set (1) for any valid
                             * segment.
                             */
#define AC_DPL_KERN 0x0     /* RING 0 kernel level */
#define AC_DPL_USER 0x60    /* RING 3 user level - 01100000b */

#define GDT_GR      0x8     /* Granularity flag, indicates the size the Limit
                             * value is scaled by. If clear (0), the Limit is
                             * in 1 Byte blocks (byte granularity). If set (1),
                             * the Limit is in 4 KiB blocks (page granularity).
                             */
#define GDT_SZ      0x4     /* size bt, 32 bit protect mode */
#define GDT_LM      0x2     /* Long-mode code flag. If set (1), the descriptor
                             * defines a 64-bit code segment. When set, DB
                             * should always be clear. For any other type of
                             * segment (other code types or any data segment),
                             * it should be clear (0).
                             */
/* gdt selector */
#define SEL_KCODE   0x1
#define SEL_KDATA   0x2
#define SEL_UCODE   0x3
#define SEL_UDATA   0x4
#define SEL_TSS     0x5

/* request privilege level */
#define RPL_KERN    0x0
#define RPL_USER    0x3

/* current privilege level */
#define CPL_KERN    0x0
#define CPL_USER    0x3

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
