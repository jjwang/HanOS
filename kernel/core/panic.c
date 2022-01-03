///-----------------------------------------------------------------------------
///
/// @file    panic.h
/// @brief   Implementation of panic related functions
/// @details
///
///   A kernel panic is one of several boot issues. In basic terms, it is a
///   situation when the kernel can't load properly and therefore the system
///   fails to boot.
///
/// @author  JW
/// @date    Nov 27, 2021
///
///-----------------------------------------------------------------------------
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <symbols.h>
#include <core/panic.h>
#include <lib/klog.h>

static int symbols_get_index(uint64_t addr)
{
    for (int i = 0; _kernel_symtab[i].addr < UINT64_MAX; i++)
        if (_kernel_symtab[i].addr < addr && _kernel_symtab[i + 1].addr >= addr)
            return i;

    return -1;
}

void dump_backtrace()
{
    uint64_t* rbp_val = 0;
    asm volatile("movq %%rbp, %0" : "=rm"(rbp_val));

    klogu("\nStacktrace:\n");
    for (int i = 0;; i++) {
        uint64_t func_addr = *(rbp_val + 1);
        if (func_addr == 0x0) {
            break;
        }
        klogu(" \t[%02d] ", i); 
        int idx = symbols_get_index(func_addr);
        klogu("\t%x", func_addr);
        if (idx < 0) {
            klogu(" (Unknown Function)\n");
            break;
        }   
        klogu(" (%s+%04x)\n",
                    _kernel_symtab[idx].name,
                    func_addr - _kernel_symtab[idx].addr);
        rbp_val = (uint64_t*)*rbp_val;
    }
    klogu("End of trace. System halted.\n");
}

