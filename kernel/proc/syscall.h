/**-----------------------------------------------------------------------------

 @file    syscall.h
 @brief   Definitions and declarations for system calls
 @details
 @verbatim

  This file contains the definitions and declarations for system calls within
  the HanOS kernel. System calls provide an interface for user space applications
  to request services from the operating system kernel. The file includes
  syscall numbers, macros, and function prototypes necessary for implementing
  and handling system calls.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once
#include <stdint.h>

#include <syscall_nr.h>

/* Standard I/O devices */
#define STDIN               0
#define STDOUT              1
#define STDERR              2

/* Used in memory map of syscall (Linux values) */
#define MAP_SHARED          0x01
#define MAP_PRIVATE         0x02
#define MAP_FIXED           0x10
#define MAP_ANONYMOUS       0x20

/* Open flags (Linux values) */
#define O_ACCMODE           00000003
#define O_RDONLY            00000000
#define O_WRONLY            00000001
#define O_RDWR              00000002
#define O_CREAT             00000100
#define O_EXCL              00000200
#define O_NOCTTY            00000400
#define O_TRUNC             00001000
#define O_APPEND            00002000
#define O_NONBLOCK          00004000
#define O_DSYNC             00010000
#define O_DIRECTORY         00200000
#define O_NOFOLLOW          00400000
#define O_CLOEXEC           02000000
#define O_SYNC              04010000
#define O_PATH              010000000

/* EFLAGS bits */
#define X86_EFLAGS_CF   0x00000001      /* Carry Flag */
#define X86_EFLAGS_PF   0x00000004      /* Parity Flag */
#define X86_EFLAGS_AF   0x00000010      /* Auxillary carry Flag */
#define X86_EFLAGS_ZF   0x00000040      /* Zero Flag */
#define X86_EFLAGS_SF   0x00000080      /* Sign Flag */
#define X86_EFLAGS_TF   0x00000100      /* Trap Flag */
#define X86_EFLAGS_IF   0x00000200      /* Interrupt Flag */
#define X86_EFLAGS_DF   0x00000400      /* Direction Flag */
#define X86_EFLAGS_OF   0x00000800      /* Overflow Flag */
#define X86_EFLAGS_IOPL 0x00003000      /* IOPL mask */
#define X86_EFLAGS_NT   0x00004000      /* Nested Process */
#define X86_EFLAGS_RF   0x00010000      /* Resume Flag */
#define X86_EFLAGS_VM   0x00020000      /* Virtual Mode */
#define X86_EFLAGS_AC   0x00040000      /* Alignment Check */
#define X86_EFLAGS_VIF  0x00080000      /* Virtual Interrupt Flag */
#define X86_EFLAGS_VIP  0x00100000      /* Virtual Interrupt Pending */
#define X86_EFLAGS_ID   0x00200000      /* CPUID detection flag */

#define CLOCK_REALTIME              0
#define CLOCK_MONOTONIC             1
#define CLOCK_PROCESS_CPUTIME_ID    2
#define CLOCK_THREAD_CPUTIME_ID     3
#define CLOCK_MONOTONIC_RAW         4
#define CLOCK_REALTIME_COARSE       5
#define CLOCK_MONOTONIC_COARSE      6
#define CLOCK_BOOTTIME              7

/* Mode and flags definitions for access() */
#define R_OK            4       /* Test mode for read permission */
#define W_OK            2       /* Test mode for write permission */
#define X_OK            1       /* Test mode for execute permission */
#define F_OK            0       /* Test mode for existence */

#define AT_SYMLINK_FOLLOW           2
#define AT_EACCESS                  4

#define PROT_NONE       0x00
#define PROT_READ       0x01
#define PROT_WRITE      0x02
#define PROT_EXEC       0x04

/**
 * @brief Time interval with second and microsecond parts
 */
typedef struct {
    uint64_t tv_sec;
    uint64_t tv_usec;
} timeval_t;

/**
 * @brief Resource usage counters for a process
 */
typedef struct {
    timeval_t ru_utime;         /* user CPU time used */
    timeval_t ru_stime;         /* system CPU time used */
    int64_t ru_maxrss;          /* maximum resident set size */
    int64_t ru_ixrss;           /* integral shared memory size */
    int64_t ru_idrss;           /* integral unshared data size */
    int64_t ru_isrss;           /* integral unshared stack size */
    int64_t ru_minflt;          /* page reclaims (soft page faults) */
    int64_t ru_majflt;          /* page faults (hard page faults) */
    int64_t ru_nswap;           /* swaps */
    int64_t ru_inblock;         /* block input operations */
    int64_t ru_oublock;         /* block output operations */
    int64_t ru_msgsnd;          /* IPC messages sent */
    int64_t ru_msgrcv;          /* IPC messages received */
    int64_t ru_nsignals;        /* signals received */
    int64_t ru_nvcsw;           /* voluntary context switches */
    int64_t ru_nivcsw;          /* involuntary context switches */
} rusage_t;

/*
 * System Calls are used to call a kernel service from user land. The goal is to
 * be able to switch from user mode to kernel mode, with the associated
 * privileges.
 *
 * The most common way to implement system calls is using a software interrupt.
 * It is probably the most portable way to implement system calls. Linux
 * traditionally uses interrupt 0x80 for this purpose on x86. Other systems may
 * have a fixed system call vector (e.g. PowerPC or Microblaze).
 *
 */
void syscall_init(void);

/* Terminate every thread of the current group with status and switch away.
 * Used by the exit syscalls and by a user-mode fault that must kill the
 * process. Does not return. */
void k_exit_group(int64_t status);

/*
 * The maximum number of syscall arguments is 5. Other arguments should be put
 * on the stack.
 */
extern int64_t syscall_entry(uint64_t id, ...);
