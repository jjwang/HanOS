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

void lock_lock_impl(lock_t * s, const char *fn, const int ln)
{
    (void) fn;
    (void) ln;

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
}

void lock_release_impl(lock_t * s, const char *fn, const int ln)
{
    (void) fn;
    (void) ln;

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
