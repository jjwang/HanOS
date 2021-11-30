///-----------------------------------------------------------------------------
///
/// @file    isr_base.h
/// @brief   Definition of ISR related data structures
/// @details
///
///   The x86 architecture is an interrupt driven system.
///
/// @author  JW
/// @date    Nov 27, 2021
///
///-----------------------------------------------------------------------------
#pragma once

typedef void (*exc_handler_t)();
void exc_register_handler(uint64_t id, exc_handler_t handler);

static inline void isr_enable_interrupts()
{
    asm volatile("sti");
}

static inline void isr_disable_interrupts()
{
    asm volatile("cli");
}

[[gnu::interrupt]] void exc0(void* p);
[[gnu::interrupt]] void exc1(void* p);
[[gnu::interrupt]] void exc2(void* p);
[[gnu::interrupt]] void exc3(void* p);
[[gnu::interrupt]] void exc4(void* p);
[[gnu::interrupt]] void exc5(void* p);
[[gnu::interrupt]] void exc6(void* p);
[[gnu::interrupt]] void exc7(void* p);
[[gnu::interrupt]] void exc8(void* p);
[[gnu::interrupt]] void exc10(void* p);
[[gnu::interrupt]] void exc11(void* p);
[[gnu::interrupt]] void exc12(void* p);
[[gnu::interrupt]] void exc13(void* p);
[[gnu::interrupt]] void exc14(void* p);
[[gnu::interrupt]] void exc16(void* p);
[[gnu::interrupt]] void exc17(void* p);
[[gnu::interrupt]] void exc18(void* p);
[[gnu::interrupt]] void exc19(void* p);
[[gnu::interrupt]] void exc20(void* p);
[[gnu::interrupt]] void exc30(void* p);
