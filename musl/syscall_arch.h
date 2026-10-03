/*
 * HanOS syscall backend for musl.
 *
 * musl issues Linux x86-64 syscall numbers and expects a negative errno in
 * rax. HanOS uses its own syscall numbers and returns -1 with errno in rdx.
 * This shim maps the number and converts the return value. The argument
 * registers match musl's convention already.
 */
#ifndef _HANOS_SYSCALL_ARCH_H
#define _HANOS_SYSCALL_ARCH_H

#include <stdint.h>

#define __SYSCALL_LL_E(x) (x)
#define __SYSCALL_LL_O(x) (x)

/* HanOS syscall numbers, kernel/proc/syscall.h. POSIX calls use Linux
 * numbers; HanOS-only calls use 0x400 and above. */
#define H_MMAP       9
#define H_OPENAT     257
#define H_READ       0
#define H_WRITE      1
#define H_SEEK       8
#define H_CLOSE      3
#define H_SET_FS     0x401
#define H_IOCTL      16
#define H_GETPID     39
#define H_CLONE      56
#define H_FORK       57
#define H_EXECVE     59
#define H_FACCESSAT  269
#define H_FSTATAT    262
#define H_FSTAT      5
#define H_FCNTL      72
#define H_DUP3       292
#define H_NANOSLEEP  35
#define H_CLOCK_NANOSLEEP 230
#define H_SCHED_YIELD 24
#define H_FSYNC      74
#define H_FDATASYNC  75
#define H_POLL       7
#define H_SELECT     23
#define H_PSELECT6   270
#define H_SOCKET     41
#define H_CONNECT    42
#define H_ACCEPT     43
#define H_SENDTO     44
#define H_RECVFROM   45
#define H_SHUTDOWN   48
#define H_BIND       49
#define H_LISTEN     50
#define H_GETSOCKNAME 51
#define H_GETPEERNAME 52
#define H_SETSOCKOPT 54
#define H_GETSOCKOPT 55
#define H_UNLINK     87
#define H_MKDIRAT    258
#define H_SYMLINKAT  266
#define H_RENAMEAT   264
#define H_READLINK   89
#define H_PRLIMIT64  302
#define H_WAITPID    61
#define H_EXIT       60
#define H_EXIT_GROUP 231
#define H_READDIR    217
#define H_MUNMAP     11
#define H_GETCWD     79
#define H_GETCLOCK   228
#define H_FUTEX_WAIT 0x410
#define H_FUTEX_WAKE 0x411
#define H_PIPE       22
#define H_GETENTROPY 318
#define H_SIGPROCMASK 14
#define H_SIGACTION  13
#define H_KILL       62
#define H_TKILL      200
#define H_TGKILL     234
#define H_RT_SIGRETURN 15
#define H_EVENTFD2   290
#define H_EPOLL_CREATE1 291
#define H_EPOLL_CTL  233
#define H_EPOLL_WAIT 232
#define H_EPOLL_PWAIT 281

/* Linux syscall numbers musl uses. */
#define L_read        0
#define L_write       1
#define L_open        2
#define L_close       3
#define L_dup         32
#define L_dup2        33
#define L_nanosleep   35
#define L_stat        4
#define L_fstat       5
#define L_lstat       6
#define L_lseek       8
#define L_poll        7
#define L_mmap        9
#define L_mprotect    10
#define L_munmap      11
#define L_brk         12
#define L_rt_sigaction 13
#define L_rt_sigprocmask 14
#define L_rt_sigreturn 15
#define L_kill        62
#define L_tkill       200
#define L_tgkill      234
#define L_ioctl       16
#define L_fsync       74
#define L_fdatasync   75
#define L_readv       19
#define L_writev      20
#define L_getdents    78
#define L_pipe        22
#define L_select      23
#define L_sched_yield 24
#define L_madvise     28
#define L_getpid      39
#define L_clone       56
#define L_fork        57
#define L_execve      59
#define L_exit        60
#define L_wait4       61
#define L_getcwd      79
#define L_getuid      102
#define L_getgid      104
#define L_geteuid     107
#define L_getegid     108
#define L_arch_prctl  158
#define L_gettid      186
#define L_futex       202
#define L_getdents64  217
#define L_clock_gettime 228
#define L_clock_nanosleep 230
#define L_exit_group  231
#define L_openat      257
#define L_newfstatat  262
#define L_unlinkat    263
#define L_mkdir       83
#define L_mkdirat     258
#define L_symlink     88
#define L_symlinkat   266
#define L_rename      82
#define L_renameat    264
#define L_readlink    89
#define L_epoll_ctl   233
#define L_epoll_wait  232
#define L_epoll_pwait 281
#define L_epoll_create 213
#define L_epoll_create1 291
#define L_eventfd     284
#define L_eventfd2    290
#define L_readlinkat  267
#define L_faccessat   269
#define L_pselect6    270
#define L_prlimit64   302
#define L_set_robust_list 273
#define L_set_tid_address 218
#define L_getrandom   318
#define L_statx       332
#define L_rseq        334
#define L_pipe2       293
#define L_dup3        292
#define L_fcntl       72
#define L_socket      41
#define L_connect     42
#define L_accept      43
#define L_sendto      44
#define L_recvfrom    45
#define L_shutdown    48
#define L_bind        49
#define L_listen      50
#define L_getsockname 51
#define L_getpeername 52
#define L_setsockopt  54
#define L_getsockopt  55

