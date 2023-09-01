#pragma once

#define SYSCALL0(NUM) ({                     \
    asm volatile ("syscall"                  \
                  : "=a"(ret), "=d"(errno)   \
                  : "a"(NUM)                 \
                  : "rcx", "r11", "memory"); \
})

#define SYSCALL1(NUM, ARG0) ({               \
    asm volatile ("syscall"                  \
                  : "=a"(ret), "=d"(errno)   \
                  : "a"(NUM), "D"(ARG0)      \
                  : "rcx", "r11", "memory"); \
})

#define SYSCALL2(NUM, ARG0, ARG1) ({               \
    asm volatile ("syscall"                        \
                  : "=a"(ret), "=d"(errno)         \
                  : "a"(NUM), "D"(ARG0), "S"(ARG1) \
                  : "rcx", "r11", "memory");       \
})

#define SYSCALL3(NUM, ARG0, ARG1, ARG2) ({                    \
    asm volatile ("syscall"                                   \
                  : "=a"(ret), "=d"(errno)                    \
                  : "a"(NUM), "D"(ARG0), "S"(ARG1), "d"(ARG2) \
                  : "rcx", "r11", "memory");                  \
})

#define SYSCALL4(NUM, ARG0, ARG1, ARG2, ARG3) ({               \
    register typeof(ARG3) arg3 asm("r10") = ARG3;              \
    asm volatile ("syscall"                                    \
                  : "=a"(ret), "=d"(errno)                     \
                  : "a"(NUM), "D"(ARG0), "S"(ARG1), "d"(ARG2), \
                    "r"(arg3)                                  \
                  : "rcx", "r11", "memory");                   \
})

#define SYSCALL6(NUM, ARG0, ARG1, ARG2, ARG3, ARG4, ARG5) ({   \
    register typeof(ARG3) arg3 asm("r10") = ARG3;              \
    register typeof(ARG4) arg4 asm("r8")  = ARG4;              \
    register typeof(ARG5) arg5 asm("r9")  = ARG5;              \
    asm volatile ("syscall"                                    \
                  : "=a"(ret), "=d"(errno)                     \
                  : "a"(NUM), "D"(ARG0), "S"(ARG1), "d"(ARG2), \
                    "r"(arg3), "r"(arg4), "r"(arg5)            \
                  : "rcx", "r11", "memory");                   \
})

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
#define SYSCALL_GETCWD      26
#define SYSCALL_GETCLOCK    27
#define SYSCALL_READLINK    28
#define SYSCALL_GETRUSAGE   29
#define SYSCALL_GETRLIMIT   30
#define SYSCALL_UNAME       31
#define SYSCALL_FUTEX_WAIT  32
#define SYSCALL_FUTEX_WAKE  33

#define STDIN               0
#define STDOUT              1
#define STDERR              2

static inline int sys_fork() {
    int64_t ret;
    int errno;
    SYSCALL0(SYSCALL_FORK);
    if (ret == -1) 
        return errno;
    /* Remember we should also return correct value in mlibc */
    return ret;
}

static inline int sys_read(
    int fd, void *buf, size_t count, size_t *bytes_read)
{
    int64_t ret;
    int errno;
    SYSCALL3(SYSCALL_READ, fd, buf, count);
    if (ret == -1)
        return errno;
    if (bytes_read != NULL)
        *bytes_read = ret;
    return ret;
}

static inline int sys_write(
    int fd, const void *buf, size_t count, size_t *bytes_written)
{
    int64_t ret;
    int errno;
    SYSCALL3(SYSCALL_WRITE, fd, buf, count);
    if (ret == -1)
        return errno;
    if (bytes_written != NULL)
        *bytes_written = ret;
    return ret;
}

