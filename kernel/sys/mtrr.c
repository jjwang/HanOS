/**-----------------------------------------------------------------------------

 @file    mtrr.c
 @brief   MTRR configuration to speed up memory reading & writing operations
 @details
 @verbatim

   MTRR, or Memory-Type Range Registers are a group of x86 Model Specific
   Registers providing a way to control access and cacheability of physical
   memory regions. These were introduced in the Intel Pentium Pro (P6) processor,
   intended to extend and enhance the memory type information provided by page
   tables (i.e. the write-through and cache disable bits).

   The set of registers is provided in two groups: 11 registers for 88 fixed
   ranges and a number of base-mask pairs for custom range configuration. The
   exact number of the latter can be known by reading the capabilities register.

   There are 5 memory types defined for use in MTRRs:

   Number  Name                 Description
   ======  ====                 ===========
   0       UC — Uncacheable     All accesses are uncacheable. Write combining is
                                not allowed. Speculative accesses are not
                                allowed.
   1       WC — Write-Combining All accesses are uncacheable. Write combining is
                                allowed. Speculative reads are allowed.
   4       WT — Writethrough    Reads allocate cache lines on a cache miss.
                                Cache lines are not allocated on a write miss.
                                Write hits update the cache and main memory.
   5       WP — Write-Protect   Reads allocate cache lines on a cache miss. All
                                writes update main memory. Cache lines are not
                                allocated on a write miss. Write hits invalidate
                                the cache line and update main memory.
   6       WB — Writeback       Reads allocate cache lines on a cache miss, and
                                can allocate to either the shared, exclusive,
                                or modified state. Writes allocate to the
                                modified state on a cache miss.

   Reference: https://wiki.osdev.org/MTRR

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <libc/string.h>

#include <sys/cpu.h>
#include <sys/mtrr.h>
#include <sys/panic.h>
#include <base/klog.h>
#include <base/kmalloc.h>

uint64_t *saved_mtrrs = NULL;

void mtrr_save(uint16_t cpu_id, void *framebuffer)
{
    if (!cpuid_check_feature(CPUID_FEATURE_MTRR)) {
        return;
    }

    uint64_t ia32_mtrrcap = read_msr(0xfe);

    uint8_t var_reg_count = ia32_mtrrcap & 0xff;
    klogd("CPU %d: variable register count is %d\n", cpu_id,
          var_reg_count);

    if (saved_mtrrs == NULL) {
        saved_mtrrs = (uint64_t *) kmalloc(((var_reg_count * 2) /* variable MTRRs, 2 MSRs each */
                                            +11 /* 11 fixed MTRRs */
                                            + 1 /* 1 default type MTRR */
                                           ) * sizeof(uint64_t));
        klogd("CPU %d: save mtrrs to 0x%x\n", cpu_id, saved_mtrrs);
    }

    /* save variable range MTRRs */
    for (uint8_t i = 0; i < var_reg_count * 2; i += 2) {
        saved_mtrrs[i] = read_msr(0x200 + i);
        saved_mtrrs[i + 1] = read_msr(0x200 + i + 1);

        uint64_t ia32_mttrphysbase = saved_mtrrs[i];
        uint64_t ia32_mttrphysmask = saved_mtrrs[i + 1];
        bool valid = (ia32_mttrphysmask & 0x800) ? true : false;
        uint64_t phys_base = ia32_mttrphysbase & 0x000FFFFFFFFFF000;
        uint64_t phys_mask = ia32_mttrphysmask & 0x000FFFFFFFFFF000;
        uint8_t type = ia32_mttrphysbase & 0xFF;
        uint64_t mask_target = phys_mask & 0xB0000000;
        if (valid) {
            klogi
                ("CPU %d: variable MTRR #%d - type %d, %s, mask base 0x%x,"
                 " target 0x%x\n", cpu_id, i / 2, type,
                 (valid ? "valid" : "invalid"), phys_base & phys_mask,
                 mask_target);
            if ((phys_base & phys_mask) == (uint64_t) framebuffer) {
                klogw("MTRR: set framebuffer 0x%x to WRITE COMBINING\n",
                      framebuffer);
                saved_mtrrs[i] = saved_mtrrs[i] & 0xFFFFFFFFFFFFF000;
                saved_mtrrs[i] |= MTRR_CACHE_WRITE_COMBINING;
            }
        }
    }

    /* save fixed range MTRRs */
    saved_mtrrs[var_reg_count * 2 + 0] = read_msr(0x250);
    saved_mtrrs[var_reg_count * 2 + 1] = read_msr(0x258);
    saved_mtrrs[var_reg_count * 2 + 2] = read_msr(0x259);
    saved_mtrrs[var_reg_count * 2 + 3] = read_msr(0x268);
    saved_mtrrs[var_reg_count * 2 + 4] = read_msr(0x269);
    saved_mtrrs[var_reg_count * 2 + 5] = read_msr(0x26a);
    saved_mtrrs[var_reg_count * 2 + 6] = read_msr(0x26b);
    saved_mtrrs[var_reg_count * 2 + 7] = read_msr(0x26c);
    saved_mtrrs[var_reg_count * 2 + 8] = read_msr(0x26d);
    saved_mtrrs[var_reg_count * 2 + 9] = read_msr(0x26e);
    saved_mtrrs[var_reg_count * 2 + 10] = read_msr(0x26f);

    /* save MTRR default type: describe the default type used for physical
     * addresses which are outside of any configured memory ranges, as well as
     * whether MTRRs and fixed ranges are enabled.
     *      Bit(s)  Label   Description
     *      ======  =====   ===========
     *      11      E       MTRR enable
     *      10      FE      Fixed range enable
     *      7..0    Type    Default memory type
     */
    uint64_t ia32_mtrrdeftype = read_msr(0x2ff);
    saved_mtrrs[var_reg_count * 2 + 11] = ia32_mtrrdeftype;
    bool enable = (ia32_mtrrdeftype & 0x800) ? true : false;
    bool fenable = (ia32_mtrrdeftype & 0x400) ? true : false;
    uint8_t deftype = ia32_mtrrdeftype & 0xFF;
    klogi("CPU %d: MTRR %s, fixed range %s, MTRR default type is %d\n",
          cpu_id, (enable ? "enabled" : "disabled"),
          (fenable ? "enabled" : "disabled"), deftype);

    /* make sure that the saved MTRR default has MTRRs off */
    saved_mtrrs[var_reg_count * 2 + 11] &= ~((uint64_t) 1 << 11);
}

