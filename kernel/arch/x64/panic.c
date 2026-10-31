/**-----------------------------------------------------------------------------

 @file    panic.c
 @brief   Implementation of panic related functions
 @details
 @verbatim

  A kernel panic is one of several boot issues. In basic terms, it is a
  situation when the kernel can't load properly and therefore the system
  fails to boot.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#include <symbols.h>
#include <arch/x64/panic.h>
#include <arch/x64/smp.h>
#include <arch/x64/serial.h>
#include <lib/klog.h>

#include <printf.h>

static int32_t symbols_get_index(uint64_t addr)
{
    for (uint64_t i = 0; _kernel_symtab[i].addr < UINT64_MAX; i++)
        if (_kernel_symtab[i].addr < addr
            && _kernel_symtab[i + 1].addr >= addr)
            return i;

    return -1;
}

_Noreturn void kpanic_impl(const char *fmt, ...)
{
    char buf[256];
    va_list args;

    va_start(args, fmt);
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    asm volatile ("cli");
    klog_vprintf(KLOG_LEVEL_ERROR, "Enter kernel panic...\n");
    klog_vprintf(KLOG_LEVEL_ERROR, "%s", buf);
    display_backtrace();
    for (;;)
        asm volatile ("hlt");
}

void display_backtrace()
{
    uint64_t *rbp_val = 0;
    asm volatile ("mov %%rbp, %0":"=g" (rbp_val)::"memory");

    klogu("\nStacktrace:\n");
    for (uint64_t i = 0;; i++) {
        uint64_t func_addr = *(rbp_val + 1);
        rbp_val = (uint64_t *) * rbp_val;
        if (func_addr == (uint64_t) NULL || rbp_val == NULL) {
            break;
        }
        int32_t idx = symbols_get_index(func_addr);
        if (idx < 0) {
            klogu(" \t[%02d] \t%016lx (Unknown Function)\n", i, func_addr);
        } else {
            klogu(" \t[%02d] \t%016lx (%s+%04lx)\n",
                  i, func_addr,
                  _kernel_symtab[idx].name,
                  func_addr - _kernel_symtab[idx].addr);
        }
    }

    cpu_t *cpu = smp_get_current_cpu(false);
    if (cpu != NULL) {
        klogu("End of trace. CPU %ld System halted.\n \n \n", cpu->cpu_id);
    } else {
        klogu("End of trace. System halted.\n \n \n");
    }
}

void dump_backtrace()
{
    char errmsg[1024] = {0};

    uint64_t *rbp_val = 0;
    asm volatile ("mov %%rbp, %0":"=g" (rbp_val)::"memory");

    serial_puts("\nStacktrace:\n");
    for (uint64_t i = 0;; i++) {
        uint64_t func_addr = *(rbp_val + 1); 
        rbp_val = (uint64_t *) * rbp_val;
        if (func_addr == (uint64_t) NULL || rbp_val == NULL) {
            break;
        }
        int32_t idx = symbols_get_index(func_addr);
        if (idx < 0) {
            snprintf(errmsg, sizeof(errmsg), " \t[%02d] \t%016lx (Unknown Function)\n", i, func_addr);
        } else {
            snprintf(errmsg, sizeof(errmsg), " \t[%02d] \t%016lx (%s+%04lx)\n",
                    i, func_addr,
                    _kernel_symtab[idx].name,
                    func_addr - _kernel_symtab[idx].addr);
        }
        serial_puts(errmsg);
    }

    snprintf(errmsg, sizeof(errmsg), "End of trace. CPU %ld System halted.\n\n\n",
            smp_get_current_cpu_id());
    serial_puts(errmsg);
}

