/**-----------------------------------------------------------------------------

 @file    panic.h
 @brief   Definition of panic related data structures
 @details
 @verbatim

  A kernel panic is one of several boot issues. In basic terms, it is a
  situation when the kernel can't load properly and therefore the system
  fails to boot.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdarg.h>

#include <base/klog.h>

void display_backtrace();
void dump_backtrace();

#define kpanic(s, ...)       {                  \
    asm volatile("cli");                        \
    klog_vprintf(KLOG_LEVEL_ERROR, "Enter kernel panic...\n"); \
    klog_vprintf(KLOG_LEVEL_ERROR, s, ##__VA_ARGS__); \
    display_backtrace();                        \
    while (true)                                \
        asm volatile("hlt");                    \
}

#define panic_unless(c) ({ \
    if(!(c)) \
        kpanic("panic_unless(" #c ") triggered in " \
              "%s:%d", __FILE__, __LINE__);     \
})

#define panic_if(c) ({ \
    if((c)) \
        kpanic("panic_if(" #c ") triggered in " \
              "%s:%d", __FILE__, __LINE__); \
})