#define L_CLONE_VM    0x100

/* HanOS stat layout (kernel/fs/vfs.h). */
struct hanos_stat {
    uint64_t st_dev;
    uint64_t st_ino;
    int32_t st_mode;
    int32_t st_nlink;
    int32_t st_uid;
    int32_t st_gid;
    uint64_t st_rdev;
    int64_t st_size;
    struct {
        int64_t sec, nsec;
    } st_atim, st_mtim, st_ctim;
    int64_t st_blksize;
    int64_t st_blocks;
};

/* Linux struct statx layout. */
struct hanos_statx {
    uint32_t stx_mask;
    uint32_t stx_blksize;
    uint64_t stx_attributes;
    uint32_t stx_nlink;
    uint32_t stx_uid;
    uint32_t stx_gid;
    uint16_t stx_mode;
    uint16_t __spare;
    uint64_t stx_ino;
    uint64_t stx_size;
    uint64_t stx_blocks;
    uint64_t stx_attributes_mask;
    struct {
        int64_t sec;
        uint32_t nsec;
        int32_t pad;
    } stx_atime, stx_btime, stx_ctime, stx_mtime;
    uint32_t stx_rdev_major;
    uint32_t stx_rdev_minor;
    uint32_t stx_dev_major;
    uint32_t stx_dev_minor;
    uint64_t stx_mnt_id;
    uint32_t stx_dio_mem_align;
    uint32_t stx_dio_offset_align;
    uint64_t spare[12];
};

static __inline void __hanos_fill_statx(struct hanos_stat *h,
                                        struct hanos_statx *x)
{
    x->stx_mask = 0x7ff;
    x->stx_blksize = 4096;
    x->stx_nlink = (uint32_t) h->st_nlink;
    x->stx_uid = (uint32_t) h->st_uid;
    x->stx_gid = (uint32_t) h->st_gid;
    x->stx_mode = (uint16_t) h->st_mode;
    x->stx_ino = h->st_ino;
    x->stx_size = (uint64_t) h->st_size;
    x->stx_blocks = (uint64_t) h->st_blocks;
    x->stx_atime.sec = h->st_atim.sec;
    x->stx_atime.nsec = (uint32_t) h->st_atim.nsec;
    x->stx_mtime.sec = h->st_mtim.sec;
    x->stx_mtime.nsec = (uint32_t) h->st_mtim.nsec;
    x->stx_ctime.sec = h->st_ctim.sec;
    x->stx_ctime.nsec = (uint32_t) h->st_ctim.nsec;
    x->stx_dev_major = (uint32_t) ((h->st_dev >> 8) & 0xfff);
    x->stx_dev_minor = (uint32_t) ((h->st_dev & 0xff)
                                   | ((h->st_dev >> 12) & 0xfff00));
    x->stx_rdev_major = 0;
    x->stx_rdev_minor = 0;
}

/* musl x86_64 struct kstat (mirrors Linux struct stat). */
struct hanos_kstat {
    uint64_t st_dev;
    uint64_t st_ino;
    uint64_t st_nlink;
    uint32_t st_mode;
    uint32_t st_uid;
    uint32_t st_gid;
    uint32_t __pad0;
    uint64_t st_rdev;
    int64_t st_size;
    int64_t st_blksize;
    int64_t st_blocks;
    int64_t st_atime_sec;
    int64_t st_atime_nsec;
    int64_t st_mtime_sec;
    int64_t st_mtime_nsec;
    int64_t st_ctime_sec;
    int64_t st_ctime_nsec;
    int64_t __unused[3];
};

