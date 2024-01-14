#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>

#include <libc/sysfunc.h>

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
#define SYSCALL_MEMINFO     34
#define SYSCALL_PIPE        35
#define SYSCALL_UNLINK      36

void sys_libc_log(const char *message)
{
    int ret, errno;
    SYSCALL1(SYSCALL_DEBUGLOG, message);
}

int sys_fork()
{
    int64_t ret;
    int errno;
    SYSCALL0(SYSCALL_FORK);
    return ret;
}

int sys_meminfo()
{
    int64_t ret;
    int errno;
    SYSCALL0(SYSCALL_MEMINFO);
    return ret;
}

int sys_openat(int dirfd, const char *path, int flags)
{
    int ret, errno;
    SYSCALL3(SYSCALL_OPENAT, dirfd, path, flags);
    return ret;
}

int sys_getcwd(char *buffer, size_t size)
{
    int ret, errno;
    SYSCALL2(SYSCALL_GETCWD, buffer, size);
    return ret;
}

int sys_chdir(const char *path)
{
    int ret, errno;
    SYSCALL1(SYSCALL_CHDIR, path);
    return ret;
}

int sys_unlink(const char *path)
{
    int ret, errno;
    SYSCALL1(SYSCALL_UNLINK, path);
    return ret;
}

int sys_pipe(int *fd)
{
    /* We need to handle different definition of file handle,
     * int32_t or int64_t? */
    int ret, errno;
    SYSCALL2(SYSCALL_PIPE, fd, 0);
    return ret;
}

int sys_open(const char *path, int flags)
{
    int ret = sys_openat(AT_FDCWD, path, flags);
    return ret;
}

int sys_close(int fd)
{
    int ret, errno;
    SYSCALL1(SYSCALL_CLOSE, fd);
    return ret;
}

int sys_read(int fd, void *buf, size_t count)
{
    int64_t ret;
    int errno;
    SYSCALL3(SYSCALL_READ, fd, buf, count);
    return ret;
}

int sys_write(int fd, const void *buf, size_t count)
{
    int64_t ret;
    int errno;
    SYSCALL3(SYSCALL_WRITE, fd, buf, count);
    return ret;
}

int sys_exec(const char *path, char *const argv[])
{
    int errno, ret;
    const char *envp[] = { 
        "TIME_STYLE=posix-long-iso",
        "TERM=hanos",
        NULL
    };
    SYSCALL3(SYSCALL_EXECVE, path, argv, envp);
    return ret;
}

void sys_exit(int status)
{
    int ret, errno;
    SYSCALL1(SYSCALL_EXIT, status);
}

int sys_wait(int pid)
{
    while (true) {
        int errno, ret;
        SYSCALL3(SYSCALL_WAITPID, pid, NULL, 0);
        if (ret < 0) break;
    }
    return 0;
}

void sys_panic(const char *message)
{
    int errno, ret;
    SYSCALL1(SYSCALL_DEBUGLOG, message);
    sys_exit(255);
}

void *sys_malloc(int size)
{
    void *ret;
    int errno;
    SYSCALL6(SYSCALL_MMAP, 0, size, 0, 0x08, 0, 0);
    return ret;
}

int sys_mkdirat(const char *path)
{
    int ret, errno;
    SYSCALL3(SYSCALL_MKDIRAT, AT_FDCWD, path, 0755);
    return ret;
}

int sys_dup(int fd, int flags, int newfd)
{
    int errno, ret;
    SYSCALL3(SYSCALL_DUP3, fd, newfd, flags);
    return ret;
}

int sys_fstat(int fd, stat_t *statbuf)
{
    int errno, ret;
    SYSCALL2(SYSCALL_FSTAT, fd, statbuf);
    return ret;
}

int sys_stat(const char *path, stat_t *statbuf)
{
    int errno, ret;
    SYSCALL4(SYSCALL_FSTATAT, AT_FDCWD, path, statbuf, 0);
    return ret;
}

int sys_readdir(int fd, void *buffer)
{
    int ret, errno;
    SYSCALL2(SYSCALL_READDIR, fd, buffer);
    return ret;
}

