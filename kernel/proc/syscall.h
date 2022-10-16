#pragma once

/* This should be defined in vfs.h */
#define STDIN               0
#define STDOUT              1
#define STDERR              2

#define SYSCALL_WRITE       1

/*
 * EFLAGS bits
 */
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

size_t k_write(int fd, const void* buf, size_t count);

