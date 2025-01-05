/**-----------------------------------------------------------------------------

 @file    signal.c
 @brief   Implementation of signal handling functions
 @details
 @verbatim

  This file contains the implementation of functions required to handle signals
  within the HanOS kernel. It includes functions to set signal actions, change
  signal masks, and manage the default actions for various signals. The signal
  handling mechanism allows tasks to handle asynchronous events such as interrupts,
  exceptions, and inter-process communication.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <base/klog.h>
#include <proc/signal.h>
#include <proc/task.h>

int32_t signal_defaultactions[NSIG] = {
    [SIGABRT] = SIG_ACTION_CORE,
    [SIGALRM] = SIG_ACTION_TERM,
    [SIGBUS]  = SIG_ACTION_CORE,
    [SIGCHLD] = SIG_ACTION_IGN,
    [SIGCONT] = SIG_ACTION_CONT,
    [SIGFPE]  = SIG_ACTION_CORE,
    [SIGHUP]  = SIG_ACTION_TERM,
    [SIGILL]  = SIG_ACTION_CORE,
    [SIGINT]  = SIG_ACTION_TERM,
    [SIGKILL] = SIG_ACTION_TERM,
    [SIGPIPE] = SIG_ACTION_TERM,
    [SIGPOLL] = SIG_ACTION_TERM,
    [SIGPROF] = SIG_ACTION_TERM,
    [SIGPWR]  = SIG_ACTION_TERM,
    [SIGQUIT] = SIG_ACTION_CORE,
    [SIGSEGV] = SIG_ACTION_CORE,
    [SIGSTOP] = SIG_ACTION_STOP,
    [SIGTSTP] = SIG_ACTION_STOP,
    [SIGSYS]  = SIG_ACTION_CORE,
    [SIGTERM] = SIG_ACTION_TERM,
    [SIGTRAP] = SIG_ACTION_CORE,
    [SIGTTIN] = SIG_ACTION_STOP,
    [SIGTTOU] = SIG_ACTION_STOP,
    [SIGURG]  = SIG_ACTION_IGN,
    [SIGUSR1] = SIG_ACTION_TERM,
    [SIGUSR2] = SIG_ACTION_TERM,
    [SIGVTALRM] = SIG_ACTION_TERM,
    [SIGXCPU] = SIG_ACTION_CORE,
    [SIGXFSZ] = SIG_ACTION_CORE,
    [SIGWINCH] = SIG_ACTION_IGN
};

void signal_action(task_t *t, int64_t signal, sigaction_t *new, sigaction_t *old)
{
    if (!((signal) < NSIG && (signal) >= 0)) return;
    if (t == NULL) return;

    lock_lock(&t->signals.lock);

    if (old != NULL) {
        memcpy(old, &(t->signals.actions[signal]), sizeof(sigaction_t));
    }

    if (new != NULL) {
        memcpy(&(t->signals.actions[signal]), new, sizeof(sigaction_t));
    }

    lock_release(&t->signals.lock);
}

void signal_changemask(task_t *t, int64_t how, sigset_t *new, sigset_t *old)
{
    if (t == NULL) return;
    lock_lock(&t->signals.lock);

    if (old != NULL) {
        memcpy(old, &(t->signals.mask), sizeof(sigset_t));
    }

    if (new != NULL) {
        switch (how) {
            case SIG_BLOCK:
            case SIG_UNBLOCK: {
                for (int32_t i = 1; i < NSIG; ++i) {
                    if (SIGNAL_GET(new, i) == 0)
                        continue;
                    if (how == SIG_BLOCK && SIGNAL_GET(&t->signals.mask, i) == 0)
                        SIGNAL_SETON(&t->signals.mask, i);
                    else if (how == SIG_UNBLOCK && SIGNAL_GET(&t->signals.mask, i))
                        SIGNAL_SETOFF(&t->signals.mask, i);
                }
                break;
            }
            case SIG_SETMASK: {
                memcpy(&(t->signals.mask), new, sizeof(sigset_t));
                break;
            }
            default:
                kloge("signal_changemask: bad how %d for task %d\n",
                      how, t->tid);
        }
    }

    lock_release(&t->signals.lock);;
}
