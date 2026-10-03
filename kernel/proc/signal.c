/**-----------------------------------------------------------------------------

 @file    signal.c
 @brief   Implementation of signal handling functions
 @details
 @verbatim

  This file contains the implementation of functions required to handle signals
  within the HanOS kernel. It includes functions to set signal actions, change
  signal masks, and manage the default actions for various signals. The signal
  handling mechanism allows processes to handle asynchronous events such as interrupts,
  exceptions, and inter-process communication.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <lib/klog.h>
#include <lib/kmalloc.h>
#include <mm/uaccess.h>
#include <proc/signal.h>
#include <proc/process.h>
#include <proc/sched.h>

int32_t signal_defaultactions[NSIG] = {
    [SIGABRT] = SIG_ACTION_CORE,
    [SIGALRM] = SIG_ACTION_TERM,
    [SIGBUS] = SIG_ACTION_CORE,
    [SIGCHLD] = SIG_ACTION_IGN,
    [SIGCONT] = SIG_ACTION_CONT,
    [SIGFPE] = SIG_ACTION_CORE,
    [SIGHUP] = SIG_ACTION_TERM,
    [SIGILL] = SIG_ACTION_CORE,
    [SIGINT] = SIG_ACTION_TERM,
    [SIGKILL] = SIG_ACTION_TERM,
    [SIGPIPE] = SIG_ACTION_TERM,
    [SIGPOLL] = SIG_ACTION_TERM,
    [SIGPROF] = SIG_ACTION_TERM,
    [SIGPWR] = SIG_ACTION_TERM,
    [SIGQUIT] = SIG_ACTION_CORE,
    [SIGSEGV] = SIG_ACTION_CORE,
    [SIGSTOP] = SIG_ACTION_STOP,
    [SIGTSTP] = SIG_ACTION_STOP,
    [SIGSYS] = SIG_ACTION_CORE,
    [SIGTERM] = SIG_ACTION_TERM,
    [SIGTRAP] = SIG_ACTION_CORE,
    [SIGTTIN] = SIG_ACTION_STOP,
    [SIGTTOU] = SIG_ACTION_STOP,
    [SIGURG] = SIG_ACTION_IGN,
    [SIGUSR1] = SIG_ACTION_TERM,
    [SIGUSR2] = SIG_ACTION_TERM,
    [SIGVTALRM] = SIG_ACTION_TERM,
    [SIGXCPU] = SIG_ACTION_CORE,
    [SIGXFSZ] = SIG_ACTION_CORE,
    [SIGWINCH] = SIG_ACTION_IGN
};

void signal_action(process_t * t, int64_t signal, sigaction_t * new,
                   sigaction_t * old)
{
    if (!((signal) < NSIG && (signal) >= 0))
        return;
    if (t == NULL)
        return;

    spinlock_acquire(&t->signals.lock);

    if (old != NULL) {
        memcpy(old, &(t->signals.actions[signal]), sizeof(sigaction_t));
    }

    if (new != NULL) {
        memcpy(&(t->signals.actions[signal]), new, sizeof(sigaction_t));
    }

    spinlock_release(&t->signals.lock);
}

void signal_changemask(process_t * t, int64_t how, sigset_t * new,
                       sigset_t * old)
{
    if (t == NULL)
        return;
    spinlock_acquire(&t->signals.lock);

    if (old != NULL) {
        memcpy(old, &(t->signals.mask), sizeof(sigset_t));
    }

    if (new != NULL) {
        switch (how) {
        case SIG_BLOCK:
        case SIG_UNBLOCK:{
                for (int32_t i = 1; i < NSIG; ++i) {
                    if (SIGNAL_GET(new, i) == 0)
                        continue;
                    if (how == SIG_BLOCK
                        && SIGNAL_GET(&t->signals.mask, i) == 0)
                        SIGNAL_SETON(&t->signals.mask, i);
                    else if (how == SIG_UNBLOCK
                             && SIGNAL_GET(&t->signals.mask, i))
                        SIGNAL_SETOFF(&t->signals.mask, i);
                }
                break;
            }
        case SIG_SETMASK:{
                memcpy(&(t->signals.mask), new, sizeof(sigset_t));
                break;
            }
        default:
            kloge("signal_changemask: bad how %ld for process %ld\n",
                  how, t->pid);
        }
    }

    spinlock_release(&t->signals.lock);;
}

/* Lowest signal number set in the 64-bit mask, or 0. */
static int32_t signal_lowest(uint64_t set)
{
    for (int32_t s = 1; s < NSIG; s++) {
        if (set & (1ul << (s - 1)))
            return s;
    }
    return 0;
}

void signal_raise(process_t * t, int32_t sig)
{
    if (t == NULL || sig < 1 || sig >= NSIG)
        return;

    spinlock_acquire(&t->signals.lock);
    SIGNAL_SETON(&t->signals.pending, sig);
    spinlock_release(&t->signals.lock);

    sched_wake_process(t);
}

/* Apply the default disposition: terminate the process or ignore. */
static void signal_default_action(process_t * t, int32_t sig)
{
    int32_t action = signal_defaultactions[sig];

    if (action == SIG_ACTION_IGN || action == SIG_ACTION_CONT)
        return;

    if (sched_get_current_process() == t)
        sched_exit(128 + sig);
    else
        sched_kill_group(t->tgid, 0);
}

