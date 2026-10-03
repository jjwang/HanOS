/**-----------------------------------------------------------------------------

 @file    sysfunc.c
 @brief   Implementation of system call functions
 @details
 @verbatim

  This file contains the implementation of system call functions for HanOS.
  It includes various system call wrappers that provide interfaces for common
  operations such as file handling, process management, memory allocation,
  and logging. These functions utilize inline assembly to perform the actual
  system calls.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stddef.h>
#include <stdint.h>
#include <stdbool.h>
#include <stdio.h>

#include <sysfunc.h>

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

#define SYSCALL5(NUM, ARG0, ARG1, ARG2, ARG3, ARG4) ({         \
    register typeof(ARG3) arg3 asm("r10") = ARG3;              \
    register typeof(ARG4) arg4 asm("r8")  = ARG4;              \
    asm volatile ("syscall"                                    \
                  : "=a"(ret), "=d"(errno)                     \
                  : "a"(NUM), "D"(ARG0), "S"(ARG1), "d"(ARG2), \
                    "r"(arg3), "r"(arg4)                       \
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

/* Syscall numbers come from the shared header. */
#include <syscall_nr.h>

void sys_libc_log(const char *message)
{
    int32_t ret, errno;
    SYSCALL1(SYSCALL_DEBUGLOG, message);
}

int32_t sys_serial_write(const char *buf, uint64_t len)
{
    int64_t ret;
    int32_t errno;
    SYSCALL2(SYSCALL_SERIAL_WRITE, buf, len);
    return (int32_t) ret;
}

int32_t sys_runcmd(const char *cmd)
{
    int32_t ret, errno;
    SYSCALL1(SYSCALL_RUNCMD, cmd);
    return ret;
}

int32_t sys_fork()
{
    int64_t ret;
    int32_t errno;
    SYSCALL0(SYSCALL_FORK);
    return ret;
}

int32_t sys_meminfo()
{
    int64_t ret;
    int32_t errno;
    SYSCALL0(SYSCALL_MEMINFO);
    return ret;
}

int32_t sys_openat(int32_t dirfd, const char *path, int32_t flags)
{
    int32_t ret, errno;
    SYSCALL3(SYSCALL_OPENAT, dirfd, path, flags);
    return ret;
}

int32_t sys_getcwd(char *buffer, uint64_t size)
{
    int32_t ret, errno;
    SYSCALL2(SYSCALL_GETCWD, buffer, size);
    return ret;
}

int32_t sys_chdir(const char *path)
{
    int32_t ret, errno;
    SYSCALL1(SYSCALL_CHDIR, path);
    return ret;
}

int32_t sys_unlink(const char *path)
{
    int32_t ret, errno;
    SYSCALL1(SYSCALL_UNLINK, path);
    return ret;
}

int32_t sys_pipe(int32_t *fd)
{
    /* We need to handle different definition of file handle,
     * int32_t or int64_t? */
    int32_t ret, errno;
    SYSCALL2(SYSCALL_PIPE, fd, 0);
    return ret;
}

int32_t sys_open(const char *path, int32_t flags)
{
    int32_t ret = sys_openat(AT_FDCWD, path, flags);
    return ret;
}

int32_t sys_close(int32_t fd)
{
    int32_t ret, errno;
    SYSCALL1(SYSCALL_CLOSE, fd);
    return ret;
}

int32_t sys_read(int32_t fd, void *buf, uint64_t count)
{
    int64_t ret;
    int32_t errno;
    SYSCALL3(SYSCALL_READ, fd, buf, count);
    return ret;
}

int32_t sys_write(int32_t fd, const void *buf, uint64_t count)
{
    int64_t ret;
    int32_t errno;
    SYSCALL3(SYSCALL_WRITE, fd, buf, count);
    return ret;
}

int32_t sys_exec(const char *path, char *const argv[])
{
    int32_t errno, ret;
    const char *envp[] = {
        "TIME_STYLE=posix-long-iso",
        "TERM=hanos",
        NULL
    };
    SYSCALL3(SYSCALL_EXECVE, path, argv, envp);
    return ret;
}

void sys_exit(int32_t status)
{
    int32_t ret, errno;

    /* The raw syscall does not run musl's exit, so flush stdio first. */
    fflush(NULL);
    SYSCALL1(SYSCALL_EXIT, status);
}

