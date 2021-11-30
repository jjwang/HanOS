///-----------------------------------------------------------------------------
///
/// @file    symbols.h
/// @brief   Definition of symbol related data structures
/// @details
///
///   Symbols are used for backtrace when kernel is crashed. It can help to
///   provide context information for debugging.
///
/// @author  JW
/// @date    Nov 27, 2021
///
///-----------------------------------------------------------------------------
#pragma once

#include <stdint.h>

typedef struct {
    uint64_t addr;
    char* name;
} symbol_t;

extern const symbol_t _kernel_symtab[];
