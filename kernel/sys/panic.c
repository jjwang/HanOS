/**-----------------------------------------------------------------------------

 @file    panic.h
 @brief   Implementation of panic related functions
 @details
 @verbatim

  A kernel panic is one of several boot issues. In basic terms, it is a
  situation when the kernel can't load properly and therefore the system
  fails to boot.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>
#include <symbols.h>
#include <sys/panic.h>
#include <sys/smp.h>
#include <lib/klog.h>
#include <device/display/term.h>

static int symbols_get_index(uint64_t addr)
{
    for (int i = 0; _kernel_symtab[i].addr < UINT64_MAX; i++)
        if (_kernel_symtab[i].addr < addr && _kernel_symtab[i + 1].addr >= addr)
            return i;

    return -1;
}

void dump_backtrace()
{
    if (term_get_mode() != TERM_MODE_INFO) {
        term_switch(TERM_MODE_INFO);
    }

    uint64_t* rbp_val = 0;
    asm volatile ("mov %%rbp, %0" : "=g"(rbp_val) :: "memory");

    klog_lock();

    klogu("\nStacktrace:\n");
    for (size_t i = 0; ; i++) {
        uint64_t func_addr = *(rbp_val + 1);
        rbp_val = (uint64_t*)*rbp_val;
        if (func_addr == (uint64_t)NULL || rbp_val == NULL) 
        {
            break;
        }
        int idx = symbols_get_index(func_addr);
        if (idx < 0) {
            klogu(" \t[%02d] \t%x (Unknown Function)\n", i, func_addr);
        } else { 
            klogu(" \t[%02d] \t%x (%s+%04x)\n",
                    i, func_addr,
                    _kernel_symtab[idx].name,
                    func_addr - _kernel_symtab[idx].addr);
        }
    }

    cpu_t* cpu = smp_get_current_cpu(false);
    if (cpu != NULL) {
        klogu("End of trace. CPU %d System halted.\n \n \n", cpu->cpu_id);
    } else {
        klogu("End of trace. System halted.\n \n \n");
    }

    klog_unlock();
}

