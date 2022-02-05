///-----------------------------------------------------------------------------
///
/// @file    isr.c
/// @brief   Implementation of ISR related functions
/// @details
///
///   The x86 architecture is an interrupt driven system. Only a common
///   interrupt handling function is implemented.
///
/// @author  JW
/// @date    Nov 27, 2021
///
///-----------------------------------------------------------------------------
#include <stdbool.h>
#include <stdint.h>
#include <lib/klog.h>
#include <core/isr_base.h>
#include <core/panic.h>

static char* exceptions[] = {
    [0] = "Division by Zero",
    [1] = "Debug",
    [2] = "Non Maskable Interrupt",
    [3] = "Breakpoint",
    [4] = "Overflow",
    [5] = "Bound Range Exceeded",
    [6] = "Invalid opcode",
    [7] = "Device Not Available",
    [8] = "Double Fault",
    [10] = "Invalid TSS",
    [11] = "Segment Not Present",
    [12] = "Stack Exception",
    [13] = "General Protection Fault",
    [14] = "Page Fault",
    [16] = "x87 Floating Point Exception",
    [17] = "Alignment Check",
    [18] = "Machine Check",
    [19] = "SIMD Floating Point Exception",
    [20] = "Virtualization Exception",
    [30] = "Security Exception",
    [32] = "Reserved",
    [33] = "Reserved",
    [34] = "Reserved",
    [35] = "Reserved",
    [36] = "Reserved",
    [37] = "Reserved",
    [38] = "Reserved",
    [39] = "Reserved",
    [40] = "Reserved",
    [41] = "Reserved",
    [42] = "Reserved",
    [43] = "Reserved",
    [44] = "Reserved"
};

void exc_handler_proc(uint64_t errcode, uint64_t excno)
{
    kpanic("Unhandled Exception: %s (%d). Error Code: %d.\n",
                 exceptions[excno], excno, errcode);
    while (true)
        ;
}
