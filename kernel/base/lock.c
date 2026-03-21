/**-----------------------------------------------------------------------------

 @file    lock.c
 @brief   Implementation of spin lock related functions
 @details
 @verbatim

  This file implements a simple and efficient spinlock for HanOS, designed for
  x86 multi-core (SMP) systems. It uses atomic instructions to ensure thread
  safety and collects lock statistics for performance analysis.

  Key features:
  - Simple spinlock with atomic xchg
  - Support for statistics: lock acquire count, total hold time, spin fail count
  - No interrupt disabling (for SMP safety and simplicity)

 @endverbatim

 **-----------------------------------------------------------------------------
 */

#include <stdatomic.h>
#include <libc/numeric.h>

#include <base/lock.h>
#include <base/klog.h>
#include <sys/hpet.h>
#include <fs/vfs.h>
#include <proc/sched.h>

#if SPINLOCK_DEBUG
#include <sys/serial.h>
#include <libc/printf.h>
#endif /* SPINLOCK_DEBUG */

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
 * Conclusion (Aug 2025): The spinlock implementation needs improvement.
 *
 * Analysis:
 *   The average lock hold time on real hardware is much higher than in
 *   QEMU. This indicates excessive lock contention or overly long critical
 *   sections, leading to degraded system performance on physical machines.
 *   It is recommended to optimize the spinlock design, reduce the scope of
 *   locked code, and consider switching to fine-grained locking or per-CPU
 *   queues with work-stealing to alleviate contention.
 */

volatile uint64_t total_lock_acquire_count = 0;
volatile uint64_t total_lock_hold_time_ns = 0;
volatile uint64_t total_spin_fail_count = 0;
volatile uint64_t max_lock_hold_time_ns = 0;
char max_lock_hold_fn[VFS_MAX_PATH_LEN] = {0};
volatile uint64_t max_lock_hold_ln = 0;

bool lock_lock_impl(lock_t *s, bool waiting, const char *fn, const int ln)
{
    (void) fn;
    (void) ln;

    uint64_t lock_start = hpet_get_nanos();
#if SPINLOCK_DEBUG
    int lockid = rand(sched_get_ticks() % 1000, 1, 1000);
    bool errmsg_displayed = false;
    uint16_t cpu_id = smp_get_current_cpu_id();
#endif

    /* Save interrupt state and disable interrupts (CLI) */
    asm volatile (
        "pushfq\n"          /* Push RFLAGS register to stack (save IF bit) */
        "pop %0\n"          /* Pop stack value to 'flags' (preserve original state) */
        "cli\n"             /* Clear Interrupt Flag (disable maskable interrupts) */
        : "=r" (s->rflags)  /* Output operand: %0 maps to 'flags' (write-only register) */
        :                   /* No input operands */
        : "memory"          /* Tell compiler memory may be modified (prevent optimization) */
    );

    /* Atomic CAS spin to acquire lock (lock cmpxchg under the hood) */
    while (true) {
        if (!__sync_bool_compare_and_swap(&(s->lock), 0, 1)) {
            total_spin_fail_count++;
            asm volatile("pause" : : : "memory");
            if (!waiting) return false;
#if SPINLOCK_DEBUG
            if (!errmsg_displayed) {
                char errmsg[1024] = {0};
                task_t *t = sched_get_current_task();
                sprintf(errmsg, "lock_lock: lock #%d failed in %s:%d with "
                        "lock value %d and tid %d @ CPU %d, "
                        "last succ in %s:%d with tid %d @ CPU %d\n",
                        lockid, fn, ln, s->lock, (t != NULL) ? t->tid : 0,
                        cpu_id, s->last_fn, s->last_ln, s->last_tid,
                        s->last_cpu_id);
                serial_puts(errmsg);
                errmsg_displayed = true;
            }
#endif
        } else {
#if SPINLOCK_DEBUG
            task_t *t = sched_get_current_task();
            if (t != NULL) s->last_tid = t->tid;
            s->last_cpu_id = cpu_id;
            sprintf((char*)s->last_fn, "%s", fn);
            s->last_ln = ln;
#endif
            break;
        }
    }

#if SPINLOCK_DEBUG
    if (errmsg_displayed) {
        char errmsg[1024] = {0};
        uint64_t lock_acquired = hpet_get_nanos();
        sprintf(errmsg, "lock_lock: lock #%d successed in %s:%d with %d, "
                "time consumption is %d nano seconds\n",
                lockid, fn, ln, s->lock, lock_acquired - lock_start);
        serial_puts(errmsg);
    }
#endif

    total_lock_acquire_count++; /* Count every lock acquire attempt */

    /* Store timestamp in lock struct for later use */
    s->acquire_time = lock_start;

    asm volatile ("mfence" : : : "memory");

    return true;
}

void lock_release_impl(lock_t * s, const char *fn, const int ln)
{
    (void) fn;
    (void) ln;

    uint64_t lock_end = hpet_get_nanos();
    if (s->acquire_time != 0) {
        uint64_t hold_time = lock_end - s->acquire_time;
        total_lock_hold_time_ns += hold_time;
        s->acquire_time = 0;
        if (hold_time > max_lock_hold_time_ns) {
            max_lock_hold_time_ns = hold_time;
            strcpy(max_lock_hold_fn, fn);
            max_lock_hold_ln = ln;
        }
    }

    /* Memory fence to prevent critical section reordering */
    asm volatile ("mfence" : : : "memory");

    /* Unlock the spinlock (reset to free state) */
    s->lock = 0;

    /* Restore original interrupt state (critical: use popfq instead of sti!) */
    asm volatile (
        "push %0\n"         /* Push saved flags to stack */
        "popfq\n"           /* Restore RFLAGS (including original IF bit state) */
        :                   /* No output operands */
        : "r" (s->rflags)   /* Input operand: %0 maps to 'flags' (saved interrupt state) */
        : "memory", "cc"    /* Notify compiler: memory/condition codes modified */
    );
}