static __inline void __hanos_fill_kstat(struct hanos_stat *h,
                                        struct hanos_kstat *k)
{
    k->st_dev = h->st_dev;
    k->st_ino = h->st_ino;
    k->st_nlink = (uint64_t) h->st_nlink;
    k->st_mode = (uint32_t) h->st_mode;
    k->st_uid = (uint32_t) h->st_uid;
    k->st_gid = (uint32_t) h->st_gid;
    k->__pad0 = 0;
    k->st_rdev = h->st_rdev;
    k->st_size = h->st_size;
    k->st_blksize = h->st_blksize;
    k->st_blocks = h->st_blocks;
    k->st_atime_sec = h->st_atim.sec;
    k->st_atime_nsec = h->st_atim.nsec;
    k->st_mtime_sec = h->st_mtim.sec;
    k->st_mtime_nsec = h->st_mtim.nsec;
    k->st_ctime_sec = h->st_ctim.sec;
    k->st_ctime_nsec = h->st_ctim.nsec;
    k->__unused[0] = 0;
    k->__unused[1] = 0;
    k->__unused[2] = 0;
}

#define AF_INET_K 2

struct hanos_sockaddr_in {
    uint16_t sin_family;
    uint16_t sin_port;          /* network order */
    uint32_t sin_addr;          /* network order */
    uint8_t sin_zero[8];
};

static __inline int __hanos_sockaddr_get(const void *addr, long len,
                                         uint32_t *ip, uint16_t *port)
{
    const struct hanos_sockaddr_in *sa =
        (const struct hanos_sockaddr_in *) addr;

    if (addr == 0 || len < 8 || sa->sin_family != AF_INET_K)
        return -1;
    *ip = __builtin_bswap32(sa->sin_addr);
    *port = __builtin_bswap16(sa->sin_port);
    return 0;
}

static __inline void __hanos_sockaddr_put(void *addr, long *len, uint32_t ip,
                                          uint16_t port)
{
    struct hanos_sockaddr_in *sa = (struct hanos_sockaddr_in *) addr;

    if (addr == 0)
        return;
    sa->sin_family = AF_INET_K;
    sa->sin_port = __builtin_bswap16(port);
    sa->sin_addr = __builtin_bswap32(ip);
    for (int i = 0; i < 8; i++)
        sa->sin_zero[i] = 0;
    if (len != 0 && *len >= (long) sizeof(*sa))
        *len = sizeof(*sa);
}

#define ARCH_SET_FS   0x1002
#define ARCH_GET_FS   0x1003
#define HANOS_FUTEX_WAIT 0
#define HANOS_FUTEX_WAKE 1
#define ENOSYS_NUM    38

static __inline long __hanos_syscall6(long n, long a1, long a2, long a3,
                                      long a4, long a5, long a6);

static __inline long __hanos_syscall0(long n)
{
    return __hanos_syscall6(n, 0, 0, 0, 0, 0, 0);
}

static __inline long __hanos_syscall1(long n, long a1)
{
    return __hanos_syscall6(n, a1, 0, 0, 0, 0, 0);
}

static __inline long __hanos_syscall2(long n, long a1, long a2)
{
    return __hanos_syscall6(n, a1, a2, 0, 0, 0, 0);
}

static __inline long __hanos_syscall3(long n, long a1, long a2, long a3)
{
    return __hanos_syscall6(n, a1, a2, a3, 0, 0, 0);
}

static __inline long __hanos_syscall4(long n, long a1, long a2, long a3, long a4)
{
    return __hanos_syscall6(n, a1, a2, a3, a4, 0, 0);
}

static __inline long __hanos_syscall5(long n, long a1, long a2, long a3, long a4,
                                      long a5)
{
    return __hanos_syscall6(n, a1, a2, a3, a4, a5, 0);
}

static __inline long __hanos_raw(long num, long a1, long a2, long a3, long a4,
                                 long a5, long a6)
{
    long ret;
    register long r10 __asm__("r10") = a4;
    register long r8 __asm__("r8") = a5;
    register long r9 __asm__("r9") = a6;

    __asm__ __volatile__ ("syscall"
                          : "=a"(ret)
                          : "a"(num), "D"(a1), "S"(a2), "d"(a3),
                            "r"(r10), "r"(r8), "r"(r9)
                          : "rcx", "r11", "memory");
    return ret;
}

