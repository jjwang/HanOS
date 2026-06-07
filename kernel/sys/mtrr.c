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
#include <mm/mm.h>

uint64_t *saved_mtrrs = NULL;

void mtrr_save(uint16_t cpu_id, void *framebuffer)
{
    if (!cpuid_check_feature(CPUID_FEATURE_MTRR)) {
        return;
    }

    uint64_t ia32_mtrrcap = read_msr(0xfe);

    uint8_t var_reg_count = ia32_mtrrcap & 0xff;
    klogd("CPU %ld: variable register count is %ld\n", cpu_id,
          var_reg_count);

    if (saved_mtrrs == NULL) {
        saved_mtrrs = (uint64_t *) kmalloc(((var_reg_count * 2) /* variable MTRRs, 2 MSRs each */
                                            +11 /* 11 fixed MTRRs */
                                            + 1 /* 1 default type MTRR */
                                           ) * sizeof(uint64_t));
        klogd("CPU %ld: save mtrrs to 0x%016lx\n", cpu_id, saved_mtrrs);
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
                ("CPU %ld: variable MTRR #%ld - type %ld, %s, mask base 0x%016lx,"
                 " target 0x%016lx\n", cpu_id, i / 2, type,
                 (valid ? "valid" : "invalid"), phys_base & phys_mask,
                 mask_target);
            if ((phys_base & phys_mask) == (uint64_t) framebuffer) {
                klogw("MTRR: set framebuffer 0x%016lx to WRITE COMBINING\n",
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
    klogi("CPU %ld: MTRR %s, fixed range %s, MTRR default type is %ld\n",
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
        kpanic("CPU %ld: Attempted restore MTRR without prior save\n",
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

__attribute__((noinline)) void fb_set_wc(uint64_t fb_phys, uint64_t fb_size)
{
    if (!cpuid_check_feature(CPUID_FEATURE_PAT)) {
        return;
    }

    /* Dump MTRR state for the framebuffer region and try to set WC */
    int mtrr_fb_idx = -1;
    if (cpuid_check_feature(CPUID_FEATURE_MTRR)) {
        uint64_t mtrrcap = read_msr(0xfe);
        uint64_t mtrrdef = read_msr(0x2ff);
        uint8_t vcnt = mtrrcap & 0xff;
        bool enabled = (mtrrdef >> 11) & 1;
        bool fenabled = (mtrrdef >> 10) & 1;
        uint8_t deftype = mtrrdef & 0xff;

        klogi("FB_WC: MTRR %s, fixed %s, default type %u, %u variable ranges\n",
              enabled ? "EN" : "DIS", fenabled ? "EN" : "DIS", deftype, vcnt);

        uint64_t fb_end = fb_phys + fb_size - 1;
        for (uint8_t i = 0; i < vcnt; i++) {
            uint64_t physbase = read_msr(0x200 + i * 2);
            uint64_t physmask = read_msr(0x200 + i * 2 + 1);
            bool valid = (physmask >> 11) & 1;
            if (!valid) continue;

            uint64_t base = physbase & 0x000ffffffffff000;
            uint64_t mask = physmask & 0x000ffffffffff000;
            uint8_t type = physbase & 0xff;

            /* MTRR covers all X where (X & mask) == (base & mask).
             * Check overlap: does any FB address match the MTRR pattern? */
            bool overlaps = ((fb_phys & mask) == (base & mask)) ||
                            ((fb_end  & mask) == (base & mask)) ||
                            ((base & mask) >= fb_phys && (base & mask) <= fb_end);

            if (overlaps) {
                uint64_t var_bits = ~mask & 0x000ffffffffff000;
                uint64_t rstart = base & mask;
                uint64_t rend   = rstart | var_bits;
                klogi("FB_WC: MTRR #%u type=%u base=0x%016lx mask=0x%016lx"
                      " [0x%016lx-0x%016lx] overlaps FB\n",
                      i, type, base, mask, rstart, rend);

                /* If this MTRR covers the entire FB with UC, note it */
                if (type == MTRR_CACHE_UNCACHEABLE && mtrr_fb_idx < 0) {
                    if ((fb_phys & mask) == (base & mask) &&
                        (fb_end  & mask) == (base & mask)) {
                        mtrr_fb_idx = i;
                    }
                }
            }
        }

        /* Try to change the first UC MTRR covering FB to WC */
        if (mtrr_fb_idx >= 0 && enabled) {
            klogi("FB_WC: Attempting to change MTRR #%d from UC to WC\n",
                  mtrr_fb_idx);

            /* Step 1: save CR0, set CD, clear NW, flush caches */
            uintptr_t old_cr0;
            asm volatile ("mov %%cr0, %0":"=r" (old_cr0)::"memory");
            uintptr_t new_cr0 = (old_cr0 | (1 << 30)) & ~((uintptr_t) 1 << 29);
            asm volatile ("mov %0, %%cr0"::"r" (new_cr0):"memory");
            asm volatile ("wbinvd":::"memory");

            /* Step 2: flush TLB */
            uintptr_t cr3;
            asm volatile ("mov %%cr3, %0":"=r" (cr3)::"memory");
            asm volatile ("mov %0, %%cr3"::"r" (cr3):"memory");

            /* Step 3: disable MTRRs */
            mtrrdef &= ~((uint64_t) 1 << 11);
            write_msr(0x2ff, mtrrdef);

            /* Step 4: read current PHYSBASE, change type to WC, write back */
            uint64_t physbase = read_msr(0x200 + mtrr_fb_idx * 2);
            physbase = (physbase & ~0xffULL) | MTRR_CACHE_WRITE_COMBINING;
            write_msr(0x200 + mtrr_fb_idx * 2, physbase);

            /* Step 5: re-enable MTRRs */
            mtrrdef |= ((uint64_t) 1 << 11);
            write_msr(0x2ff, mtrrdef);

            /* Step 6: flush TLB and caches again */
            asm volatile ("mov %%cr3, %0":"=r" (cr3)::"memory");
            asm volatile ("mov %0, %%cr3"::"r" (cr3):"memory");
            asm volatile ("wbinvd":::"memory");

            /* Step 7: restore CR0 */
            asm volatile ("mov %0, %%cr0"::"r" (old_cr0):"memory");

            /* Verify */
            physbase = read_msr(0x200 + mtrr_fb_idx * 2);
            uint8_t new_type = physbase & 0xff;
            klogi("FB_WC: MTRR #%d type now %s (expected WC=%d)\n",
                  mtrr_fb_idx,
                  new_type == MTRR_CACHE_WRITE_COMBINING ? "WC" : "FAILED",
                  MTRR_CACHE_WRITE_COMBINING);
        } else {
            klogi("FB_WC: No suitable MTRR to change (idx=%d, enabled=%d)\n",
                  mtrr_fb_idx, enabled);
        }
    } else {
        klogi("FB_WC: CPU lacks MTRR support\n");
    }

    uint64_t pat = read_msr(MSR_PAT);
    klogi("FB_WC: PAT current 0x%016lx\n", pat);
    pat &= ~((uint64_t) 0xFF << 16);
    pat |= ((uint64_t) MTRR_CACHE_WRITE_COMBINING << 16);
    write_msr(MSR_PAT, pat);
    klogi("FB_WC: PAT index2=WC (0x%016lx)\n", pat);

    klogi("FB_WC: framebuffer phys=0x%016lx size=%ld\n", fb_phys, fb_size);

}
