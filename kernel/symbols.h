/**-----------------------------------------------------------------------------

 @file    symbols.h
 @brief   Definition of symbol related data structures
 @details
 @verbatim

  Symbols are used for backtrace when kernel is crashed. It can help to
  provide context information for debugging.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>

typedef struct {
    uint64_t addr;
    char* name;
} symbol_t;

extern const symbol_t _kernel_symtab[];
