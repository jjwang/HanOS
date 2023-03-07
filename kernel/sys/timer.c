/**-----------------------------------------------------------------------------

 @file    timer.c
 @brief   Implementation of APIC timer related functions
 @details
 @verbatim

  The timer has 2 or 3 modes. The first 2 modes (periodic and one-shot)
  are supported by all local APICs. The third mode (TSC-Deadline mode) is
  an extension that is only supported on recent CPUs.

  Periodic Mode:
  - Generate IRQs at a fixed rate depending on the initial count.

  One-Shot Mode:
  - Decrement the current count (and generates a timer IRQ when the count
    reaches zero) in the same way as in periodic mode. However it doesn't
    reset the current count to the initial count when the current count
    reaches zero.

  TSC-Deadline mode:
  - Similar with one-shot mode but using CPU's time stamp counter instead
    to get higher precision.

 @endverbatim
   Ref: https://wiki.osdev.org/APIC_timer

 **-----------------------------------------------------------------------------
 */
#include <sys/timer.h>
#include <sys/apic.h>
#include <sys/idt.h>
#include <sys/pit.h>
#include <lib/klog.h>
#include <lib/time.h>

static uint64_t base_freq = 0;
static uint8_t divisor = 0;
static uint8_t vector = 0;

[[gnu::interrupt]] void apic_timer_handler(void* v);

void apic_timer_stop(void)
{
    uint32_t val = apic_read_reg(APIC_REG_TIMER_LVT);
    apic_write_reg(APIC_REG_TIMER_LVT, val | APIC_TIMER_FLAG_MASKED);
}

void apic_timer_start(void)
{
    uint32_t val = apic_read_reg(APIC_REG_TIMER_LVT);
    apic_write_reg(APIC_REG_TIMER_LVT, val & ~(APIC_TIMER_FLAG_MASKED));
}

void apic_timer_set_handler(void (*h)(void*))
{
    idt_set_handler(vector, h);
}

void apic_timer_set_frequency(uint64_t freq)
{
    apic_write_reg(APIC_REG_TIMER_ICR, base_freq / (freq * divisor));
}

void apic_timer_set_period(time_t tv)
{
    uint64_t freq = 1000000000 / tv;
    klogv("APIC timer's frequency is %dHz.\n", freq);
    apic_timer_set_frequency(freq);
}

uint8_t apic_timer_get_vector(void)
{
    return vector;
}

void apic_timer_set_mode(apic_timer_mode_t mode)
{
    uint32_t val = apic_read_reg(APIC_REG_TIMER_LVT);

    if(mode == APIC_TIMER_MODE_PERIODIC)
        apic_write_reg(APIC_REG_TIMER_LVT, val | APIC_TIMER_FLAG_PERIODIC);
    else
        apic_write_reg(APIC_REG_TIMER_LVT, val & ~(APIC_TIMER_FLAG_PERIODIC));
}

void apic_timer_enable(void)
{
    apic_write_reg(APIC_REG_TIMER_LVT, APIC_TIMER_FLAG_MASKED | vector);
    apic_write_reg(APIC_REG_TIMER_ICR, UINT32_MAX);
    apic_write_reg(APIC_REG_TIMER_DCR, 0b0001);
}

void apic_timer_init(void)
{
    vector = idt_get_available_vector();
    idt_set_handler(vector, &apic_timer_handler);

    apic_write_reg(APIC_REG_TIMER_LVT, APIC_TIMER_FLAG_MASKED | vector);
    apic_write_reg(APIC_REG_TIMER_DCR, 0b0001);
    divisor = 4;

    apic_write_reg(APIC_REG_TIMER_ICR, UINT32_MAX);

    /* If we do not sleep enough time, the whole system will halt when
     * running in QEMU-KVM mode.
     */
    hpet_sleep(50);

    base_freq = ((UINT32_MAX - apic_read_reg(APIC_REG_TIMER_CCR)) * 2) * divisor;

    klogi("APIC timer base frequency: %d Hz. Divisor: 4. IRQ %d.\n",
          base_freq, vector);
}

