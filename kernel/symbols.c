/* 
 * This file is a placeholder
 * DO NOT EDIT
 */

/**-----------------------------------------------------------------------------

 @file    symbols.c
 @brief   Weak fallback kernel symbol table replaced by generated data at link time.

 **-----------------------------------------------------------------------------
 */
#include <symbols.h>

[[gnu::weak]]
const symbol_t _kernel_symtab[] = {
    {.addr = UINT64_MAX,.name = "" }
};
