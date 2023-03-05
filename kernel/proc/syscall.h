#pragma once

#define SYSCALL_DEBUGLOG    0
#define SYSCALL_MMAP        1
#define SYSCALL_OPENAT      2
#define SYSCALL_READ        3
#define SYSCALL_WRITE       4
#define SYSCALL_SEEK        5
#define SYSCALL_CLOSE       6
#define SYSCALL_SET_FS_BASE 7
#define SYSCALL_IOCTL       8
#define SYSCALL_GETPID      9
#define SYSCALL_CHDIR       10
#define SYSCALL_MKDIRAT     11
#define SYSCALL_SOCKET      12
#define SYSCALL_BIND        13
#define SYSCALL_FORK        14
#define SYSCALL_EXECVE      15
#define SYSCALL_FACCESSAT   16
#define SYSCALL_FSTATAT     17
#define SYSCALL_FSTAT       18
#define SYSCALL_GETPPID     19
#define SYSCALL_FCNTL       20
#define SYSCALL_DUP3        21
#define SYSCALL_WAITPID     22
#define SYSCALL_EXIT        23
#define SYSCALL_READDIR     24
#define SYSCALL_MUNMAP      25

/* Standard I/O devices */
#define STDIN               0
#define STDOUT              1
#define STDERR              2

/* Used in memory map of syscall */
#define MAP_PRIVATE         0x01
#define MAP_SHARED          0x02
#define MAP_FIXED           0x04
#define MAP_ANONYMOUS       0x08

/* Reserve 3 bits for the access mode */
#define O_ACCMODE           0x0007
#define O_EXEC              1
#define O_RDONLY            2
#define O_RDWR              3
#define O_SEARCH            4
#define O_WRONLY            5

/* All remaining flags get their own bit */
#define O_APPEND            0x0008
#define O_CREAT             0x0010
#define O_DIRECTORY         0x0020
#define O_EXCL              0x0040
#define O_NOCTTY            0x0080
#define O_NOFOLLOW          0x0100
#define O_TRUNC             0x0200
#define O_NONBLOCK          0x0400
#define O_DSYNC             0x0800
#define O_RSYNC             0x1000
#define O_SYNC              0x2000
#define O_CLOEXEC           0x4000
#define O_PATH              0x8000

/* Error code */
#define EPERM               1   /* Operation not permitted */
#define ENOENT              2   /* No such file or directory */
#define EBADF               9   /* Bad file descriptor */
#define EINVAL              22  /* Invalid argument */
#define ENFILE              23  /* File table overflow */
#define EMFILE              24  /* Too many open files */
#define ENOSYS              38  /* Function not implemented */
#define ENOTSOCK            88  /* Argument is not a socket */
#define EPROTONOSUPPORT     93  /* Protocol not supported */
#define ESOCKTNOSUPPORT     94  /* Socket type not supported */
#define EAFNOSUPPORT        97  /* Address family not supported */

/* EFLAGS bits */
#define X86_EFLAGS_CF   0x00000001 /* Carry Flag */
#define X86_EFLAGS_PF   0x00000004 /* Parity Flag */
#define X86_EFLAGS_AF   0x00000010 /* Auxillary carry Flag */
#define X86_EFLAGS_ZF   0x00000040 /* Zero Flag */
#define X86_EFLAGS_SF   0x00000080 /* Sign Flag */
#define X86_EFLAGS_TF   0x00000100 /* Trap Flag */
#define X86_EFLAGS_IF   0x00000200 /* Interrupt Flag */
#define X86_EFLAGS_DF   0x00000400 /* Direction Flag */
#define X86_EFLAGS_OF   0x00000800 /* Overflow Flag */
#define X86_EFLAGS_IOPL 0x00003000 /* IOPL mask */
#define X86_EFLAGS_NT   0x00004000 /* Nested Task */
#define X86_EFLAGS_RF   0x00010000 /* Resume Flag */
#define X86_EFLAGS_VM   0x00020000 /* Virtual Mode */
#define X86_EFLAGS_AC   0x00040000 /* Alignment Check */
#define X86_EFLAGS_VIF  0x00080000 /* Virtual Interrupt Flag */
#define X86_EFLAGS_VIP  0x00100000 /* Virtual Interrupt Pending */
#define X86_EFLAGS_ID   0x00200000 /* CPUID detection flag */

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

/*
 * The maximum number of syscall arguments is 5. Other arguments should be put
 * on the stack.
 */
extern int64_t syscall_entry(uint64_t id, ...);

