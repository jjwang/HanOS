/**-----------------------------------------------------------------------------

 @file    hpet.c
 @brief   Implementation of HPET (High Precision Event Timer) functions
 @details
 @verbatim

  This module includes implementation of HPET initialization, getting nanos
  and nanos sleep.

  HPET consists of (usually 64-bit) main counter (which counts up), as well
  as from 3 to 32 32-bit or 64-bit wide comparators. HPET is programmed
  using memory mapped IO, and the base address of HPET can be found using
  ACPI.

  General initialization:
    1. Find HPET base address in 'HPET' ACPI table.
    2. Calculate HPET frequency (f = 10^15 / period).
    3. Save minimal tick (either from ACPI table or configuration register).
    4. Initialize comparators.
    5. Set ENABLE_CNF bit.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <lib/klog.h>
#include <lib/time.h>
#include <core/hpet.h>
#include <core/acpi.h>
#include <core/mm.h>
#include <core/pit.h>
#include <core/panic.h>
#include <proc/sched.h>

static hpet_t* hpet = NULL;
static uint64_t hpet_period = 0;

uint64_t hpet_get_nanos()
{
    if(hpet == NULL) {
        return pit_get_ticks();
    }

    uint64_t tf = hpet->main_counter_value * hpet_period;
    return tf;
}

uint64_t hpet_get_millis()
{
    return hpet_get_nanos() / MILLIS_TO_NANOS(1); 
}

void hpet_nanosleep(uint64_t nanos)
{
    uint64_t stt = hpet_get_nanos();
    uint64_t tgt = hpet_get_nanos() + nanos;
    while (true) {
        uint64_t cur = hpet_get_nanos();
        if (cur >= tgt) break;
        if (cur <= stt) {
            break;
        }
        asm volatile ("nop;");
    }
}

void hpet_init()
{
    /* Find the HPET description table */
    hpet_sdt_t* hpet_sdt = (hpet_sdt_t*)acpi_get_sdt("HPET");
    if (!hpet_sdt) {
        kpanic("HPET not found\n");
        return;
    }
    hpet = (hpet_t *)PHYS_TO_VIRT(hpet_sdt->base_addr.address);
    vmm_map(NULL, (uint64_t)hpet, (uint64_t)hpet_sdt->base_addr.address,
            1, VMM_FLAGS_MMIO, true);

    uint64_t tmp = hpet->general_capabilities;
    /* Check that the HPET is valid or not */
    if (!(tmp & (1 << 15))) {
        kloge("HPET is not legacy replacement capable\n");
        hpet = NULL;
        return;
    }

    /* Calculate HPET frequency (f = 10^15 / period) */
    uint64_t counter_clk_period = tmp >> 32;
    uint64_t frequency = 1000000000000000 / counter_clk_period;

    klogi("HPET: Detected frequency of %d Hz\n", frequency);
    hpet_period = counter_clk_period / 1000000;

    /* Set ENABLE_CNF bit */
    hpet->general_configuration = hpet->general_configuration | 0b01;

    klogi("HPET initialization finished\n");
}

