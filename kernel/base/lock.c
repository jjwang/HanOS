/**-----------------------------------------------------------------------------

 @file    lock.c
 @brief   Implementation of spin lock related functions
 @details
 @verbatim

  There are a few atomic operations on the x86 processor that set and compare
  memory or registers, that can be used as the basis of a spin lock.
  See below implementation for details.

  Note that when any task acquires spin lock, the system interrupts will be
  disabled. When this task releases the corresponding spin lock, interrupts
  will be enabled. So if some tasks acquire spin lock twice, it will block task
  scheduler and system will fall into DEAD busy loop.

 @endverbatim

 **-----------------------------------------------------------------------------
 */


#include <base/lock.h>
#include <base/klog.h>
#include <sys/hpet.h>

/* Global statistics variables (should be atomic if multi-core)
 *
 * Launch OS and execute "ls" command, LOCK-STATS reports as below:
 *
 * On real HW (NEC VersaPro):
 *   acquired 116059 times, total hold time 13465382770 ns (13465 ms),
 *   avg hold time 116021 ns
 * On QEMU:
 *   acquired 117357 times, total hold time 3227881650 ns (3227 ms),
 *   avg hold time 27504 ns
 *
 * Conclusion in Aug 2025: Need to improve implementation of spinlock.
 */

volatile uint64_t total_lock_acquire_count = 0;
volatile uint64_t total_lock_hold_time_ns = 0;

void lock_lock_impl(lock_t * s, const char *fn, const int ln)
{
    (void) fn;
    (void) ln;

    uint64_t lock_start = hpet_get_nanos();
    total_lock_acquire_count++; /* Count every lock acquire attempt */

    asm volatile ("pushfq;" "cli;" "lock;"      /* Make the next instruction atomic */
                  "btsl $0, %[lock];"   /* The Bit Test and Set Long (btsl): the Carry Flag
                                         * (CF) is set if the value of the 0th bit of
                                         * register operand %[lock] before the instruction
                                         * executes is 1, and in any case sets the 0th bit
                                         * of %[lock] to 1. Here, $0 denotes an immediate 
                                         * operand with value zero. */
                  "jnc 2f;"     /* Jump if Carry Flag is not set (zero) */
                  "1:"          /* Loop to check %[lock] */
                  "pause;" "btl $0, %[lock];" "jc 1b;" "lock;" "btsl $0, %[lock];" "jc 1b;" "2:"        /* Get the lock which is free */
                  "pop %[flags]":[lock] "=m"((s)->lock),
                  [flags] "=m"((s)->rflags)
                  ::"memory", "cc");

    /* Store timestamp in lock struct for later use */
    s->timestamp = lock_start;
}

void lock_release_impl(lock_t * s, const char *fn, const int ln)
{
    (void) fn;
    (void) ln;

    uint64_t lock_end = hpet_get_nanos();
    if (s->timestamp != 0) {
        uint64_t hold_time = lock_end - s->timestamp;
        total_lock_hold_time_ns += hold_time;
        s->timestamp = 0;
    }

    /* The below Bit Test and Reset Long (btrl) instruction stores the value
     * of the zeroth bit of the operand into the CF flag, and clears the bit
     * in the memory operand.
     */
    asm volatile ("push %[flags];"
                  "lock;"
                  "btrl $0, %[lock];" "popfq;":[lock] "=m"((s)->lock)
                  :[flags] "m"((s)->rflags)
                  :"memory", "cc");
}