static __inline long __hanos_syscall6(long n, long a1, long a2, long a3,
                                      long a4, long a5, long a6)
{
    switch (n) {
    case L_readv:{
            struct hanos_iovec {
                void *base;
                unsigned long len;
            } *iov = (struct hanos_iovec *) a2;
            unsigned long cnt = (unsigned long) a3;
            long total = 0;

            for (unsigned long i = 0; i < cnt; i++) {
                long r = __hanos_raw(H_READ, a1, (long) iov[i].base,
                                     (long) iov[i].len, 0, 0, 0);

                if (r < 0)
                    return total ? total : r;
                total += r;
                if ((unsigned long) r < iov[i].len)
                    break;
            }
            return total;
        }
    case L_writev:{
            /* HanOS has no writev; write each iovec in turn. */
            struct hanos_iovec {
                void *base;
                unsigned long len;
            } *iov = (struct hanos_iovec *) a2;
            unsigned long cnt = (unsigned long) a3;
            long total = 0;

            if (a1 == 1 || a1 == 2) {
                for (unsigned long i = 0; i < cnt; i++) {
                    long r = __hanos_raw(H_WRITE, a1, (long) iov[i].base,
                                         (long) iov[i].len, 0, 0, 0);

                    if (r < 0)
                        return total ? total : r;
                    total += r;
                }
            }
            return total;
        }
    case L_read:
        return __hanos_raw(H_READ, a1, a2, a3, 0, 0, 0);
    case L_write:
        return __hanos_raw(H_WRITE, a1, a2, a3, 0, 0, 0);
    case L_close:
        return __hanos_raw(H_CLOSE, a1, 0, 0, 0, 0, 0);
    case L_dup:
        return __hanos_raw(H_DUP3, a1, -1, 0, 0, 0, 0);
    case L_dup2:
        return __hanos_raw(H_DUP3, a1, a2, 0, 0, 0, 0);
    case L_dup3:
        return __hanos_raw(H_DUP3, a1, a2, a3, 0, 0, 0);
    case L_nanosleep:
        return __hanos_raw(H_NANOSLEEP, a1, a2, 0, 0, 0, 0);
    case L_unlinkat:
        return __hanos_raw(H_UNLINK, a2, 0, 0, 0, 0, 0);
    case L_mkdir:
        return __hanos_raw(H_MKDIRAT, -100, a1, a2, 0, 0, 0);
    case L_mkdirat:
        return __hanos_raw(H_MKDIRAT, a1, a2, a3, 0, 0, 0);
    case L_symlink:
        return __hanos_raw(H_SYMLINKAT, a1, -100, a2, 0, 0, 0);
    case L_symlinkat:
        return __hanos_raw(H_SYMLINKAT, a1, a2, a3, 0, 0, 0);
    case L_rename:
        return __hanos_raw(H_RENAMEAT, -100, a1, -100, a2, 0, 0);
    case L_renameat:
        return __hanos_raw(H_RENAMEAT, a1, a2, a3, a4, 0, 0);
    case L_readlink:
        return __hanos_raw(H_READLINK, -100, a1, a2, a3, 0, 0);
    case L_readlinkat:
        return __hanos_raw(H_READLINK, a1, a2, a3, a4, 0, 0);
    case L_eventfd:
        return __hanos_raw(H_EVENTFD2, a1, 0, 0, 0, 0, 0);
    case L_eventfd2:
        return __hanos_raw(H_EVENTFD2, a1, a2, 0, 0, 0, 0);
    case L_epoll_create:
        return __hanos_raw(H_EPOLL_CREATE1, 0, 0, 0, 0, 0, 0);
    case L_epoll_create1:
        return __hanos_raw(H_EPOLL_CREATE1, a1, 0, 0, 0, 0, 0);
    case L_epoll_ctl:
        return __hanos_raw(H_EPOLL_CTL, a1, a2, a3, a4, 0, 0);
    case L_epoll_wait:
        return __hanos_raw(H_EPOLL_WAIT, a1, a2, a3, a4, 0, 0);
    case L_epoll_pwait:
        return __hanos_raw(H_EPOLL_PWAIT, a1, a2, a3, a4, a5, 0);
    case L_prlimit64:
        return __hanos_raw(H_PRLIMIT64, a1, a2, a3, a4, 0, 0);
    case L_lseek:
        return __hanos_raw(H_SEEK, a1, a2, a3, 0, 0, 0);
    case L_mmap:
        return __hanos_raw(H_MMAP, a1, a2, a3, a4, a5, a6);
    case L_munmap:
        return __hanos_raw(H_MUNMAP, a1, a2, 0, 0, 0, 0);
    case L_ioctl:
        return __hanos_raw(H_IOCTL, a1, a2, a3, 0, 0, 0);
    case L_getpid:
    case L_gettid:
        return __hanos_raw(H_GETPID, 0, 0, 0, 0, 0, 0);
    case L_exit:
        return __hanos_raw(H_EXIT, a1, 0, 0, 0, 0, 0);
    case L_exit_group:
        return __hanos_raw(H_EXIT_GROUP, a1, 0, 0, 0, 0, 0);
    case L_wait4:
        return __hanos_raw(H_WAITPID, a1, a2, a3, 0, 0, 0);
    case L_open:
        return __hanos_raw(H_OPENAT, -100, a1, a2, a3, 0, 0);
    case L_openat:
        return __hanos_raw(H_OPENAT, a1, a2, a3, a4, 0, 0);
    case L_stat:
    case L_lstat:{
            struct hanos_stat hs;
            long r = __hanos_raw(H_FSTATAT, -100, a1, (long) &hs, 0, 0, 0);

            if (r < 0)
                return r;
            __hanos_fill_kstat(&hs, (struct hanos_kstat *) a2);
            return 0;
        }
    case L_fstat:{
            struct hanos_stat hs;
            long r = __hanos_raw(H_FSTAT, a1, (long) &hs, 0, 0, 0, 0);

            if (r < 0)
                return r;
            __hanos_fill_kstat(&hs, (struct hanos_kstat *) a2);
            return 0;
        }
    case L_newfstatat:{
            struct hanos_stat hs;
            long r;
            const char *path = (const char *) a2;

            if ((a4 & 0x1000) && path != 0 && *path == 0)
                r = __hanos_raw(H_FSTAT, a1, (long) &hs, 0, 0, 0, 0);
            else
                r = __hanos_raw(H_FSTATAT, a1, a2, (long) &hs, 0, 0, 0);
            if (r < 0)
                return r;
            __hanos_fill_kstat(&hs, (struct hanos_kstat *) a3);
            return 0;
        }
    case L_faccessat:
        return __hanos_raw(H_FACCESSAT, a1, a2, a3, a4, 0, 0);
    case L_statx:{
            struct hanos_stat hs;
            long r;

            if (a2 != 0 && *(const char *) a2 == 0)
                r = __hanos_raw(H_FSTAT, a1, (long) &hs, 0, 0, 0, 0);
            else
                r = __hanos_raw(H_FSTATAT, a1, a2, (long) &hs, 0, 0, 0);
            if (r < 0)
                return r;
            __hanos_fill_statx(&hs, (struct hanos_statx *) a5);
            return 0;
        }
    case L_getdents:
    case L_getdents64:{
            struct {
                uint64_t d_ino;
                int64_t d_off;
                uint16_t d_reclen;
                uint8_t d_type;
                char d_name[1024];
            } de;
            long r = __hanos_raw(H_READDIR, a1, (long) &de, 0, 0, 0, 0);

            if (r <= 0)
                return 0;       /* end of directory */
            unsigned long n = 0;

            while (n < sizeof(de.d_name) && de.d_name[n] != 0)
                n++;
            unsigned long reclen = (19 + n + 1 + 7) & ~(unsigned long) 7;

            if (reclen > (unsigned long) a3)
                return 0;

            struct {
                uint64_t d_ino;
                int64_t d_off;
                uint16_t d_reclen;
                uint8_t d_type;
                char name[1];
            } *o = (void *) a2;

            o->d_ino = de.d_ino;
            o->d_off = de.d_off;
            o->d_reclen = (uint16_t) reclen;
            o->d_type = de.d_type;
            for (unsigned long i = 0; i < n; i++)
                o->name[i] = de.d_name[i];
            o->name[n] = 0;
            return (long) reclen;
        }
    case L_fcntl:
        return __hanos_raw(H_FCNTL, a1, a2, a3, 0, 0, 0);
    case L_getcwd:{
            long r = __hanos_raw(H_GETCWD, a1, a2, 0, 0, 0, 0);

            return r < 0 ? r : a1;
        }
    case L_clone:
        return __hanos_raw(H_CLONE, a1, a2, a3, a4, a5, 0);
    case L_fork:
        return __hanos_raw(H_FORK, 0, 0, 0, 0, 0, 0);
    case L_execve:
        return __hanos_raw(H_EXECVE, a1, a2, a3, 0, 0, 0);
    case L_pipe:
        return __hanos_raw(H_PIPE, a1, 0, 0, 0, 0, 0);
    case L_pipe2:
        return __hanos_raw(H_PIPE, a1, a2, 0, 0, 0, 0);
    case L_clock_gettime:
        return __hanos_raw(H_GETCLOCK, 0, a1, a2, 0, 0, 0);
    case L_getrandom:{
            long r = __hanos_raw(H_GETENTROPY, a1, a2, 0, 0, 0, 0);

            return r < 0 ? r : a2;
        }
    case L_rt_sigprocmask:
        return __hanos_raw(H_SIGPROCMASK, a1, a2, a3, 0, 0, 0);
    case L_rt_sigaction:
        return __hanos_raw(H_SIGACTION, a1, a2, a3, 0, 0, 0);
    case L_kill:
    case L_tkill:
    case L_tgkill:
        /* HanOS numbers match the Linux numbers for these calls. */
        return __hanos_raw(n, a1, a2, a3, a4, a5, a6);
    case L_rt_sigreturn:
        return __hanos_raw(H_RT_SIGRETURN, 0, 0, 0, 0, 0, 0);
    case L_arch_prctl:
        if (a1 == ARCH_SET_FS)
            return __hanos_raw(H_SET_FS, a2, 0, 0, 0, 0, 0);
        if (a1 == ARCH_GET_FS) {
            *(long *) a2 = 0;
            return 0;
        }
        return -ENOSYS_NUM;
    case L_set_tid_address:
        return __hanos_raw(H_GETPID, 0, 0, 0, 0, 0, 0);
    case L_futex:
        if (a2 == HANOS_FUTEX_WAIT)
            return __hanos_raw(H_FUTEX_WAIT, a1, a4, a3, 0, 0, 0);
        if (a2 == HANOS_FUTEX_WAKE)
            return __hanos_raw(H_FUTEX_WAKE, a1, a3, 0, 0, 0, 0);
        return 0;
    case L_sched_yield:
        return __hanos_raw(H_SCHED_YIELD, 0, 0, 0, 0, 0, 0);
    case L_fsync:
        return __hanos_raw(H_FSYNC, a1, 0, 0, 0, 0, 0);
    case L_fdatasync:
        return __hanos_raw(H_FDATASYNC, a1, 0, 0, 0, 0, 0);
    case L_clock_nanosleep:
        return __hanos_raw(H_CLOCK_NANOSLEEP, a1, a2, a3, a4, 0, 0);
    case L_poll:
        return __hanos_raw(H_POLL, a1, a2, a3, 0, 0, 0);
    case L_select:
        return __hanos_raw(H_SELECT, a1, a2, a3, a4, a5, 0);
    case L_pselect6:
        return __hanos_raw(H_PSELECT6, a1, a2, a3, a4, a5, a6);
    case L_socket:
        return __hanos_raw(H_SOCKET, a1, a2, a3, 0, 0, 0);
    case L_bind:{
            uint32_t ip;
            uint16_t port;

            if (__hanos_sockaddr_get((const void *) a2, a3, &ip, &port) != 0)
                return -97;     /* -EAFNOSUPPORT */
            return __hanos_raw(H_BIND, a1, ip, port, 0, 0, 0);
        }
    case L_connect:{
            uint32_t ip;
            uint16_t port;

            if (__hanos_sockaddr_get((const void *) a2, a3, &ip, &port) != 0)
                return -97;
            return __hanos_raw(H_CONNECT, a1, ip, port, 0, 0, 0);
        }
    case L_listen:
        return __hanos_raw(H_LISTEN, a1, a2, 0, 0, 0, 0);
    case L_accept:{
            long fd = __hanos_raw(H_ACCEPT, a1, 0, 0, 0, 0, 0);

            if (fd < 0)
                return fd;
            if (a2 != 0) {
                uint32_t ip = 0;
                uint16_t port = 0;

                if (__hanos_raw(H_GETPEERNAME, fd, (long) &ip, (long) &port,
                                0, 0, 0) == 0)
                    __hanos_sockaddr_put((void *) a2, (long *) a3, ip, port);
            }
            return fd;
        }
    case L_sendto:{
            uint32_t ip = 0;
            uint16_t port = 0;

            if (a5 != 0
                && __hanos_sockaddr_get((const void *) a5, a6, &ip, &port) != 0)
                return -97;
            return __hanos_raw(H_SENDTO, a1, ip, port, a2, a3, 0);
        }
    case L_recvfrom:{
            uint32_t ip = 0;
            uint16_t port = 0;
            long r = __hanos_raw(H_RECVFROM, a1, a2, a3, (long) &ip,
                                 (long) &port, 0);

            if (r < 0)
                return r;
            if (a5 != 0)
                __hanos_sockaddr_put((void *) a5, (long *) a6, ip, port);
            return r;
        }
    case L_getsockname:{
            uint32_t ip = 0;
            uint16_t port = 0;
            long r = __hanos_raw(H_GETSOCKNAME, a1, (long) &ip, (long) &port,
                                 0, 0, 0);

            if (r < 0)
                return r;
            __hanos_sockaddr_put((void *) a2, (long *) a3, ip, port);
            return 0;
        }
    case L_getpeername:{
            uint32_t ip = 0;
            uint16_t port = 0;
            long r = __hanos_raw(H_GETPEERNAME, a1, (long) &ip, (long) &port,
                                 0, 0, 0);

            if (r < 0)
                return r;
            __hanos_sockaddr_put((void *) a2, (long *) a3, ip, port);
            return 0;
        }
    case L_shutdown:
        return __hanos_raw(H_SHUTDOWN, a1, a2, 0, 0, 0, 0);
    case L_setsockopt:
        return __hanos_raw(H_SETSOCKOPT, a1, a2, a3, a4, a5, 0);
    case L_getsockopt:
        return __hanos_raw(H_GETSOCKOPT, a1, a2, a3, a4, a5, 0);
    case L_mprotect:
    case L_madvise:
    case L_set_robust_list:
        return 0;
    case L_brk:
        return -ENOSYS_NUM;
    case L_rseq:
        return -ENOSYS_NUM;
    default:{
            static const char hx[] = "0123456789abcdef";
            char m[13];

            m[0] = 's';
            m[1] = 'y';
            m[2] = 's';
            m[3] = '=';
            for (int i = 0; i < 8; i++)
                m[4 + i] = hx[(n >> (28 - i * 4)) & 0xf];
            m[12] = '\n';
            __hanos_raw(H_WRITE, 2, (long) m, 13, 0, 0, 0);
            return -ENOSYS_NUM;
        }
    }
}

