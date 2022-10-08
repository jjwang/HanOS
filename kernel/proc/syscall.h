#pragma once

/* This should be defined in vfs.h */
#define STDIN               0
#define STDOUT              1
#define STDERR              2

#define SYSCALL_WRITE       1

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
 * This is defined in syscall_usermode.asm. The maximum number of syscall
 * arguments is 5. Other arguments should be put on the stack.
 */
extern int64_t syscall(uint64_t id, ...);

size_t k_write(int fd, const void* buf, size_t count);

