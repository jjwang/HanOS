/**-----------------------------------------------------------------------------

 @file    syscall_nr.h
 @brief   Syscall numbers shared by the kernel and userspace
 @details
 @verbatim

  Numbers follow the Linux x86-64 ABI. Calls with a direct Linux equivalent use
  the Linux number. HanOS-only calls use 0x400 and above. Same-kind HanOS calls
  stay contiguous.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#define SYSCALL_READ            0
#define SYSCALL_WRITE           1
#define SYSCALL_CLOSE           3
#define SYSCALL_POLL            7
#define SYSCALL_FSTAT           5
#define SYSCALL_LSEEK           8
#define SYSCALL_MMAP            9
#define SYSCALL_MUNMAP          11
#define SYSCALL_RT_SIGACTION    13
#define SYSCALL_RT_SIGPROCMASK  14
#define SYSCALL_IOCTL           16
#define SYSCALL_SELECT          23
#define SYSCALL_PIPE            22
#define SYSCALL_SCHED_YIELD     24
#define SYSCALL_NANOSLEEP       35
#define SYSCALL_GETPID          39
#define SYSCALL_SOCKET          41
#define SYSCALL_CONNECT         42
#define SYSCALL_ACCEPT          43
#define SYSCALL_SENDTO          44
#define SYSCALL_RECVFROM        45
#define SYSCALL_SHUTDOWN        48
#define SYSCALL_BIND            49
#define SYSCALL_LISTEN          50
#define SYSCALL_GETSOCKNAME     51
#define SYSCALL_GETPEERNAME     52
#define SYSCALL_SETSOCKOPT      54
#define SYSCALL_GETSOCKOPT      55
#define SYSCALL_CLONE           56
#define SYSCALL_FORK            57
#define SYSCALL_EXECVE          59
#define SYSCALL_EXIT            60
#define SYSCALL_WAIT4           61
#define SYSCALL_UNAME           63
#define SYSCALL_FCNTL           72
#define SYSCALL_FSYNC           74
#define SYSCALL_FDATASYNC       75
#define SYSCALL_GETCWD          79
#define SYSCALL_CHDIR           80
#define SYSCALL_UNLINK          87
#define SYSCALL_READLINK        89
#define SYSCALL_GETRUSAGE       98
#define SYSCALL_GETPPID         110
#define SYSCALL_GETDENTS64      217
#define SYSCALL_CLOCK_GETTIME   228
#define SYSCALL_CLOCK_NANOSLEEP 230
#define SYSCALL_EXIT_GROUP      231
#define SYSCALL_OPENAT          257
#define SYSCALL_MKDIRAT         258
#define SYSCALL_NEWFSTATAT      262
#define SYSCALL_FACCESSAT       269
#define SYSCALL_PSELECT6        270
#define SYSCALL_DUP3            292
#define SYSCALL_GETRANDOM       318
#define SYSCALL_PRLIMIT64       302

/* HanOS microkernel calls. */
#define SYSCALL_DEBUGLOG        0x400
#define SYSCALL_SET_FS_BASE     0x401
#define SYSCALL_MEMINFO         0x402
#define SYSCALL_RUNCMD          0x403
#define SYSCALL_CHMOD           0x404
#define SYSCALL_BOOTINFO        0x405
#define SYSCALL_IOPORT_ACCESS   0x406
#define SYSCALL_SERIAL_WRITE    0x407

#define SYSCALL_FUTEX_WAIT      0x410
#define SYSCALL_FUTEX_WAKE      0x411

#define SYSCALL_EP_CREATE       0x420
#define SYSCALL_IPC_SEND        0x421
#define SYSCALL_IPC_RECV        0x422
#define SYSCALL_IPC_CALL        0x423
#define SYSCALL_IPC_REPLY       0x424
#define SYSCALL_IPC_RECV_NB     0x425
#define SYSCALL_IPC_RECV_TIMEOUT 0x426

#define SYSCALL_MEM_ALLOC       0x430
#define SYSCALL_MEM_MAP         0x431
#define SYSCALL_MEM_UNMAP       0x432
#define SYSCALL_MEM_PHYS        0x433

#define SYSCALL_HANDLE_CLOSE    0x438
#define SYSCALL_HANDLE_DUP      0x439

#define SYSCALL_IRQ_BIND        0x440
#define SYSCALL_IRQ_ACK         0x441

#define SYSCALL_PROC_SPAWN      0x450
#define SYSCALL_PROC_MAP        0x451
#define SYSCALL_PROC_SET_ENTRY  0x452
#define SYSCALL_PROC_START      0x453

#define SYSCALL_SOCKET_CLOSE    0x460

/* Size of syscall_funcs, used by the entry stub for the bounds check. */
#define SYSCALL_TABLE_SIZE      0x470
