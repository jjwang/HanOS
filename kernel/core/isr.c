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
    [7] = "Device not available",
    [8] = "Double Fault",
    [10] = "Invalid TSS",
    [11] = "Segment not present",
    [12] = "Stack Exception",
    [13] = "General Protection fault",
    [14] = "Page fault",
    [16] = "x87 Floating Point Exception",
    [17] = "Alignment check",
    [18] = "Machine check",
    [19] = "SIMD floating point Exception",
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

static exc_handler_t handlers[256] = { 0 };

void exc_register_handler(uint64_t id, exc_handler_t handler)
{
    handlers[id] = handler;
}

void exc_handler_proc(uint64_t errcode, uint64_t isrno)
{
    exc_handler_t handler = handlers[isrno];

    if (handler != 0) {
        handler();
        return;
    }

    kpanic("Unhandled Exception: %s (%d). Error Code: %d.\n",
                 exceptions[isrno], isrno, errcode);
    while (true)
        ;
}
