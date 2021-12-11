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

static void dump_backtrace()
{
    uint64_t* rbp_val = 0;
    asm volatile("movq %%rbp, %0" : "=rm"(rbp_val));

    klog_printf("\nStacktrace:\n");
    for (int i = 0;; i++) {
        uint64_t func_addr = *(rbp_val + 1);
        if (func_addr == 0x0) {
            break;
        }
        klog_printf(" \t[%02d] ", i); 
        int idx = symbols_get_index(func_addr);
        klog_printf("\t%x", func_addr);
        if (idx < 0) {
            klog_printf(" (Unknown Function)");
            break;
        }   
        klog_printf(" (%s+%04x)\n",
                    _kernel_symtab[idx].name,
                    func_addr - _kernel_symtab[idx].addr);
        rbp_val = (uint64_t*)*rbp_val;
    }
    klog_printf("End of trace. System halted.\n");
}

_Noreturn void kpanic(const char* s, ...)
{
    asm volatile("cli");

    klog_printf("\077[11m[PANIC] \077[10;1m");

    va_list args;
    va_start(args, s);
    klog_vprintf(s, args);
    va_end(args);

    klog_printf("\077[0m");
    dump_backtrace();

    while (true)
        asm volatile("hlt");
}