static __inline long __syscall0(long n)
{
    return __hanos_syscall6(n, 0, 0, 0, 0, 0, 0);
}

static __inline long __syscall1(long n, long a1)
{
    return __hanos_syscall6(n, a1, 0, 0, 0, 0, 0);
}

static __inline long __syscall2(long n, long a1, long a2)
{
    return __hanos_syscall6(n, a1, a2, 0, 0, 0, 0);
}

static __inline long __syscall3(long n, long a1, long a2, long a3)
{
    return __hanos_syscall6(n, a1, a2, a3, 0, 0, 0);
}

static __inline long __syscall4(long n, long a1, long a2, long a3, long a4)
{
    return __hanos_syscall6(n, a1, a2, a3, a4, 0, 0);
}

static __inline long __syscall5(long n, long a1, long a2, long a3, long a4,
                                long a5)
{
    return __hanos_syscall6(n, a1, a2, a3, a4, a5, 0);
}

static __inline long __syscall6(long n, long a1, long a2, long a3, long a4,
                                long a5, long a6)
{
    return __hanos_syscall6(n, a1, a2, a3, a4, a5, a6);
}

#define VDSO_USEFUL
#define VDSO_CGT_SYM "__vdso_clock_gettime"
#define VDSO_CGT_VER "LINUX_2.6"
#define VDSO_GETCPU_SYM "__vdso_getcpu"
#define VDSO_GETCPU_VER "LINUX_2.6"

#define IPC_64 0

#endif
