/**-----------------------------------------------------------------------------

 @file    signal.h
 @brief   Definitions for signal handling
 @details
 @verbatim

  This file contains the definitions and structures used for signal handling
  within the HanOS kernel. It includes definitions for signal sets, signal
  actions, and various signal-related constants. The functions provided allow
  for setting signal actions, changing signal masks, and managing signals
  within processes.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <stdint.h>
#include <stddef.h>
#include <stdbool.h>

#include <fs/vfs.h>

/**
 * @brief Alternate signal stack description
 */
typedef struct {
    void *base;
    int64_t flags;
    uint64_t size;
} stack_t;

/**
 * @brief Signal set stored as a 64-bit mask
 */
typedef struct {
    uint64_t sig;
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

#define NSIG            64      /* 64 instead of 65 here */

/**
 * @brief Signal action giving the handler, mask, flags and restorer
 *
 * Layout matches the Linux x86-64 struct k_sigaction, so musl's raw
 * rt_sigaction payload can be copied in directly.
 */
typedef struct {
    void (*address)(int);
    uint64_t flags;
    void (*restorer)(void);
    uint32_t mask[2];
} sigaction_t;

/* Combine the two 32-bit words of a sigaction mask into one 64-bit set. */
#define SIGACTION_MASK64(a) \
    ((uint64_t) (a)->mask[0] | ((uint64_t) (a)->mask[1] << 32))

/**
 * @brief Register frame saved by the syscall entry stub
 *
 * The stub pushes the 15 general-purpose registers (rax first), then the
 * interrupted RIP/CS/RFLAGS and RSP/SS. Unlike exception_regs_t it has no
 * error-code slot.
 */
typedef struct[[gnu::packed]] {
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
} syscall_regs_t;

#define SIGFRAME_INFO_SIZE      128
#define SIGFRAME_UC_SIZE        968

/**
 * @brief Signal frame pushed on the user stack before a handler runs
 *
 * pretcode is the return address the handler's ret pops, so it sits at the
 * frame's base. rt_sigreturn recovers the frame from rsp - 8.
 */
typedef struct[[gnu::packed]] {
    uint64_t pretcode;
    uint64_t rax;
    uint64_t rbx;
    uint64_t rcx;
    uint64_t rdx;
    uint64_t rsi;
    uint64_t rdi;
    uint64_t rbp;
    uint64_t r8;
    uint64_t r9;
    uint64_t r10;
    uint64_t r11;
    uint64_t r12;
    uint64_t r13;
    uint64_t r14;
    uint64_t r15;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
    uint64_t rsp;
    uint64_t ss;
    uint64_t signo;
    uint64_t blocked;
    uint64_t retval;
    uint8_t siginfo[SIGFRAME_INFO_SIZE];
    uint8_t ucontext[SIGFRAME_UC_SIZE];
} hanos_sigframe_t;

#define POLL_IN         1
#define POLL_OUT        2
#define POLL_MSG        3
#define POLL_ERR        4
#define POLL_PRI        5
#define POLL_HUP        6

/**
 * @brief Value carried with a queued signal
 */
typedef union {
    int32_t sival_int;
    void *sival_ptr;
} sigval_t;

typedef int64_t clock_t;

/**
 * @brief Detailed information about a delivered signal
 */
typedef struct {
    int32_t si_signo, si_errno, si_code;
    union {
        char __pad[128 - 2 * sizeof(int32_t) - sizeof(int64_t)];
        struct {
            union {
                struct {
                    posix_pid_t si_pid;
                    uid_t si_uid;
                } __piduid;
                struct {
                    int32_t si_timerid;
                    int32_t si_overrun;
                } __timer;
            } __first;
            union {
                sigval_t si_value;
                struct {
                    int32_t si_status;
                    clock_t si_utime, si_stime;
                } __sigchld;
            } __second;
        } __si_common;
        struct {
            void *si_addr;
            int16_t si_addr_lsb;
            union {
                struct {
                    void *si_lower;
                    void *si_upper;
                } __addr_bnd;
                uint32_t si_pkey;
            } __first;
        } __sigfault;
        struct {
            int64_t si_band;
            int32_t si_fd;
        } __sigpoll;
        struct {
            void *si_call_addr;
            int32_t si_syscall;
            uint32_t si_arch;
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
#define SIGIO           29      /* Same with SIGPOLL? */
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

typedef struct process process_t;   /* Definition in process.h */

void signal_action(process_t * t, int64_t signal, sigaction_t * new,
                   sigaction_t * old);
void signal_changemask(process_t * t, int64_t how, sigset_t * new,
                       sigset_t * old);

/* Queue a signal on a process. A signal already pending stays pending (no
 * queueing of duplicates). Wakes the target when it sleeps. */
void signal_raise(process_t * t, int32_t sig);

/* Deliver one pending, unblocked signal by rewriting the syscall return frame
 * to enter the handler. Applies the default action (terminate or ignore) when
 * no handler is installed. Returns true when a frame was rewritten. */
bool signal_deliver(process_t * t, syscall_regs_t * regs, int64_t retval);

/* Restore the context saved by signal_deliver from the frame on the user
 * stack, writing the interrupted syscall result to *out. Returns 0, or -1 on
 * a bad frame pointer. */
int32_t signal_restore(process_t * t, syscall_regs_t * regs, int64_t * out);