bool signal_deliver(process_t * t, syscall_regs_t * regs, int64_t retval)
{
    if (t == NULL || regs == NULL || t->mode != PROC_USER_MODE)
        return false;

    uint64_t pending;
    uint64_t blocked;

    spinlock_acquire(&t->signals.lock);
    pending = t->signals.pending.sig;
    blocked = t->signals.mask.sig;
    spinlock_release(&t->signals.lock);

    /* SIGKILL bypasses both the mask and the handler. */
    bool kill = (pending & (1ul << (SIGKILL - 1))) != 0;
    int32_t sig = kill ? SIGKILL : signal_lowest(pending & ~blocked);

    if (sig == 0)
        return false;

    spinlock_acquire(&t->signals.lock);
    sigaction_t act;

    memcpy(&act, &t->signals.actions[sig], sizeof(act));
    SIGNAL_SETOFF(&t->signals.pending, sig);
    spinlock_release(&t->signals.lock);

    if (!kill && (void *) act.address == SIG_IGN)
        return false;

    if (kill || (void *) act.address == SIG_DFL) {
        signal_default_action(t, sig);
        return false;
    }

    uint64_t restorer = (uint64_t) act.restorer;

    if (restorer == 0) {
        /* No restorer: the handler cannot return. Terminate. */
        signal_default_action(t, sig);
        return false;
    }

    hanos_sigframe_t *sf = kmalloc(sizeof(*sf));

    if (sf == NULL) {
        signal_default_action(t, sig);
        return false;
    }

    memset(sf, 0, sizeof(*sf));
    sf->pretcode = restorer;
    /* The frame's rax slot still holds the syscall number; the interrupted
     * syscall result arrives in retval and must resume in rax. */
    sf->rax = (uint64_t) retval;
    sf->rbx = regs->rbx;
    sf->rcx = regs->rcx;
    sf->rdx = regs->rdx;
    sf->rsi = regs->rsi;
    sf->rdi = regs->rdi;
    sf->rbp = regs->rbp;
    sf->r8 = regs->r8;
    sf->r9 = regs->r9;
    sf->r10 = regs->r10;
    sf->r11 = regs->r11;
    sf->r12 = regs->r12;
    sf->r13 = regs->r13;
    sf->r14 = regs->r14;
    sf->r15 = regs->r15;
    /* SYSCALL leaves the interrupted RIP in rcx and RFLAGS in r11. */
    sf->rip = regs->rcx;
    sf->cs = regs->cs;
    sf->rflags = regs->r11;
    sf->rsp = regs->rsp;
    sf->ss = regs->ss;
    sf->signo = (uint64_t) sig;
    sf->blocked = blocked;
    sf->retval = (uint64_t) retval;

    /* The syscall handler runs on the user stack, so leave room for its own
     * frames. Entry rsp must be 16n+8 for the handler ABI. */
    uint64_t newrsp = ((regs->rsp - sizeof(*sf) - 2048) & ~0xFULL) - 8;

    if (copy_to_user((void *) newrsp, sf, sizeof(*sf)) != 0) {
        kmfree(sf);
        signal_default_action(t, sig);
        return false;
    }
    kmfree(sf);

    regs->rcx = (uint64_t) act.address;
    regs->rip = (uint64_t) act.address;
    regs->rsp = newrsp;
    regs->rdi = (uint64_t) sig;
    regs->rax = 0;

    if (act.flags & SA_SIGINFO) {
        regs->rsi = newrsp + offsetof(hanos_sigframe_t, siginfo);
        regs->rdx = newrsp + offsetof(hanos_sigframe_t, ucontext);
    } else {
        regs->rsi = 0;
        regs->rdx = 0;
    }

    /* Block the signal during its handler unless SA_NODEFER, plus sa_mask. */
    spinlock_acquire(&t->signals.lock);
    if (!kill && !(act.flags & SA_NODEFER))
        SIGNAL_SETON(&t->signals.mask, sig);
    t->signals.mask.sig |= SIGACTION_MASK64(&act);
    spinlock_release(&t->signals.lock);

    return true;
}

int32_t signal_restore(process_t * t, syscall_regs_t * regs, int64_t * out)
{
    if (t == NULL || regs == NULL)
        return -1;

    hanos_sigframe_t sf;

    if (copy_from_user(&sf, (void *) (regs->rsp - 8), sizeof(sf)) != 0)
        return -1;

    regs->rbx = sf.rbx;
    regs->rcx = sf.rip;         /* SYSCALL resumes at rcx */
    regs->rdx = sf.rdx;
    regs->rsi = sf.rsi;
    regs->rdi = sf.rdi;
    regs->rbp = sf.rbp;
    regs->r8 = sf.r8;
    regs->r9 = sf.r9;
    regs->r10 = sf.r10;
    regs->r11 = sf.rflags;      /* ... with RFLAGS from r11 */
    regs->r12 = sf.r12;
    regs->r13 = sf.r13;
    regs->r14 = sf.r14;
    regs->r15 = sf.r15;
    regs->rip = sf.rip;
    regs->cs = sf.cs;
    regs->rflags = sf.rflags;
    regs->rsp = sf.rsp;
    regs->ss = sf.ss;

    spinlock_acquire(&t->signals.lock);
    t->signals.mask.sig = sf.blocked;
    spinlock_release(&t->signals.lock);

    if (out != NULL)
        *out = (int64_t) sf.rax;
    return 0;
}