void mtrr_restore(uint16_t cpu_id)
{
    if (!cpuid_check_feature(CPUID_FEATURE_MTRR)) {
        return;
    }

    uint64_t ia32_mtrrcap = read_msr(0xfe);
    uint8_t var_reg_count = ia32_mtrrcap & 0xff;

    if (saved_mtrrs == NULL) {
        kpanic("CPU %d: Attempted restore MTRR without prior save\n",
               cpu_id);
    }

    /* according to the Intel SDM 12.11.7.2 "MemTypeSet() Function",
       we need to follow this precedure before changing MTRR set up */

    /* save old cr0 and then enable the CD flag and disable the NW flag */
    uintptr_t old_cr0;
    asm volatile ("mov %%cr0, %0":"=r" (old_cr0)::"memory");
    uintptr_t new_cr0 = (old_cr0 | (1 << 30)) & ~((uintptr_t) 1 << 29);
    asm volatile ("mov %0, %%cr0"::"r" (new_cr0):"memory");

    /* then invalidate the caches */
    asm volatile ("wbinvd":::"memory");

    /* do a cr3 read/write to flush the TLB */
    uintptr_t cr3;
    asm volatile ("mov %%cr3, %0":"=r" (cr3)::"memory");
    asm volatile ("mov %0, %%cr3"::"r" (cr3):"memory");

    /* disable the MTRRs */
    uint64_t mtrr_def = read_msr(0x2ff);
    mtrr_def &= ~((uint64_t) 1 << 11);
    write_msr(0x2ff, mtrr_def);

    /* restore variable range MTRRs */
    for (uint8_t i = 0; i < var_reg_count * 2; i += 2) {
        write_msr(0x200 + i, saved_mtrrs[i]);
        write_msr(0x200 + i + 1, saved_mtrrs[i + 1]);
    }

    /* restore fixed range MTRRs */
    write_msr(0x250, saved_mtrrs[var_reg_count * 2 + 0]);
    write_msr(0x258, saved_mtrrs[var_reg_count * 2 + 1]);
    write_msr(0x259, saved_mtrrs[var_reg_count * 2 + 2]);
    write_msr(0x268, saved_mtrrs[var_reg_count * 2 + 3]);
    write_msr(0x269, saved_mtrrs[var_reg_count * 2 + 4]);
    write_msr(0x26a, saved_mtrrs[var_reg_count * 2 + 5]);
    write_msr(0x26b, saved_mtrrs[var_reg_count * 2 + 6]);
    write_msr(0x26c, saved_mtrrs[var_reg_count * 2 + 7]);
    write_msr(0x26d, saved_mtrrs[var_reg_count * 2 + 8]);
    write_msr(0x26e, saved_mtrrs[var_reg_count * 2 + 9]);
    write_msr(0x26f, saved_mtrrs[var_reg_count * 2 + 10]);

    /* restore MTRR default type */
    write_msr(0x2ff, saved_mtrrs[var_reg_count * 2 + 11]);

    /* now do the opposite of the cache disable and flush from above */

    /* re-enable MTRRs */
    mtrr_def = read_msr(0x2ff);
    mtrr_def |= (1 << 11);
    write_msr(0x2ff, mtrr_def);

    /* do a cr3 read/write to flush the TLB */
    asm volatile ("mov %%cr3, %0":"=r" (cr3)::"memory");
    asm volatile ("mov %0, %%cr3"::"r" (cr3):"memory");

    /* then invalidate the caches */
    asm volatile ("wbinvd":::"memory");

    /* restore old value of cr0 */
    asm volatile ("mov %0, %%cr0"::"r" (old_cr0):"memory");
}
