#pragma once

#include <stdint.h>
#include <stddef.h>

#include <fs/vfs.h>

typedef struct {
    void            *base;
    int             flags;
    size_t          size;
} stack_t;

typedef struct {
    uint64_t        sig;
} sigset_t;

#define SIGNAL_GET(sigset, signal)      \
    ((sigset)->sig & (1lu << (((signal) - 1) % 64)))
#define SIGNAL_SETON(sigset, signal)    \
    (sigset)->sig |= (1lu << (((signal) - 1) % 64))
#define SIGNAL_SETOFF(sigset, signal)   \
    (sigset)->sig &= ~(1lu << (((signal) - 1) % 64))

#define SIG_DFL         ((void *)(0))
#define SIG_IGN         ((void *)(1))

#define SA_NOCLDSTOP    1
#define SA_NOCLDWAIT    2
#define SA_SIGINFO      4
#define SA_ONSTACK      0x08000000
#define SA_RESTART      0x10000000
#define SA_NODEFER      0x40000000
#define SA_RESETHAND    0x80000000
#define SA_RESTORER     0x04000000

#define NSIG            64  /* 64 instead of 65 here */

typedef struct {
    void            *address;
    sigset_t        mask;
    int             flags;
    void            (*restorer)(void);
} sigaction_t;

#define POLL_IN         1
#define POLL_OUT        2
#define POLL_MSG        3
#define POLL_ERR        4
#define POLL_PRI        5
#define POLL_HUP        6

union sigval {
    int             sival_int;
    void            *sival_ptr;
};

typedef long clock_t;

typedef struct {
    int si_signo, si_errno, si_code;
    union {
        char __pad[128 - 2 * sizeof(int) - sizeof(long)];
        struct {
            union {
                struct {
                    pid_t si_pid;
                    uid_t si_uid;
                } __piduid;
                struct {
                    int si_timerid;
                    int si_overrun;
                } __timer;
            } __first;
            union {
                union sigval si_value;
                struct {
                    int si_status;
                    clock_t si_utime, si_stime;
                } __sigchld;
            } __second;
        } __si_common;
        struct {
            void *si_addr;
            short si_addr_lsb;
            union {
                struct {
                    void *si_lower;
                    void *si_upper;
                } __addr_bnd;
                unsigned si_pkey;
            } __first;
        } __sigfault;
        struct {
            long si_band;
            int si_fd;
        } __sigpoll;
        struct {
            void *si_call_addr;
            int si_syscall;
            unsigned si_arch;
        } __sigsys;
    } __si_fields;
} siginfo_t;

#define SIGHUP          1
#define SIGQUIT         3
#define SIGTRAP         5
#define SIGABRT         6
#define SIGIOT          SIGABRT
#define SIGBUS          7
#define SIGKILL         9
#define SIGUSR1         10
#define SIGUSR2         12
#define SIGPIPE         13
#define SIGALRM         14
#define SIGSTKFLT       16
#define SIGCHLD         17
#define SIGCONT         18
#define SIGSTOP         19
#define SIGTSTP         20
#define SIGTTIN         21
#define SIGTTOU         22
#define SIGURG          23
#define SIGXCPU         24
#define SIGXFSZ         25
#define SIGVTALRM       26
#define SIGWINCH        28
#define SIGPOLL         29
#define SIGSYS          31
#define SIGUNUSED       SIGSYS
#define SIGCANCEL       32
#define SIGABRT         6
#define SIGFPE          8
#define SIGILL          4
#define SIGINT          2
#define SIGSEGV         11
#define SIGTERM         15
#define SIGPROF         27
#define SIGIO           29  /* Same with SIGPOLL? */
#define SIGPWR          30
#define SIGRTMIN        35
#define SIGRTMAX        64

#define SIG_BLOCK       0
#define SIG_UNBLOCK     1
#define SIG_SETMASK     2

#define SIG_ACTION_TERM 0
#define SIG_ACTION_IGN  1
#define SIG_ACTION_CORE 2
#define SIG_ACTION_STOP 3
#define SIG_ACTION_CONT 4

typedef struct _task_t task_t;  /* Definition in task.h */

void signal_action(task_t *t, int signal, sigaction_t *new, sigaction_t *old);
void signal_changemask(task_t *t, int how, sigset_t *new, sigset_t *old);