int32_t sys_wait(int32_t pid)
{
    while (true) {
        int32_t errno, ret;
        SYSCALL3(SYSCALL_WAIT4, pid, NULL, 0);
        if (ret < 0)
            break;
    }
    return 0;
}

void sys_panic(const char *message)
{
    int32_t errno, ret;
    SYSCALL1(SYSCALL_DEBUGLOG, message);
    sys_exit(255);
}

void *sys_malloc(int32_t size)
{
    void *ret;
    int32_t errno;
    SYSCALL6(SYSCALL_MMAP, 0, size, 0, MAP_ANONYMOUS, 0, 0);
    return ret;
}

int32_t sys_mkdirat(const char *path)
{
    int32_t ret, errno;
    SYSCALL3(SYSCALL_MKDIRAT, AT_FDCWD, path, 0755);
    return ret;
}

int32_t sys_dup(int32_t oldfd, int32_t flags, int32_t newfd)
{
    int32_t errno, ret;
    SYSCALL3(SYSCALL_DUP3, oldfd, newfd, flags);
    return ret;
}

int32_t sys_fcntl(int32_t fd, int32_t cmd, int32_t arg)
{
    int32_t errno, ret;
    SYSCALL3(SYSCALL_FCNTL, fd, cmd, arg);
    return ret;
}

int32_t sys_proc_spawn(int32_t parent, const char *name)
{
    int32_t errno, ret;
    SYSCALL2(SYSCALL_PROC_SPAWN, parent, name);
    return ret;
}

int32_t sys_proc_map(int32_t pid, uint64_t vaddr, int32_t memh, int32_t prot)
{
    int32_t errno, ret;
    SYSCALL4(SYSCALL_PROC_MAP, pid, vaddr, memh, prot);
    return ret;
}

int32_t sys_proc_set_entry(int32_t pid, uint64_t rip, uint64_t rsp)
{
    int32_t errno, ret;
    SYSCALL3(SYSCALL_PROC_SET_ENTRY, pid, rip, rsp);
    return ret;
}

int32_t sys_proc_start(int32_t pid)
{
    int32_t errno, ret;
    SYSCALL1(SYSCALL_PROC_START, pid);
    return ret;
}

int32_t sys_fstat(int32_t fd, stat_t * statbuf)
{
    int32_t errno, ret;
    SYSCALL2(SYSCALL_FSTAT, fd, statbuf);
    return ret;
}

int32_t sys_stat(const char *path, stat_t * statbuf)
{
    int32_t errno, ret;
    SYSCALL4(SYSCALL_NEWFSTATAT, AT_FDCWD, path, statbuf, 0);
    return ret;
}

int32_t sys_readdir(int32_t fd, void *buffer)
{
    int32_t ret, errno;
    SYSCALL2(SYSCALL_GETDENTS64, fd, buffer);
    return ret;
}

int64_t sys_ep_create(void)
{
    int64_t ret, errno;
    SYSCALL0(SYSCALL_EP_CREATE);
    return ret;
}

int32_t sys_ipc_send(int64_t handle, const sys_ipc_msg_t * msg)
{
    int64_t ret, errno;
    SYSCALL2(SYSCALL_IPC_SEND, handle, msg);
    return (int32_t) ret;
}

int32_t sys_ipc_recv(int64_t handle, sys_ipc_msg_t * msg)
{
    int64_t ret, errno;
    SYSCALL2(SYSCALL_IPC_RECV, handle, msg);
    return (int32_t) ret;
}

int32_t sys_ipc_recv_nb(int64_t handle, sys_ipc_msg_t * msg)
{
    int64_t ret, errno;
    SYSCALL2(SYSCALL_IPC_RECV_NB, handle, msg);
    return (int32_t) ret;
}

int32_t sys_ipc_recv_timeout(int64_t handle, sys_ipc_msg_t * msg,
                         int64_t timeout_ms)
{
    int64_t ret, errno;
    SYSCALL3(SYSCALL_IPC_RECV_TIMEOUT, handle, msg, timeout_ms);
    return (int32_t) ret;
}

int32_t sys_ipc_call(int64_t handle, const sys_ipc_msg_t * req,
                 sys_ipc_msg_t * rep)
{
    int64_t ret, errno;
    SYSCALL3(SYSCALL_IPC_CALL, handle, req, rep);
    return (int32_t) ret;
}

