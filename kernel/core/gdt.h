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

typedef uint64_t gdt_entry_t;

typedef struct [[gnu::packed]] {
    gdt_entry_t null;
    gdt_entry_t kcode;
    gdt_entry_t kdata;
} gdt_table_t;

typedef struct [[gnu::packed]] {
    uint16_t size;
    uint64_t offset;
} gdt_register_t;

void gdt_init(gdt_table_t* gdt);

