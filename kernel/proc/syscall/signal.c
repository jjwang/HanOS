/**-----------------------------------------------------------------------------

 @file    signal.c
 @brief   Signal system calls

 @details
 @verbatim

  Applies signal masks and actions, validates signal targets, records the
  clear-child-tid address and restores a signal frame on sigreturn.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>
#include <errno.h>

#include <arch/x64/cpu.h>
#include <arch/x64/smp.h>
#include <mm/uaccess.h>
#include <lib/klog.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/signal.h>
#include <proc/syscall.h>
#include <proc/syscall/syscall_internal.h>

int64_t k_sigprocmask(int64_t how, sigset_t * set, sigset_t * oldset)
{
    process_t *t = sched_get_current_process();
    if (t == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    cpu_set_errno(0);

    sigset_t new, old;

    if (set != NULL) {
        if (how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) {
            cpu_set_errno(EINVAL);
            return -1;
        }
        if (copy_from_user(&new, set, sizeof(sigset_t)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
    }

    klogd("k_sigprocmask: how %ld from old 0x%016lx to new 0x%016lx\n",
          how, oldset, set);

    signal_changemask(t, how, set ? &new : NULL, oldset ? &old : NULL);

    if (oldset != NULL) {
        if (copy_to_user(oldset, &old, sizeof(sigset_t)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
    }

    return 0;
}

int64_t k_sigaction(int64_t s, sigaction_t * new, sigaction_t * old)
{
    process_t *t = sched_get_current_process();
    int64_t signal = (int64_t) ((int32_t) s);

    cpu_set_errno(0);

    if (signal >= NSIG || signal < 0 || signal == SIGKILL
        || signal == SIGSTOP || t == NULL) {
        klogd("k_sigaction: signal %ld from old 0x%016lx to new 0x%016lx "
              "and return -1 for invalid parameters\n", signal, old, new);
        cpu_set_errno(EINVAL);
        return -1;
    }

    sigaction_t newtmp = { 0 }, oldtmp = { 0 };
    if (new != NULL) {
        if (copy_from_user(&newtmp, new, sizeof(sigaction_t)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }

        if ((newtmp.flags & SA_RESTORER) == 0) {
            /* How to handle? cpu_set_errno(EINVAL) */
        }
    }

    klogd("k_sigaction: signal %ld from old 0x%016lx to new 0x%016lx\n",
          signal, old, new);

    signal_action(t, signal, new ? &newtmp : NULL, old ? &oldtmp : NULL);

    if (old != NULL) {
        if (copy_to_user(old, &oldtmp, sizeof(sigaction_t)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
    }

    return 0;
}

int64_t k_kill(int32_t pid, int32_t sig)
{
    process_t *t;

    cpu_set_errno(0);

    if (pid == 0)
        t = sched_get_current_process();
    else if (pid > 0)
        t = process_lookup((pid_t) pid);
    else {
        cpu_set_errno(EINVAL);
        return -1;
    }

    if (t == NULL) {
        cpu_set_errno(ESRCH);
        return -1;
    }
    if (sig == 0)
        return 0;
    if (sig < 1 || sig >= NSIG) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    signal_raise(t, sig);
    return 0;
}

int64_t k_tkill(int32_t tid, int32_t sig)
{
    return k_kill(tid, sig);
}

int64_t k_tgkill(int32_t tgid, int32_t tid, int32_t sig)
{
    (void) tgid;
    return k_kill(tid, sig);
}

int64_t k_set_tid_address(int32_t * tidptr)
{
    process_t *t = sched_get_current_process();

    cpu_set_errno(0);

    if (t == NULL) {
        cpu_set_errno(ESRCH);
        return -1;
    }

    t->clear_child_tid = tidptr;
    return (int64_t) t->pid;
}

int64_t k_sigreturn(void)
{
    process_t *t = sched_get_current_process();
    cpu_t *cpu = smp_get_current_cpu(false);
    int64_t ret;

    cpu_set_errno(0);

    if (t == NULL || cpu == NULL || cpu->syscall_frame == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    if (signal_restore(t, (syscall_regs_t *) cpu->syscall_frame, &ret) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    return ret;
}

/* Called from the syscall stub after the handler. Gives a queued signal the
 * chance to rewrite the return frame before it is restored to user mode. */