int32_t sys_ipc_reply(int64_t handle, const sys_ipc_msg_t * msg)
{
    int64_t ret, errno;
    SYSCALL2(SYSCALL_IPC_REPLY, handle, msg);
    return (int32_t) ret;
}

int64_t sys_mem_alloc(uint64_t size)
{
    int64_t ret, errno;
    SYSCALL1(SYSCALL_MEM_ALLOC, size);
    return ret;
}

int32_t sys_mem_map(int64_t handle, uint64_t vaddr, int32_t prot)
{
    int64_t ret, errno;
    SYSCALL3(SYSCALL_MEM_MAP, handle, vaddr, prot);
    return (int32_t) ret;
}

int32_t sys_mem_unmap(int64_t handle, uint64_t vaddr)
{
    int64_t ret, errno;
    SYSCALL2(SYSCALL_MEM_UNMAP, handle, vaddr);
    return (int32_t) ret;
}

int32_t sys_irq_bind(int64_t irq_handle, int64_t ep_handle)
{
    int64_t ret, errno;
    SYSCALL2(SYSCALL_IRQ_BIND, irq_handle, ep_handle);
    return (int32_t) ret;
}

int32_t sys_irq_ack(int64_t irq_handle)
{
    int64_t ret, errno;
    SYSCALL1(SYSCALL_IRQ_ACK, irq_handle);
    return (int32_t) ret;
}

int32_t sys_handle_close(int64_t handle)
{
    int64_t ret, errno;
    SYSCALL1(SYSCALL_HANDLE_CLOSE, handle);
    return (int32_t) ret;
}

int64_t sys_handle_dup(int64_t handle)
{
    int64_t ret, errno;
    SYSCALL1(SYSCALL_HANDLE_DUP, handle);
    return ret;
}

int64_t sys_ioport_access(int32_t op, int32_t port, int32_t width, int32_t value)
{
    int64_t ret, errno;
    SYSCALL4(SYSCALL_IOPORT_ACCESS, op, port, width, value);
    return ret;
}

int32_t sys_bootinfo(bootinfo_t * bi)
{
    int64_t ret, errno;
    SYSCALL1(SYSCALL_BOOTINFO, bi);
    return (int32_t) ret;
}

int32_t sys_socket(int32_t domain, int32_t type, int32_t protocol)
{
    int64_t ret, errno;
    SYSCALL3(SYSCALL_SOCKET, domain, type, protocol);
    return (int32_t) ret;
}

int32_t sys_bind(int32_t sock, uint32_t ip, uint16_t port)
{
    int64_t ret, errno;
    SYSCALL3(SYSCALL_BIND, sock, ip, port);
    return (int32_t) ret;
}

int64_t sys_sendto(int32_t sock, uint32_t ip, uint16_t port, const void *buf,
                   uint64_t len)
{
    int64_t ret, errno;
    SYSCALL5(SYSCALL_SENDTO, sock, ip, port, buf, len);
    return ret;
}

int64_t sys_recvfrom(int32_t sock, void *buf, uint64_t len, uint32_t *ip,
                     uint16_t *port)
{
    int64_t ret, errno;
    SYSCALL5(SYSCALL_RECVFROM, sock, buf, len, ip, port);
    return ret;
}

int32_t sys_socket_close(int32_t sock)
{
    int64_t ret, errno;
    SYSCALL1(SYSCALL_SOCKET_CLOSE, sock);
    return (int32_t) ret;
}

int32_t sys_connect(int32_t sock, uint32_t ip, uint16_t port)
{
    int64_t ret, errno;
    SYSCALL3(SYSCALL_CONNECT, sock, ip, port);
    return (int32_t) ret;
}

int32_t sys_listen(int32_t sock, int32_t backlog)
{
    int64_t ret, errno;
    SYSCALL2(SYSCALL_LISTEN, sock, backlog);
    return (int32_t) ret;
}

int32_t sys_accept(int32_t sock)
{
    int64_t ret, errno;
    SYSCALL1(SYSCALL_ACCEPT, sock);
    return (int32_t) ret;
}

int64_t sys_mem_phys(int64_t handle, uint64_t offset)
{
    int64_t ret, errno;
    SYSCALL2(SYSCALL_MEM_PHYS, handle, offset);
    return ret;
}
