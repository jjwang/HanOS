/**-----------------------------------------------------------------------------

 @file    syscall.c
 @brief   Implementation of system calls
 @details
 @verbatim

  This file contains the implementation of system calls within the HanOS kernel.
  System calls provide an interface for user space applications to request services
  from the kernel. This file defines various system call handlers, including those
  for file operations, process management, memory management, and signal handling.

  Ref: Page 2994 in Intel® 64 and IA-32 Architectures Software Developer’s Manual
  Combined Volumes: 1, 2A, 2B, 2C, 2D, 3A, 3B, 3C, 3D, and 4

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <numeric.h>

#include <arch/x64/cpu.h>
#include <arch/x64/idt.h>
#include <arch/x64/apic.h>
#include <arch/x64/panic.h>
#include <arch/x64/pci.h>
#include <arch/x64/isr_base.h>
#include <lib/klog.h>
#include <lib/vector.h>
#include <lib/kmalloc.h>
#include <mm/uaccess.h>
#include <mm/memobj.h>
#include <mm/ipc_buf.h>
#include <ipc/ipc.h>
#include <ipc/irq.h>
#include <srv/tty_srv.h>
#include <srv/fat32_srv.h>
#include <router/router.h>
#include <protocol.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/wait.h>
#include <proc/syscall.h>
#include <proc/signal.h>
#include <fs/vfs.h>
#include <srv/process_srv.h>
#include <srv/net_srv.h>
#include <device/display/gfx.h>

#define MMAP_ANON_BASE      0x80000000000

extern int64_t syscall_handler();
int32_t k_getclock(void *_ignored, int64_t which, vfs_timespec_t * out);

typedef int64_t(*syscall_ptr_t) (void);

static bool debug_info = false;

/* Copy a user path into a kernel buffer. Returns false on a bad pointer or a
 * path that is not NUL-terminated within ksize. */
static bool copy_user_path(const char *upath, char *kpath, uint64_t ksize)
{
    if (upath == NULL)
        return false;
    return strncpy_from_user(kpath, upath, ksize) >= 0;
}

#define EXEC_MAX_ARGS   32
#define EXEC_MAX_STRLEN VFS_MAX_PATH_LEN

static void free_exec_argv(char **kargv)
{
    for (int32_t i = 0; kargv != NULL && kargv[i] != NULL; i++)
        kmfree(kargv[i]);
}

/* Copy a NULL-terminated array of user strings into kernel memory. The caller
 * must provide room for EXEC_MAX_ARGS + 1 entries. Returns 0 on success. */
static int32_t copy_exec_argv(const char *uarr[], char **karr)
{
    for (int32_t i = 0; i <= EXEC_MAX_ARGS; i++)
        karr[i] = NULL;

    if (uarr == NULL)
        return 0;

    for (int32_t i = 0; i < EXEC_MAX_ARGS; i++) {
        uint64_t uptr = 0;

        if (copy_from_user(&uptr, &uarr[i], sizeof(uptr)) != 0)
            return -1;
        if (uptr == 0)
            return 0;

        char *s = kmalloc(EXEC_MAX_STRLEN);
        if (s == NULL)
            return -1;
        if (strncpy_from_user(s, (const char *) uptr, EXEC_MAX_STRLEN) < 0) {
            kmfree(s);
            return -1;
        }
        karr[i] = s;
    }

    return 0;
}

int64_t k_print_log()
{
    klogd("SYSCALL: useless log is just for debug purpose\n");
    return -1;
}

int64_t k_not_implemented()
{
    kpanic("SYSCALL: unimplemented\n");
    return -1;
}

int64_t k_debug_log(char *message)
{
    char kmsg[256];

    if (message == NULL
        || strncpy_from_user(kmsg, message, sizeof(kmsg)) < 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    char *s = strchr(kmsg, '\n');

    if (s != NULL && (*(s + 1) == '\0')) {
        *s = '\0';
    }

    klogd("debug: %s [message buffer: 0x%016lx]\n", kmsg, kmsg);

    return strlen(kmsg);
}

/* Write a server's bytes to the serial console. Serialised with the kernel log
 * so the two streams do not interleave mid-line. */
int64_t k_serial_write(const char *ubuf, uint64_t len)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    if (ubuf == NULL)
        return -1;

    if (len > 256)
        len = 256;

    char buf[256];

    if (len > 0) {
        if (!user_range_ok(t, ubuf, len)
            || copy_from_user(buf, ubuf, len) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
    }

    klog_write_raw(buf, len);
    return (int64_t) len;
}

int64_t k_sigprocmask(int64_t how, sigset_t * set, sigset_t * oldset)
{
    process_t *t = sched_get_current_process();
    if (t == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    cpu_set_errno(0);

    sigset_t new, old;

    if (set != NULL) {
        if (how != SIG_BLOCK && how != SIG_UNBLOCK && how != SIG_SETMASK) {
            cpu_set_errno(EINVAL);
            return -1;
        }
        if (copy_from_user(&new, set, sizeof(sigset_t)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
    }

    klogd("k_sigprocmask: how %ld from old 0x%016lx to new 0x%016lx\n",
          how, oldset, set);

    signal_changemask(t, how, set ? &new : NULL, oldset ? &old : NULL);

    if (oldset != NULL) {
        if (copy_to_user(oldset, &old, sizeof(sigset_t)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
    }

    return 0;
}

int64_t k_sigaction(int64_t s, sigaction_t * new, sigaction_t * old)
{
    process_t *t = sched_get_current_process();
    int64_t signal = (int64_t) ((int32_t) s);

    cpu_set_errno(0);

    if (signal >= NSIG || signal < 0 || signal == SIGKILL
        || signal == SIGSTOP || t == NULL) {
        klogd("k_sigaction: signal %ld from old 0x%016lx to new 0x%016lx "
              "and return -1 for invalid parameters\n", signal, old, new);
        cpu_set_errno(EINVAL);
        return -1;
    }

    sigaction_t newtmp = { 0 }, oldtmp = { 0 };
    if (new != NULL) {
        if (copy_from_user(&newtmp, new, sizeof(sigaction_t)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }

        if ((newtmp.flags & SA_RESTORER) == 0) {
            /* How to handle? cpu_set_errno(EINVAL) */
        }
    }

    klogd("k_sigaction: signal %ld from old 0x%016lx to new 0x%016lx\n",
          signal, old, new);

    signal_action(t, signal, new ? &newtmp : NULL, old ? &oldtmp : NULL);

    if (old != NULL) {
        if (copy_to_user(old, &oldtmp, sizeof(sigaction_t)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
    }

    return 0;
}

int64_t k_runcmd(char *cmd)
{
    char kcmd[64];

    if (cmd == NULL || strncpy_from_user(kcmd, cmd, sizeof(kcmd)) < 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    if (strcmp(kcmd, "lspci") == 0) {
        pci_list();
        gfx_start();
        return 0;
    } else {
        cpu_set_errno(EINVAL);
        return -1;
    }
}

int64_t k_getentropy(void *buffer, uint64_t length)
{
    cpu_set_errno(0);

    if (length > 256 || buffer == NULL) {
        cpu_set_errno(EINVAL);
        goto err_exit;
    }

    uint8_t kbuf[256];
    uint64_t remaining = length;
    uint8_t *p = kbuf;

    while (remaining >= 8) {
        uint64_t value = (uint64_t) ((rand(1337, 0, 0x8FFFFFFF) % 65535)
                                     * (hpet_get_nanos() % 65535));
        memcpy(p, &value, sizeof(value));
        p += 8;
        remaining -= 8;
    }

    if (remaining > 0) {
        uint64_t value = (uint64_t) ((rand(1337, 0, 0x8FFFFFFF) % 65535)
                                     * (hpet_get_nanos() % 65535));
        memcpy(p, &value, remaining);
    }

    if (copy_to_user(buffer, kbuf, length) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    return 0;
  err_exit:
    klogd("k_getentropy: return error with buffer 0x%016lx and length %ld\n",
          buffer, length);
    return -1;
}

/*
 * Need to use prot parameter - PROT_READ (0x01), PROT_WRITE (0x02),
 * PROT_EXEC (0x04).
 */
uint64_t k_vm_map(uint64_t * hint, uint64_t length, uint64_t prot,
                  uint64_t flags, uint64_t fd, uint64_t offset)
{
    (void) fd;
    (void) offset;

    cpu_set_errno(0);

    process_t *t = sched_get_current_process();
    addrspace_t *as = NULL;

    if (t != NULL) {
        if (t->pid < 1)
            kpanic("SYSCALL: %s meets corrupted pid\n", __func__);
        as = t->addrspace;
    }

    if (length == 0) {
        cpu_set_errno(EINVAL);
        goto err_exit;
    }

    if ((flags & MAP_ANONYMOUS) == 0) {
        cpu_set_errno(ENODEV);
        goto err_exit;
    }

    if (as == NULL) {
        cpu_set_errno(EINVAL);
        kpanic("k_vm_map: address space manager does not exist\n");
        goto err_exit;
    }

    uint64_t pf = VMM_FLAGS_USERMODE;
    uint64_t ptr = (uint64_t) hint;
    uint64_t np = NUM_PAGES(length);

    /* TODO: How to handle the first information page? */

    /* Unmap before mapping to a new malloc-ed memory block */
    if (ptr != (uint64_t) NULL)
        vmm_unmap(as, ptr, np);

    uint64_t phys_ptr =
        VIRT_TO_PHYS(kmalloc_chunk(np * PAGE_SIZE, __func__, __LINE__));

    /* On QEMU, the memory will be set to zero. But on real hardaware,
     * maybe they will not be set to zero. Need to do this!
     */
    memset((void *) PHYS_TO_VIRT(phys_ptr), 0, np * PAGE_SIZE);

    if (!(flags & MAP_FIXED)) {
        ptr = phys_ptr + MMAP_ANON_BASE;
    }

    vmm_map(as, ptr, phys_ptr, NUM_PAGES(length), pf);

    if (debug_info) {
        klogi
            ("k_vm_map: pid %ld #%ld 0x%016lx(PML4 0x%016lx) map 0x%016lx to 0x%016lx with %ld "
             "pages, prot 0x%016lx, flags 0x%016lx\n", t->pid,
             vec_length(&t->addrspace->mmap_list), as, as->PML4, phys_ptr, ptr, np,
             prot, flags);
    }

    mem_map_t m = { 0 };

    m.vaddr = ptr;
    m.paddr = phys_ptr;
    m.np = NUM_PAGES(length);
    m.flags = pf;

    vec_push_back(&t->addrspace->mmap_list, m);

    return ptr;

  err_exit:
    kloge("k_vm_map: pid %ld 0x%016lx(PML4 0x%016lx) returns NULL in malloc()\n",
          t->pid, as, as->PML4);
    return -1;
}

int64_t k_vm_unmap(void *ptr, uint64_t size)
{
    /* Need to implement memory free */
    cpu_set_errno(0);

    process_t *t = sched_get_current_process();
    addrspace_t *as = NULL;

    if (t != NULL) {
        if (t->pid < 1)
            kpanic("SYSCALL: %s meets corrupted pid\n", __func__);
        as = t->addrspace;
    }

    if (size == 0) {
        cpu_set_errno(EINVAL);
        goto err_exit;
    }

    uint64_t np = NUM_PAGES(size);
    vmm_unmap(as, (uint64_t) ptr, np);

    if (debug_info) {
        klogi("k_vm_unmap: 0x%016lx(PML4 0x%016lx) unmap 0x%016lx with %ld pages\n",
              as, as->PML4, ptr, np);
    }

    return 0;

  err_exit:
    cpu_set_errno(EINVAL);
    return (uint64_t) NULL;
}

int64_t k_openat(int64_t dirfd, char *path, int64_t flags, int64_t mode)
{
    /* "mode" is always zero */
    (void) mode;
    (void) dirfd;
    cpu_set_errno(0);

    /* Copy the user path into the kernel before touching it. */
    char kpath[VFS_MAX_PATH_LEN] = { 0 };
    if (path == NULL || strncpy_from_user(kpath, path, sizeof(kpath)) < 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    path = kpath;

    process_t *t = sched_get_current_process();
    const char *cwd = (t != NULL) ? t->cwd : "/";
    int32_t svc = SVC_FS;

    /* The VFS server owns the namespace and resolves cwd+path; it redirects
     * paths under the FAT mount to the FAT server. */
    vfs_fd_t nfd = vfs_open_path(cwd, path, (int32_t) flags, &svc);

    if (nfd == VFS_INVALID_FD) {
        cpu_set_errno(ENOENT);
        return -1;
    }

    if (flags & O_CLOEXEC)
        process_fd_fcntl((int32_t) nfd, F_SETFD, FD_CLOEXEC);

    cpu_set_errno(0);
    return nfd;
}

int64_t k_chmod(char *path, int64_t flags)
{
    (void) path;
    (void) flags;
    cpu_set_errno(ENOSYS);
    return -1;
}

int64_t k_unlink(char *path)
{
    cpu_set_errno(0);

    char kpath[VFS_MAX_PATH_LEN] = { 0 };
    if (!copy_user_path(path, kpath, sizeof(kpath))) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    path = kpath;

    klogi("k_unlink: %s\n", path);

    process_t *t = sched_get_current_process();
    const char *cwd = (t != NULL) ? t->cwd : "/";

    if (vfs_unlink_path(cwd, path) < 0) {
        cpu_set_errno(ENOENT);
        return -1;
    }
    cpu_set_errno(0);
    return 0;
}

int64_t k_seek(int64_t fd, int64_t offset, int64_t whence)
{
    cpu_set_errno(0);

    if (fd >= 0 && fd < 3) {
        int32_t kind = 0, svc = 0;
        int64_t sfd = 0;
        uint64_t size = 0, seek = 0;

        /* Standard streams are not seekable unless a dup redirected them. */
        if (process_fd_get((int32_t) fd, &kind, &svc, &sfd, &size, &seek) != 0) {
            klogv("k_seek: fd %ld(0x%016lx), offset %ld, whence %ld\n",
                  fd, fd, offset, whence);
            return 0;
        }
    }

    int64_t ret = vfs_seek(fd, offset, whence);

    klogd("k_seek: fd %ld(0x%016lx), offset %ld, whence %ld and return %ld\n",
          fd, fd, offset, whence, ret);
    if (ret < 0)
        cpu_set_errno(EINVAL);

    return ret;
}

int64_t k_close(int64_t fd)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    klogd("k_close: close fd %ld\n", fd);

    if (fd >= 0 && fd < 3) {
        /* Closing a standard fd only drops any redirection to a file. */
        int32_t kind = 0, svc = 0;
        int64_t sfd = 0;

        process_fd_close((int32_t) fd, &kind, &svc, &sfd);
        return 0;
    }

    if (t == NULL) {
        cpu_set_errno(ESRCH);
        return -1;
    }

    return vfs_close(fd);
}

int64_t k_read(int64_t fd, void *buf, uint64_t count)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    if (buf != NULL && count > 0 && !user_range_ok(t, buf, count)) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    klogd("k_read: read %ld from fd %ld\n", count, fd);

    if (fd >= 0 && fd < 3) {
        /* Standard input is the tty unless a dup redirected the fd. */
        int32_t kind = 0, svc = 0;
        int64_t sfd = 0;
        uint64_t size = 0, seek = 0;

        if (process_fd_get((int32_t) fd, &kind, &svc, &sfd, &size, &seek) == 0)
            return vfs_read(fd, count, buf);

        if (fd == STDIN)
            return tty_server_read(buf, count);

        cpu_set_errno(EBADF);
        return -1;
    } else if (fd >= VFS_MIN_FD) {
        int64_t len = vfs_read(fd, count, buf);
        klogd
            ("k_read: try to read %ld bytes from file %ld and return %ld bytes\n",
             count, fd, len);
        return len;
    } else {
        cpu_set_errno(EBADF);
        return -1;
    }
}

int64_t k_write(int64_t fd, const void *buf, uint64_t count)
{
    process_t *t = sched_get_current_process();

    cpu_set_errno(0);

    if (buf != NULL && count > 0 && !user_range_ok(t, buf, count)) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    if (fd >= 0 && fd < 3) {
        /* Standard output is the tty unless a dup redirected the fd. */
        int32_t kind = 0, svc = 0;
        int64_t sfd = 0;
        uint64_t size = 0, seek = 0;

        if (process_fd_get((int32_t) fd, &kind, &svc, &sfd, &size, &seek) == 0)
            return vfs_write(fd, count, buf);

        if (fd == STDOUT || fd == STDERR)
            return tty_server_write(buf, count);

        cpu_set_errno(EBADF);
        return -1;
    }

    return vfs_write(fd, count, buf);
}

int64_t k_set_fs_base(uint64_t val)
{
    process_t *t = sched_get_current_process();

    cpu_set_errno(0);
    klogd("k_set_fs_base: process #%ld set to 0x%016lx\n",
          t == NULL ? 0 : t->pid, val);
    write_msr(MSR_FS_BASE, val);
    if (t != NULL)
        t->fs_base = val;
    return 0;
}

int64_t k_ioctl(int64_t fd, int64_t request, int64_t arg)
{
    (void) fd;
    (void) request;
    (void) arg;

    /* The userspace tty server does not implement terminal ioctls yet. */
    cpu_set_errno(EINVAL);
    return -1;
}

int64_t k_fstatat(int64_t dirfd, const char *path, int64_t statbuf,
                  int64_t flags)
{
    (void) dirfd;
    (void) flags;

    char kpath[VFS_MAX_PATH_LEN] = { 0 };
    if (!copy_user_path(path, kpath, sizeof(kpath))) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    path = kpath;

    process_t *t = sched_get_current_process();
    const char *cwd = (t != NULL) ? t->cwd : "/";
    vfs_stat_t st;

    if (vfs_stat_path(cwd, path, &st) < 0) {
        cpu_set_errno(ENOENT);
        return -1;
    }
    if (copy_to_user((void *) statbuf, &st, sizeof(st)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    cpu_set_errno(0);
    return 0;
}

int64_t k_fstat(int64_t fd, int64_t statbuf)
{
    if (fd == STDIN || fd == STDOUT || fd == STDERR) {
        /*
         * Set the file stat buffer to zero. If we do nothing here, maybe it
         * will cause crash in some apps, e.g., cat in coreutils.
         */
        if (clear_user((void *) statbuf, sizeof(vfs_stat_t)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
        klogd("k_fstat: success with fd %ld\n", fd);
        return 0;
    }

    vfs_node_desc_t *desc = vfs_fd_to_desc(fd, __func__);
    cpu_set_errno(0);

    if (desc != NULL && desc->server) {
        vfs_stat_t st;

        memset(&st, 0, sizeof(st));

        if (desc->svc == SVC_FAT) {
            uint64_t size = 0;
            bool is_dir = false;

            if (fat32_fstat_fd(desc->server_fd, &size, &is_dir) < 0) {
                cpu_set_errno(ENOENT);
                return -1;
            }
            st.st_mode = is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
            st.st_nlink = 1;
            st.st_size = size;
        } else if (vfs_server_fstat(desc->server_fd, &st) < 0) {
            cpu_set_errno(ENOENT);
            return -1;
        }

        if (copy_to_user((void *) statbuf, &st, sizeof(st)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
        return 0;
    }

    kloge("k_fstat: fail with fd %ld\n", fd);
    cpu_set_errno(EINVAL);
    return -1;
}

/* TODO: Currently ignoring the flags parameter. */
int64_t k_faccessat(int64_t dirfd, const char *path, uint64_t mode,
                    uint64_t flags)
{
    (void) dirfd;
    (void) flags;

    cpu_set_errno(0);

    char kpath[VFS_MAX_PATH_LEN] = { 0 };
    if (!copy_user_path(path, kpath, sizeof(kpath))) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    path = kpath;

    process_t *t = sched_get_current_process();
    const char *cwd = (t != NULL) ? t->cwd : "/";

    klogi("k_faccessat: access \"%s\" at mode 0x%016lx\n", path, mode);

    if (vfs_access_path(cwd, path, mode) < 0) {
        cpu_set_errno(ENOENT);
        return -1;
    }

    cpu_set_errno(0);
    return 0;
}

int64_t k_getpid()
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    if (t != NULL) {
        klogd("k_getpid: process #%ld\n", t->pid);
        if (t->pid >= 1)
            return t->pid;
    }

    cpu_set_errno(EINVAL);
    return -1;
}

int64_t k_chdir(char *dir)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    char kdir[VFS_MAX_PATH_LEN] = { 0 };
    if (!copy_user_path(dir, kdir, sizeof(kdir))) {
        cpu_set_errno(EFAULT);
        goto err_exit;
    }
    dir = kdir;

    /* TODO: Need to add a bounds check. */
    while (*dir == ' ') {
        dir++;
    }

    if (strlen(dir) == 0) {
        cpu_set_errno(ENOENT);
        goto err_exit;
    }

    if (t == NULL) {
        cpu_set_errno(ENODEV);
        goto err_exit;
    }

    if (t->pid < 1) {
        cpu_set_errno(ESRCH);
        goto err_exit;
    }

    char fullpath[VFS_MAX_PATH_LEN] = { 0 };
    char parent[VFS_MAX_PATH_LEN] = { 0 };
    char currdir[VFS_MAX_PATH_LEN] = { 0 };

    uint64_t k = 0;
    uint64_t len = strlen(dir);

    strcpy(fullpath, t->cwd);

    /* Note that "i" will loop to last character "\0" */
    for (uint64_t i = 0; i < len; i++) {
        if (dir[i] != '/') {
            currdir[k++] = dir[i];
            if (i != len - 1)
                continue;
        }
        currdir[k] = '\0';

        /* "currdir" stores current folder name */
        if (strcmp(currdir, ".") == 0) {
            /* It is current folder, do nothing */
        } else if (strcmp(currdir, "..") == 0) {
            /* Change to parent folder */
            if (vfs_get_parent_dir(fullpath, parent, currdir) < 0) {
                cpu_set_errno(EINVAL);
                goto err_exit;
            }
            strcpy(fullpath, parent);
        } else if (strlen(currdir) == 0 && i == 0) {
            /* It is root folder based */
            strcpy(fullpath, "/");
        } else {
            uint64_t fpl = strlen(fullpath);
            if (fpl > 0) {
                if (fullpath[fpl - 1] != '/')
                    strncat(fullpath, "/", sizeof(fullpath));
            }
            strncat(fullpath, currdir, sizeof(fullpath));
        }

        /* Set "currdir" to zero length */
        k = 0;
    }

    klogd("k_chdir: current \"%s\", target \"%s\" and change to \"%s\"",
          t->cwd, dir, fullpath);

    /* The VFS server owns the namespace; ask it whether the folder exists. */
    {
        vfs_stat_t st;

        memset(&st, 0, sizeof(st));
        if (vfs_stat_path("/", fullpath, &st) < 0
            || (st.st_mode & S_IFMT) != S_IFDIR) {
            cpu_set_errno(ENOENT);
            goto err_exit;
        }
    }

    strcpy(t->cwd, fullpath);
    return 0;
  err_exit:
    return -1;
}

int64_t k_readdir(int64_t fd, uint64_t buff)
{
    vfs_node_desc_t *desc = vfs_fd_to_desc(fd, __func__);
    int64_t errno = 0;

    cpu_set_errno(errno);

    if (desc == NULL) {
        errno = EINVAL;
        goto err_exit;
    }

    if (desc->server) {
        dirent_t de;

        memset(&de, 0, sizeof(de));

        if (desc->svc == SVC_FAT) {
            char name[256];
            uint64_t size = 0;
            bool is_dir = false;

            if (fat32_readdir_fd(desc->server_fd, desc->curr_dir_idx, name,
                                 sizeof(name), &size, &is_dir) != 0) {
                cpu_set_errno(0);
                return 0;       /* end of directory */
            }
            de.d_ino = desc->curr_dir_idx + 1;
            de.d_type = is_dir ? DT_DIR : DT_REG;
            strncpy(de.d_name, name, sizeof(de.d_name) - 1);
            desc->curr_dir_idx++;
        } else if (vfs_server_readdir(fd, &de) < 0) {
            /* End of directory or a server error. */
            cpu_set_errno(0);
            return 0;
        }

        if (copy_to_user((void *) buff, &de, sizeof(de)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
        return (int64_t) sizeof(de);
    }

    errno = ENOTDIR;
  err_exit:
    cpu_set_errno(errno);
    return -1;
}

int64_t k_meminfo()
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    if (t == NULL) {
        cpu_set_errno(ENODEV);
        goto err_exit;
    }

    if (t->pid < 1) {
        cpu_set_errno(ESRCH);
        goto err_exit;
    }

    pmm_dump_usage();
    return 0;

  err_exit:
    return -1;
}

int64_t k_pipe(int32_t * fd, uint32_t flags)
{
    (void) flags;

    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    if (t == NULL) {
        cpu_set_errno(ENODEV);
        goto err_exit;
    }

    if (t->pid < 1) {
        cpu_set_errno(ESRCH);
        goto err_exit;
    }

    if (router_lookup(SVC_PIPE) != NULL) {
        ipc_msg_t req;
        ipc_msg_t rep;

        memset(&req, 0, sizeof(req));
        req.tag = PIPE_CREATE;

        if (!router_forward(SVC_PIPE, &req, &rep)
            || (int64_t) rep.words[0] < 0) {
            cpu_set_errno(ENOMEM);
            return -1;
        }

        vfs_fd_t rfd = vfs_open_server_svc((int64_t) rep.words[1],
                                           "/dev/pipe", VFS_MODE_READ, 0,
                                           SVC_PIPE);
        vfs_fd_t wfd = vfs_open_server_svc((int64_t) rep.words[2],
                                           "/dev/pipe", VFS_MODE_WRITE, 0,
                                           SVC_PIPE);

        if (rfd == VFS_INVALID_FD || wfd == VFS_INVALID_FD) {
            cpu_set_errno(ENOMEM);
            return -1;
        }

        int32_t kfds[2] = { (int32_t) rfd, (int32_t) wfd };

        if (fd == NULL || copy_to_user(fd, kfds, sizeof(kfds)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }

        klogi("k_pipe: server pipe read %ld write %ld\n", (int64_t) rfd,
              (int64_t) wfd);
        return 0;
    }

    /* Pipes are only served from userspace. */
    cpu_set_errno(ENOSYS);

  err_exit:
    return -1;
}

int64_t k_nanosleep(vfs_timespec_t * req, vfs_timespec_t * rem)
{
    vfs_timespec_t kreq = { 0 };

    cpu_set_errno(0);

    if (rem != NULL) {
        vfs_timespec_t zero = { 0, 0 };

        copy_to_user(rem, &zero, sizeof(zero));
    }

    if (req == NULL || copy_from_user(&kreq, req, sizeof(kreq)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    if (kreq.tv_sec < 0 || kreq.tv_nsec < 0 || kreq.tv_nsec >= 1000000000LL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    int64_t ms = kreq.tv_sec * 1000 + (kreq.tv_nsec + 999999) / 1000000;

    sched_sleep(ms);
    return 0;
}

int64_t k_clock_nanosleep(int32_t clockid, int32_t flags,
                          vfs_timespec_t * req, vfs_timespec_t * rem)
{
    vfs_timespec_t kreq = { 0, 0 };

    (void)rem;
    cpu_set_errno(0);

    if (req == NULL || copy_from_user(&kreq, req, sizeof(kreq)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    if (kreq.tv_nsec < 0 || kreq.tv_nsec >= 1000000000LL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    if (flags & 1) {            /* TIMER_ABSTIME */
        vfs_timespec_t now = { 0, 0 };
        int64_t ns;

        k_getclock(NULL, clockid, &now);
        ns = (kreq.tv_sec - now.tv_sec) * 1000000000LL
            + (kreq.tv_nsec - now.tv_nsec);
        if (ns <= 0)
            return 0;
        kreq.tv_sec = ns / 1000000000LL;
        kreq.tv_nsec = ns % 1000000000LL;
    } else if (kreq.tv_sec < 0) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    sched_sleep(kreq.tv_sec * 1000 + (kreq.tv_nsec + 999999) / 1000000);
    return 0;
}

int64_t k_fsync(int64_t fd)
{
    (void)fd;
    cpu_set_errno(0);
    return 0;
}

#define POLLIN      0x001
#define POLLOUT     0x004
#define POLLNVAL    0x020

/* Readiness is not tracked, so every valid descriptor is reported ready.
 * A blocking read still waits in the server. */
static bool fd_is_valid(int32_t fd)
{
    if (fd < 0)
        return false;
    if (fd < 3)
        return true;
    return process_fd_get(fd, NULL, NULL, NULL, NULL, NULL) == 0;
}

int64_t k_poll(void *fds, uint64_t nfds, int32_t timeout)
{
    struct pollfd_k {
        int32_t fd;
        int16_t events;
        int16_t revents;
    };

    cpu_set_errno(0);

    if (fds == NULL || nfds > 1024) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    int32_t ready = 0;

    for (uint64_t i = 0; i < nfds; i++) {
        struct pollfd_k p;
        uint8_t *slot = (uint8_t *) fds + i * sizeof(p);

        if (copy_from_user(&p, slot, sizeof(p)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }

        int16_t rev = 0;

        if (p.fd < 0) {
            rev = 0;
        } else if (fd_is_valid(p.fd)) {
            if (p.events & POLLIN)
                rev |= POLLIN;
            if (p.events & POLLOUT)
                rev |= POLLOUT;
        } else {
            rev = POLLNVAL;
        }

        p.revents = rev;

        if (copy_to_user(slot, &p, sizeof(p)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
        if (rev)
            ready++;
    }

    if (ready == 0 && timeout > 0)
        sched_sleep(timeout);

    return ready;
}

static int64_t select_common(int32_t nfds, uint8_t * rfds, uint8_t * wfds,
                             uint8_t * efds, int64_t timeout_ms)
{
    uint8_t in[3][128] = { {0} };
    uint8_t out[3][128] = { {0} };
    uint8_t *sets[3] = { rfds, wfds, efds };
    uint64_t bytes;

    cpu_set_errno(0);

    if (nfds < 0 || nfds > 1024) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    bytes = ((uint64_t) nfds + 7) / 8;

    for (int32_t s = 0; s < 3; s++) {
        if (sets[s] != NULL && bytes > 0
            && copy_from_user(in[s], sets[s], bytes) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
    }

    int32_t ready = 0;

    for (int32_t fd = 0; fd < nfds; fd++) {
        int32_t bit = fd / 8;
        uint8_t mask = (uint8_t) (1 << (fd % 8));
        bool any = false;

        if (!fd_is_valid(fd))
            continue;

        for (int32_t s = 0; s < 3; s++) {
            if (sets[s] != NULL && (in[s][bit] & mask)) {
                out[s][bit] |= mask;
                any = true;
            }
        }
        if (any)
            ready++;
    }

    for (int32_t s = 0; s < 3; s++) {
        if (sets[s] != NULL && bytes > 0
            && copy_to_user(sets[s], out[s], bytes) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
    }

    if (ready == 0 && timeout_ms > 0)
        sched_sleep(timeout_ms);

    return ready;
}

int64_t k_select(int32_t nfds, uint8_t * rfds, uint8_t * wfds,
                 uint8_t * efds, void *tv)
{
    int64_t ms = 0;

    cpu_set_errno(0);

    if (tv != NULL) {
        struct {
            int64_t sec;
            int64_t usec;
        } ktv;

        if (copy_from_user(&ktv, tv, sizeof(ktv)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
        ms = ktv.sec * 1000 + (ktv.usec + 999) / 1000;
    }

    return select_common(nfds, rfds, wfds, efds, ms);
}

int64_t k_pselect6(int32_t nfds, uint8_t * rfds, uint8_t * wfds,
                   uint8_t * efds, void *tsp, void *sigmask)
{
    int64_t ms = 0;

    (void)sigmask;
    cpu_set_errno(0);

    if (tsp != NULL) {
        vfs_timespec_t ts = { 0, 0 };

        if (copy_from_user(&ts, tsp, sizeof(ts)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
        ms = ts.tv_sec * 1000 + (ts.tv_nsec + 999999) / 1000000;
    }

    return select_common(nfds, rfds, wfds, efds, ms);
}

int64_t k_sched_yield(void)
{
    cpu_set_errno(0);
    sched_sleep(0);
    return 0;
}

int64_t k_prlimit64(int32_t pid, int32_t resource, void *newlim, void *oldlim)
{
    struct {
        uint64_t cur;
        uint64_t max;
    } lim = { ~0ULL, ~0ULL };

    /* Only reading is supported: report RLIM_INFINITY. */
    (void)pid;
    (void)resource;
    (void)newlim;

    cpu_set_errno(0);

    if (oldlim != NULL
        && copy_to_user(oldlim, &lim, sizeof(lim)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    return 0;
}

int64_t k_clone(uint64_t flags, uint64_t stack, int32_t * ptid,
                int32_t * ctid, uint64_t tls)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);


    if (t == NULL || t->mode != PROC_USER_MODE || t->addrspace == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    /* Only thread creation shares the address space. */
    if (!(flags & CLONE_VM)) {
        cpu_set_errno(ENOSYS);
        return -1;
    }

    cpu_t *cpu = smp_get_current_cpu(false);
    if (cpu == NULL || cpu->syscall_frame == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    process_t *tc = process_clone(t, flags, stack, tls, ctid,
                                  cpu->syscall_frame);
    if (tc == NULL) {
        cpu_set_errno(ENOMEM);
        return -1;
    }

    pid_t tid = tc->pid;

    if ((flags & CLONE_PARENT_SETTID) && ptid != NULL)
        copy_to_user(ptid, &tid, sizeof(tid));
    if ((flags & CLONE_CHILD_SETTID) && ctid != NULL)
        copy_to_user(ctid, &tid, sizeof(tid));

    sched_add(tc);
    return tid;
}

int64_t k_fork()
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    if (t == NULL) {
        cpu_set_errno(ENODEV);
        goto err_exit;
    }

    if (t->pid < 1) {
        cpu_set_errno(ESRCH);
        goto err_exit;
    }

    pid_t tid_child = sched_fork();
    process_t *curr_proc = sched_get_current_process();

    klogd("k_fork: parent process id #%ld, current process id #%ld, PML4 0x%016lx, "
          "sched_fork() returns #%ld\n",
          t->pid, sched_get_pid(), curr_proc->addrspace->PML4, tid_child);

    if (tid_child == PID_MAX) {
        cpu_set_errno(ECHILD);
        return -1;
    } else if (t->pid == sched_get_pid()) {
        /* Parent: the process server clones the child's fd table, then the
         * child is allowed to run. */
        process_fd_fork((int32_t) t->pid, (int32_t) tid_child);
        sched_mark_fds_ready(tid_child);
        klogd("k_fork: return %ld from parent process #%ld\n", tid_child,
              t->pid);
        return tid_child;
    } else {
        /* Child: wait until the parent has cloned our fd table. */
        klogd("k_fork: return 0 from child process #%ld\n", tid_child);
        sched_wait_fds_ready(curr_proc);
        return 0;
    }
  err_exit:
    return -1;
}

int64_t k_getppid()
{
    cpu_set_errno(ENOSYS);
    return -1;
}

int64_t k_fcntl(int64_t fd, int64_t request, int64_t arg)
{
    cpu_set_errno(0);

    /* Only the descriptor flags live in the process server for now. */
    if (request == F_GETFD || request == F_SETFD) {
        int64_t r = process_fd_fcntl((int32_t) fd, (int32_t) request, arg);

        if (r < 0) {
            cpu_set_errno(EBADF);
            return -1;
        }
        return r;
    }

    cpu_set_errno(ENOSYS);
    return -1;
}

int64_t k_waitpid(int64_t pid, int32_t * status, int32_t flags)
{
    process_t *parent = sched_get_current_process();

    if (status != NULL && clear_user(status, sizeof(*status)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    if (parent == NULL) {
        cpu_set_errno(ECHILD);
        return -1;
    }

    /* pid is passed as a 32-bit signed value, so truncate before comparing:
     * a userland wait(-1) arrives as 0x00000000FFFFFFFF. */
    int32_t wpid = (int32_t) pid;
    bool nohang = (flags & WNOHANG) != 0;

    while (true) {
        int64_t st = 0;
        int64_t r = process_wait((int32_t) wpid, nohang ? 1 : 0, &st);

        if (r > 0) {
            if (status != NULL) {
                int32_t st32 = (int32_t) st;
                if (copy_to_user(status, &st32, sizeof(st32)) != 0) {
                    cpu_set_errno(EFAULT);
                    return -1;
                }
            }
            cpu_set_errno(0);
            return r;
        }

        if (r == 0) {           /* WNOHANG: a child exists but has not exited */
            cpu_set_errno(0);
            return 0;
        }

        if (r == PROC_WAIT_BLOCK) {
            /* A child exists but has not exited. The server records the status;
             * sleep briefly and ask again. */
            sched_wait_child(10);
            continue;
        }

        cpu_set_errno(ECHILD);
        return -1;
    }
}

void k_exit(int64_t status)
{
    process_t *t = sched_get_current_process();
    if (t != NULL) {
        if (t->is_thread) {
            klogi("k_exit: thread %ld in group %ld exit\n", t->pid, t->tgid);
            /* CLONE_CHILD_CLEARTID: release the joiner. */
            if (t->clear_child_tid != NULL) {
                int32_t zero = 0;

                copy_to_user(t->clear_child_tid, &zero, sizeof(zero));
                sched_wake_key((void *) t->clear_child_tid);
            }
        } else {
            klogi("k_exit: process %ld exit with status %ld\n", t->pid,
                  status);
            /* The process server owns the fd table and closes the server side
             * of every descriptor the process held. */
            process_fd_exit((int32_t) t->pid);
            /* Record the exit status so the parent's wait can collect it. */
            process_exit_notify(status);
        }
    }

    /* Exit from scheduler */
    sched_exit(status);
}

void k_exit_group(int64_t status)
{
    process_t *t = sched_get_current_process();

    if (t != NULL) {
        klogi("k_exit_group: group %ld exit with status %ld\n", t->tgid,
              status);
        /* Stop the other threads of the group first. */
        sched_kill_group(t->tgid, t->pid);

        if (!t->is_thread) {
            process_fd_exit((int32_t) t->pid);
            process_exit_notify(status);
        }
    }

    sched_exit(status);
}

int32_t k_getcwd(char *buffer, uint64_t size)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    if (buffer == NULL || size <= 0) {
        cpu_set_errno(EINVAL);
        goto err_exit;
    }

    if (t == NULL) {
        cpu_set_errno(ENODEV);
        goto err_exit;
    }

    if (t->pid < 1) {
        cpu_set_errno(ESRCH);
        goto err_exit;
    }

    uint64_t len = strlen(t->cwd);
    if (len < size - 1) {
        if (copy_to_user(buffer, t->cwd, len + 1) != 0) {
            cpu_set_errno(EFAULT);
            goto err_exit;
        }
    } else {
        cpu_set_errno(ENAMETOOLONG);
        goto err_exit;
    }

    return 0;

  err_exit:
    return -1;
}

int32_t k_getrusage(int64_t who, uint64_t usage)
{
    /* When gcc is launched, it will call getrusage(). We need to dive into
     * gcc to know the purpose of this function call.
     */
    klogw("SYSCALL: get 0x%016lx rusage\n", who);

    if (clear_user((void *) usage, sizeof(rusage_t)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    return 0;
}

/* Pack path/cwd/argv/envp and the ELF file into one buffer and hand it to the
 * process server, which parses and maps it. Returns 0 on success. */
static int32_t exec_via_server(process_t * t, const char *kpath,
                           const char **argv, const char **envp,
                           const char *cwd)
{
    uint64_t argc = 0, envc = 0, i;

    while (argv != NULL && argv[argc] != NULL && argc < EXEC_MAX_ARGS)
        argc++;
    while (envp != NULL && envp[envc] != NULL && envc < EXEC_MAX_ARGS)
        envc++;

    uint8_t *elf = NULL;
    uint64_t elf_len = 0;

    if (vfs_load_file(kpath, &elf, &elf_len) != 0 || elf == NULL) {
        kloge("k_execve: cannot read \"%s\"\n", kpath);
        return -1;
    }

    uint64_t header = 8 * sizeof(uint64_t);
    uint64_t path_len = strlen(kpath) + 1;
    uint64_t cwd_len = ((cwd != NULL) ? strlen(cwd) : 0) + 1;
    uint64_t off_argv = header + path_len + cwd_len;
    uint64_t off_envp = off_argv + argc * sizeof(uint64_t);
    uint64_t off_strs = off_envp + envc * sizeof(uint64_t);

    uint64_t strs = 0;

    for (i = 0; i < argc; i++)
        strs += strlen(argv[i]) + 1;
    for (i = 0; i < envc; i++)
        strs += strlen(envp[i]) + 1;

    uint64_t off_elf = off_strs + strs;
    uint64_t total = off_elf + elf_len;
    uint8_t *buf = kmalloc(total);

    if (buf == NULL) {
        kmfree_chunk(elf, __func__, __LINE__);
        return -1;
    }

    uint64_t *h = (uint64_t *) buf;

    memset(buf, 0, header);
    h[0] = header;
    h[1] = header + path_len;
    h[2] = off_argv;
    h[3] = off_envp;
    h[4] = argc;
    h[5] = envc;
    h[6] = off_elf;
    h[7] = elf_len;
    memcpy(buf + header, kpath, path_len);
    memcpy(buf + header + path_len, (cwd != NULL) ? cwd : "", cwd_len);

    uint64_t *av = (uint64_t *) (buf + off_argv);
    uint64_t *ev = (uint64_t *) (buf + off_envp);
    uint8_t *p = buf + off_strs;

    for (i = 0; i < argc; i++) {
        uint64_t n = strlen(argv[i]) + 1;
        memcpy(p, argv[i], n);
        av[i] = (uint64_t) (p - buf);
        p += n;
    }
    for (i = 0; i < envc; i++) {
        uint64_t n = strlen(envp[i]) + 1;
        memcpy(p, envp[i], n);
        ev[i] = (uint64_t) (p - buf);
        p += n;
    }
    memcpy(buf + off_elf, elf, elf_len);
    kmfree_chunk(elf, __func__, __LINE__);

    handle_t bh = 0;

    if (ipc_buf_from_kernel(buf, total, &bh) != 0) {
        kloge("k_execve: ipc_buf_from_kernel failed\n");
        kmfree(buf);
        return -1;
    }
    kmfree(buf);

    ipc_msg_t req, rep;

    memset(&req, 0, sizeof(req));
    memset(&rep, 0, sizeof(rep));
    req.tag = PROC_EXEC;
    req.words[0] = (uint64_t) t->pid;
    req.xfer[0] = bh;
    req.xfer_count = 1;

    if (!router_forward(SVC_PROC, &req, &rep)) {
        kloge("k_execve: router_forward(SVC_PROC) failed\n");
        return -1;
    }
    if ((int64_t) rep.words[0] < 0) {
        kloge("k_execve: process server returned %ld\n",
              (int64_t) rep.words[0]);
        return -1;
    }
    return 0;
}

int64_t k_execve(const char *path, const char *argv[], const char *envp[])
{
    char *cwd = NULL;
    process_t *t = sched_get_current_process();
    if (t != NULL)
        cwd = t->cwd;

    char kpath[VFS_MAX_PATH_LEN] = { 0 };
    if (path == NULL || strncpy_from_user(kpath, path, sizeof(kpath)) < 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    char *kargv[EXEC_MAX_ARGS + 1];
    char *kenvp[EXEC_MAX_ARGS + 1];

    if (copy_exec_argv(argv, kargv) != 0
        || copy_exec_argv(envp, kenvp) != 0) {
        free_exec_argv(kargv);
        free_exec_argv(kenvp);
        cpu_set_errno(EFAULT);
        return -1;
    }

    const char **kargv_p = (argv != NULL) ? (const char **) kargv : NULL;
    const char **kenvp_p = (envp != NULL) ? (const char **) kenvp : NULL;

    /* The process server parses and maps the image. Fall back to the in-kernel
     * loader when the server is not registered. */
    if (t != NULL && router_lookup(SVC_PROC) != NULL
        && exec_via_server(t, kpath, kargv_p, kenvp_p, cwd) == 0) {
        klogi("k_execve: server ran \"%s\", exit process %ld\n", kpath, t->pid);
        free_exec_argv(kargv);
        free_exec_argv(kenvp);
        process_fd_exit((int32_t) t->pid);
        process_exit_notify(0);
        sched_exit(0);
        cpu_set_errno(0);
        return 0;
    }

    if (sched_execve(kpath, kargv_p, kenvp_p, cwd) != NULL) {
        klogi("k_execve: run \"%s\" and exit from process %ld\n", kpath,
              t != NULL ? t->pid : 0);
        free_exec_argv(kargv);
        free_exec_argv(kenvp);
        process_fd_exit((int32_t) (t != NULL ? t->pid : 0));
        sched_exit(0);
        cpu_set_errno(0);
        return 0;
    }

    free_exec_argv(kargv);
    free_exec_argv(kenvp);
    cpu_set_errno(EINVAL);
    return -1;
}

int32_t k_getclock(void *_, int64_t which, vfs_timespec_t * out)
{
    (void) _;

    vfs_timespec_t ts = { 0 };
    cpu_set_errno(0);

    uint64_t now_sec = hpet_get_nanos() / 1000000000;
    uint64_t now_ns = hpet_get_nanos();

    time_t boot_time = cmos_boot_time();

    switch (which) {
    case CLOCK_REALTIME:
    case CLOCK_REALTIME_COARSE:
        ts = (vfs_timespec_t) {
        .tv_sec = now_sec + boot_time,.tv_nsec =
                now_ns + boot_time * 1000000000};
        break;
    case CLOCK_BOOTTIME:
    case CLOCK_MONOTONIC:
    case CLOCK_MONOTONIC_RAW:
    case CLOCK_MONOTONIC_COARSE:
        ts = (vfs_timespec_t) {
        .tv_sec = now_sec,.tv_nsec = now_ns};
        break;
    case CLOCK_PROCESS_CPUTIME_ID:
    case CLOCK_THREAD_CPUTIME_ID:
        ts = (vfs_timespec_t) {
        .tv_sec = 0,.tv_nsec = 0};
        break;
    default:
        cpu_set_errno(EINVAL);
        return -1;
    }

    if (copy_to_user(out, &ts, sizeof(ts)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    return 0;
}

int64_t k_readlink(int64_t dirfd, const char *path, void *buffer,
                   uint64_t max_size)
{
    (void) dirfd;
    (void) path;
    (void) buffer;
    (void) max_size;
    cpu_set_errno(ENOSYS);
    return -1;
}

void k_uname(void)
{
}

int64_t k_dup3(int64_t oldfd, int64_t newfd, int64_t flags)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    if (t == NULL) {
        cpu_set_errno(ENOSYS);
        return -1;
    }

    /* Linux dup3(oldfd, newfd, flags): make newfd refer to oldfd. newfd < 0
     * means the caller wants the lowest free descriptor (dup). */
    int64_t r = process_fd_dup((int32_t) oldfd, (int32_t) newfd);

    if (r < 0) {
        cpu_set_errno(EBADF);
        return -1;
    }

    if ((flags & O_CLOEXEC) && r >= 0)
        process_fd_fcntl((int32_t) r, F_SETFD, FD_CLOEXEC);

    return r;
}

/* TODO: Need to add a futex implementation. */
int64_t k_futex_wait(int64_t * ptr, vfs_timespec_t * tv, int64_t expected)
{
    int64_t val = 0;
    int64_t millis = 0;

    cpu_set_errno(0);

    if (ptr == NULL || copy_from_user(&val, ptr, sizeof(val)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    if (val != expected) {
        cpu_set_errno(EAGAIN);
        return -1;
    }

    if (tv != NULL) {
        vfs_timespec_t ktv = { 0 };

        if (copy_from_user(&ktv, tv, sizeof(ktv)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
        millis = ktv.tv_sec * 1000 + ktv.tv_nsec / 1000000;
    }

    sched_wait_key_begin(ptr);

    /* Re-check after arming: a wake between the first check and the arm must
     * not be lost. */
    if (copy_from_user(&val, ptr, sizeof(val)) != 0) {
        sched_wait_key_cancel();
        cpu_set_errno(EFAULT);
        return -1;
    }
    if (val != expected) {
        sched_wait_key_cancel();
        cpu_set_errno(EAGAIN);
        return -1;
    }

    if (tv == NULL)
        sched_wait_key_commit_infinite();
    else
        sched_wait_key_commit(millis);

    return 0;
}

int64_t k_futex_wake(int64_t * ptr, int64_t nr)
{
    cpu_set_errno(0);

    if (ptr == NULL) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    return sched_wake_key_n(ptr, nr);
}

/* ----- Microkernel: endpoints, IPC and handles ----- */

int64_t k_ep_create(void)
{
    process_t *t = sched_get_current_process();
    if (t == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    endpoint_t *ep = endpoint_create();
    if (ep == NULL) {
        cpu_set_errno(ENOMEM);
        return -1;
    }

    handle_t h = handle_alloc(&t->handles, endpoint_object(ep),
                              HANDLE_RIGHT_SEND | HANDLE_RIGHT_RECV
                              | HANDLE_RIGHT_TRANSFER);
    /* The creation reference is dropped; the handle owns the object now. */
    object_unref(endpoint_object(ep));

    if (h == HANDLE_INVALID) {
        cpu_set_errno(ENOMEM);
        return -1;
    }

    cpu_set_errno(0);
    return (int64_t) h;
}

static endpoint_t *k_ipc_resolve(int64_t handle, uint32_t rights)
{
    process_t *t = sched_get_current_process();
    if (t == NULL)
        return NULL;

    kernel_object_t *o = handle_get(&t->handles, (handle_t) handle, rights);
    if (o == NULL || o->type != OBJ_ENDPOINT)
        return NULL;

    return (endpoint_t *) o->impl;
}

/* Rights a receiver gets for a handle the sender transfers. The sender's own
 * rights are not intersected yet (there is no per-handle read of them). */
static uint32_t k_transfer_rights(obj_type_t type)
{
    switch (type) {
    case OBJ_MEMORY:
        return HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE | HANDLE_RIGHT_MAP;
    case OBJ_ENDPOINT:
        return HANDLE_RIGHT_SEND | HANDLE_RIGHT_RECV;
    case OBJ_IRQ:
        return HANDLE_RIGHT_READ;
    default:
        return HANDLE_RIGHT_READ;
    }
}

int64_t k_ipc_send(int64_t handle, void *umsg)
{
    endpoint_t *ep = k_ipc_resolve(handle, HANDLE_RIGHT_SEND);
    process_t *t = sched_get_current_process();

    if (ep == NULL || t == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    ipc_msg_t m;
    if (copy_from_user(&m, umsg, sizeof(m)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    if (m.xfer_count > 2)
        m.xfer_count = 2;

    /* Move each transferred handle out of the sender's table to the queued
     * message. Move semantics: the sender's handle is closed. */
    kernel_object_t *objs[2];
    uint32_t rights[2];
    uint8_t n = 0;

    for (uint8_t i = 0; i < m.xfer_count; i++) {
        kernel_object_t *o =
            handle_get(&t->handles, m.xfer[i], HANDLE_RIGHT_TRANSFER);
        if (o == NULL) {
            for (uint8_t k = 0; k < n; k++)
                object_unref(objs[k]);
            cpu_set_errno(EINVAL);
            return -1;
        }
        object_ref(o);          /* the reference carried by the message */
        handle_close(&t->handles, m.xfer[i]);
        objs[n] = o;
        rights[n] = k_transfer_rights(o->type);
        m.xfer[i] = 0;
        n++;
    }

    if (ipc_send_objs(ep, &m, objs, rights, n) != 0) {
        for (uint8_t k = 0; k < n; k++)
            object_unref(objs[k]);
        cpu_set_errno(EAGAIN);
        return -1;
    }

    cpu_set_errno(0);
    return 0;
}

/* Common receive path: install any handles the sender moved onto this message
 * into the receiver's table, then return the (rewritten) message to user space.
 * mode 0 = blocking, 1 = timeout, 2 = non-blocking. */
static int64_t k_ipc_recv_common(endpoint_t *ep, void *umsg, int32_t mode,
                                 time_t timeout)
{
    process_t *t = sched_get_current_process();
    if (t == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    ipc_msg_t m;
    kernel_object_t *objs[2];
    uint32_t rights[2];
    uint8_t n = 0;
    int32_t r;

    if (mode == 0)
        r = ipc_recv_objs(ep, &m, objs, rights, &n);
    else
        r = ipc_recv_timeout_objs(ep, &m, objs, rights, &n, timeout);

    if (r != 0) {
        cpu_set_errno(EAGAIN);
        return -1;
    }

    m.xfer_count = n;
    for (uint8_t i = 0; i < n; i++) {
        handle_t h = handle_alloc(&t->handles, objs[i], rights[i]);
        object_unref(objs[i]);  /* the handle owns the object now */
        m.xfer[i] = (h == HANDLE_INVALID) ? 0 : h;
    }

    if (copy_to_user(umsg, &m, sizeof(m)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    cpu_set_errno(0);
    return 0;
}

int64_t k_ipc_recv(int64_t handle, void *umsg)
{
    endpoint_t *ep = k_ipc_resolve(handle, HANDLE_RIGHT_RECV);
    if (ep == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    return k_ipc_recv_common(ep, umsg, 0, 0);
}

int64_t k_ipc_call(int64_t handle, void *ureq, void *urep)
{
    endpoint_t *ep = k_ipc_resolve(handle, HANDLE_RIGHT_SEND);
    if (ep == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    ipc_msg_t req, rep;
    if (copy_from_user(&req, ureq, sizeof(req)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    if (ipc_call(ep, &req, &rep) != 0) {
        cpu_set_errno(EAGAIN);
        return -1;
    }

    if (copy_to_user(urep, &rep, sizeof(rep)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    cpu_set_errno(0);
    return 0;
}

int64_t k_ipc_reply(int64_t handle, void *umsg)
{
    endpoint_t *ep = k_ipc_resolve(handle, HANDLE_RIGHT_SEND);
    if (ep == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    ipc_msg_t m;
    if (copy_from_user(&m, umsg, sizeof(m)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    if (ipc_send(ep, &m) != 0) {
        cpu_set_errno(EAGAIN);
        return -1;
    }

    cpu_set_errno(0);
    return 0;
}

int64_t k_handle_close(int64_t handle)
{
    process_t *t = sched_get_current_process();
    if (t == NULL || handle_close(&t->handles, (handle_t) handle) != 0) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    cpu_set_errno(0);
    return 0;
}

int64_t k_irq_bind(int64_t irq_handle, int64_t ep_handle)
{
    process_t *t = sched_get_current_process();
    if (t == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    kernel_object_t *io =
        handle_get(&t->handles, (handle_t) irq_handle, 0);
    kernel_object_t *ep =
        handle_get(&t->handles, (handle_t) ep_handle, HANDLE_RIGHT_RECV);

    if (io == NULL || io->type != OBJ_IRQ || ep == NULL
        || ep->type != OBJ_ENDPOINT) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    irq_bind((irq_obj_t *) io->impl, (endpoint_t *) ep->impl);
    cpu_set_errno(0);
    return 0;
}

int64_t k_irq_ack(int64_t irq_handle)
{
    process_t *t = sched_get_current_process();
    if (t == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    kernel_object_t *io =
        handle_get(&t->handles, (handle_t) irq_handle, 0);
    if (io == NULL || io->type != OBJ_IRQ) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    irq_ack((irq_obj_t *) io->impl);
    cpu_set_errno(0);
    return 0;
}

int64_t k_ioport_access(int64_t op, int64_t port, int64_t width,
                        int64_t value)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    bool allowed = false;
    if (t != NULL) {
        for (uint8_t i = 0; i < t->io_port_count; i++) {
            if (port >= t->io_ports[i].first
                && port <= t->io_ports[i].last) {
                allowed = true;
                break;
            }
        }
    }

    if (!allowed) {
        cpu_set_errno(EPERM);
        return -1;
    }

    if (op == 0) {              /* input */
        uint64_t v = 0;
        if (width == 1)
            v = port_inb((uint16_t) port);
        else if (width == 2)
            v = port_inw((uint16_t) port);
        else
            v = port_ind((uint16_t) port);
        return (int64_t) v;
    }

    if (width == 1)
        port_outb((uint16_t) port, (uint8_t) value);
    else if (width == 2)
        port_outw((uint16_t) port, (uint16_t) value);
    else
        port_outd((uint16_t) port, (uint32_t) value);

    return 0;
}

int64_t k_ipc_recv_nb(int64_t handle, void *umsg)
{
    endpoint_t *ep = k_ipc_resolve(handle, HANDLE_RIGHT_RECV);
    if (ep == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    return k_ipc_recv_common(ep, umsg, 2, 0);
}

int64_t k_ipc_recv_timeout(int64_t handle, void *umsg, int64_t timeout)
{
    endpoint_t *ep = k_ipc_resolve(handle, HANDLE_RIGHT_RECV);
    if (ep == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    return k_ipc_recv_common(ep, umsg, 1, (time_t) timeout);
}

int64_t k_bootinfo(void *ubi)
{
    process_t *t = sched_get_current_process();

    if (t == NULL || t->bootinfo == NULL) {
        cpu_set_errno(ENOENT);
        return -1;
    }

    if (copy_to_user(ubi, t->bootinfo, sizeof(bootinfo_t)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    cpu_set_errno(0);
    return 0;
}

int64_t k_mem_alloc(int64_t size)
{
    process_t *t = sched_get_current_process();

    if (t == NULL || size <= 0) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    memobj_t *m = memobj_create((uint64_t) size);
    if (m == NULL) {
        cpu_set_errno(ENOMEM);
        return -1;
    }

    handle_t h = handle_alloc(&t->handles, memobj_object(m),
                              HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE
                              | HANDLE_RIGHT_MAP | HANDLE_RIGHT_TRANSFER);
    object_unref(memobj_object(m));

    if (h == HANDLE_INVALID) {
        cpu_set_errno(ENOMEM);
        return -1;
    }

    cpu_set_errno(0);
    return (int64_t) h;
}

static memobj_t *k_mem_resolve(int64_t handle, uint32_t rights)
{
    process_t *t = sched_get_current_process();
    if (t == NULL)
        return NULL;

    kernel_object_t *o = handle_get(&t->handles, (handle_t) handle, rights);
    if (o == NULL || o->type != OBJ_MEMORY)
        return NULL;

    return (memobj_t *) o->impl;
}

int64_t k_mem_map(int64_t handle, uint64_t vaddr, int64_t prot)
{
    process_t *t = sched_get_current_process();
    memobj_t *m = k_mem_resolve(handle, HANDLE_RIGHT_MAP);

    if (t == NULL || m == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    if (memobj_map(m, t->addrspace, vaddr, (uint32_t) prot) != 0) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    cpu_set_errno(0);
    return 0;
}

int64_t k_mem_unmap(int64_t handle, uint64_t vaddr)
{
    process_t *t = sched_get_current_process();
    memobj_t *m = k_mem_resolve(handle, HANDLE_RIGHT_MAP);
    if (t == NULL || m == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    if (memobj_unmap(m, t->addrspace, vaddr) != 0) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    cpu_set_errno(0);
    return 0;
}

/* Physical address of a mapped memory object at a byte offset. Used by device
 * servers that program DMA. */
int64_t k_mem_phys(int64_t handle, uint64_t offset)
{
    process_t *t = sched_get_current_process();
    memobj_t *m = k_mem_resolve(handle, HANDLE_RIGHT_MAP);

    if (t == NULL || m == NULL || offset >= memobj_size(m)) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    cpu_set_errno(0);
    return (int64_t) (memobj_page(m, offset / PAGE_SIZE)
                      + (offset & (PAGE_SIZE - 1)));
}

int64_t k_handle_dup(int64_t handle)
{
    process_t *t = sched_get_current_process();
    if (t == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }
    handle_t h = handle_dup(&t->handles, (handle_t) handle);
    if (h == HANDLE_INVALID) {
        cpu_set_errno(EBADF);
        return -1;
    }

    cpu_set_errno(0);
    return (int64_t) h;
}

/* --- Process services primitives (used by the process server) ------------- */

/* Create an empty user process, child of parent_pid. It is not scheduled until
 * PROC_START. */
int64_t k_proc_spawn(int64_t parent_pid, const char *name)
{
    cpu_set_errno(0);

    char kname[64];
    if (name == NULL || strncpy_from_user(kname, name, sizeof(kname)) < 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    process_t *parent = process_lookup((pid_t) parent_pid);
    process_t *tc = process_make(kname, NULL, 0, PROC_USER_MODE,
                                 parent == NULL ? NULL : parent->addrspace);
    if (tc == NULL) {
        cpu_set_errno(ENOMEM);
        return -1;
    }

    if (parent != NULL) {
        tc->ppid = parent->pid;
        strcpy(tc->cwd, parent->cwd);
    }

    return (int64_t) tc->pid;
}

/* Copy the pages of memory object memh into process pid at vaddr. The copy
 * keeps the pages owned by the target process, so the server can drop the
 * object right away. */
int64_t k_proc_map(int64_t pid, uint64_t vaddr, int64_t memh, int64_t prot)
{
    cpu_set_errno(0);

    process_t *cur = sched_get_current_process();
    process_t *t = process_lookup((pid_t) pid);

    if (cur == NULL || t == NULL || t->addrspace == NULL
        || (vaddr & (PAGE_SIZE - 1))) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    kernel_object_t *o = handle_get(&cur->handles, (handle_t) memh,
                                    HANDLE_RIGHT_MAP);
    if (o == NULL || o->type != OBJ_MEMORY) {
        cpu_set_errno(EBADF);
        return -1;
    }

    memobj_t *m = (memobj_t *) o->impl;
    uint64_t np = memobj_page_count(m);
    uint64_t pf = VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE;
    if (prot & PROT_WRITE)
        pf |= VMM_FLAG_READWRITE;

    for (uint64_t i = 0; i < np; i++) {
        uint64_t dst =
            VIRT_TO_PHYS(kmalloc_chunk(PAGE_SIZE, __func__, __LINE__));
        if (dst == 0) {
            cpu_set_errno(ENOMEM);
            return -1;
        }
        memcpy((void *)PHYS_TO_VIRT(dst),
               (void *)PHYS_TO_VIRT(memobj_page(m, i)), PAGE_SIZE);
        vmm_map(t->addrspace, vaddr + i * PAGE_SIZE, dst, 1, pf);

        mem_map_t mm = {
            .vaddr = vaddr + i * PAGE_SIZE,
            .paddr = dst,
            .np = 1,
            .flags = pf,
        };
        vec_push_back(&t->addrspace->mmap_list, mm);
    }

    return 0;
}

/* Set process pid's initial instruction pointer and stack pointer. */
int64_t k_proc_set_entry(int64_t pid, uint64_t rip, uint64_t rsp)
{
    cpu_set_errno(0);

    process_t *t = process_lookup((pid_t) pid);
    if (t == NULL || t->context == NULL) {
        cpu_set_errno(ESRCH);
        return -1;
    }

    process_regs_t *regs = (process_regs_t *) PHYS_TO_VIRT((uint64_t) t->context);
    regs->rip = rip;
    if (rsp != 0)
        regs->rsp = rsp;
    return 0;
}

/* Schedule a process created by PROC_SPAWN. */
int64_t k_proc_start(int64_t pid)
{
    cpu_set_errno(0);

    process_t *t = process_lookup((pid_t) pid);
    if (t == NULL) {
        cpu_set_errno(ESRCH);
        return -1;
    }

    sched_add(t);
    return 0;
}

/* --- network sockets ----------------------------------------------------- */

int64_t k_socket(int64_t domain, int64_t type, int64_t protocol)
{
    cpu_set_errno(0);

    if (net_server_active() == false) {
        cpu_set_errno(EAFNOSUPPORT);
        return -1;
    }

    int64_t fd = net_socket((int32_t) domain, (int32_t) type, (int32_t) protocol);

    if (fd < 0) {
        cpu_set_errno(EAFNOSUPPORT);
        return -1;
    }
    return fd;
}

int64_t k_bind(int64_t sock, int64_t ip, int64_t port)
{
    cpu_set_errno(0);

    if (net_bind((int32_t) sock, (uint32_t) ip, (uint16_t) port) < 0) {
        cpu_set_errno(EINVAL);
        return -1;
    }
    return 0;
}

int64_t k_sendto(int64_t sock, int64_t ip, int64_t port, void *buf,
                 uint64_t len)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    if (buf != NULL && len > 0 && !user_range_ok(t, buf, len)) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    int64_t n = net_sendto((int32_t) sock, (uint32_t) ip, (uint16_t) port, buf,
                           len);

    if (n < 0) {
        cpu_set_errno(EIO);
        return -1;
    }
    return n;
}

int64_t k_recvfrom(int64_t sock, void *buf, uint64_t len, void *ip_ptr,
                   void *port_ptr)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    if (buf != NULL && len > 0 && !user_range_ok(t, buf, len)) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    uint32_t ip = 0;
    uint16_t port = 0;
    int64_t n = net_recvfrom((int32_t) sock, buf, len, &ip, &port);

    if (n < 0) {
        cpu_set_errno(EIO);
        return -1;
    }

    if (ip_ptr != NULL
        && copy_to_user(ip_ptr, &ip, sizeof(ip)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    if (port_ptr != NULL
        && copy_to_user(port_ptr, &port, sizeof(port)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    return n;
}

int64_t k_socket_close(int64_t sock)
{
    cpu_set_errno(0);

    if (net_close((int32_t) sock) < 0) {
        cpu_set_errno(EBADF);
        return -1;
    }
    return 0;
}

int64_t k_connect(int64_t sock, int64_t ip, int64_t port)
{
    cpu_set_errno(0);

    if (net_connect((int32_t) sock, (uint32_t) ip, (uint16_t) port) < 0) {
        cpu_set_errno(EIO);
        return -1;
    }
    return 0;
}

int64_t k_listen(int64_t sock, int64_t backlog)
{
    cpu_set_errno(0);

    if (net_listen((int32_t) sock, (int32_t) backlog) < 0) {
        cpu_set_errno(EINVAL);
        return -1;
    }
    return 0;
}

int64_t k_accept(int64_t sock)
{
    cpu_set_errno(0);

    int64_t fd = net_accept((int32_t) sock);

    if (fd < 0) {
        cpu_set_errno(EBADF);
        return -1;
    }
    return fd;
}

syscall_ptr_t syscall_funcs[SYSCALL_TABLE_SIZE] = {
    [SYSCALL_READ] = (syscall_ptr_t) k_read,
    [SYSCALL_WRITE] = (syscall_ptr_t) k_write,
    [SYSCALL_CLOSE] = (syscall_ptr_t) k_close,
    [SYSCALL_FSTAT] = (syscall_ptr_t) k_fstat,
    [SYSCALL_LSEEK] = (syscall_ptr_t) k_seek,
    [SYSCALL_MMAP] = (syscall_ptr_t) k_vm_map,
    [SYSCALL_MUNMAP] = (syscall_ptr_t) k_vm_unmap,
    [SYSCALL_RT_SIGACTION] = (syscall_ptr_t) k_sigaction,
    [SYSCALL_RT_SIGPROCMASK] = (syscall_ptr_t) k_sigprocmask,
    [SYSCALL_IOCTL] = (syscall_ptr_t) k_ioctl,
    [SYSCALL_PIPE] = (syscall_ptr_t) k_pipe,
    [SYSCALL_NANOSLEEP] = (syscall_ptr_t) k_nanosleep,
    [SYSCALL_CLOCK_NANOSLEEP] = (syscall_ptr_t) k_clock_nanosleep,
    [SYSCALL_SCHED_YIELD] = (syscall_ptr_t) k_sched_yield,
    [SYSCALL_FSYNC] = (syscall_ptr_t) k_fsync,
    [SYSCALL_FDATASYNC] = (syscall_ptr_t) k_fsync,
    [SYSCALL_POLL] = (syscall_ptr_t) k_poll,
    [SYSCALL_SELECT] = (syscall_ptr_t) k_select,
    [SYSCALL_PSELECT6] = (syscall_ptr_t) k_pselect6,
    [SYSCALL_GETPID] = (syscall_ptr_t) k_getpid,
    [SYSCALL_SOCKET] = (syscall_ptr_t) k_socket,
    [SYSCALL_CONNECT] = (syscall_ptr_t) k_connect,
    [SYSCALL_ACCEPT] = (syscall_ptr_t) k_accept,
    [SYSCALL_SENDTO] = (syscall_ptr_t) k_sendto,
    [SYSCALL_RECVFROM] = (syscall_ptr_t) k_recvfrom,
    [SYSCALL_BIND] = (syscall_ptr_t) k_bind,
    [SYSCALL_LISTEN] = (syscall_ptr_t) k_listen,
    [SYSCALL_CLONE] = (syscall_ptr_t) k_clone,
    [SYSCALL_FORK] = (syscall_ptr_t) k_fork,
    [SYSCALL_EXECVE] = (syscall_ptr_t) k_execve,
    [SYSCALL_EXIT] = (syscall_ptr_t) k_exit,
    [SYSCALL_EXIT_GROUP] = (syscall_ptr_t) k_exit_group,
    [SYSCALL_WAIT4] = (syscall_ptr_t) k_waitpid,
    [SYSCALL_UNAME] = (syscall_ptr_t) k_uname,
    [SYSCALL_FCNTL] = (syscall_ptr_t) k_fcntl,
    [SYSCALL_GETCWD] = (syscall_ptr_t) k_getcwd,
    [SYSCALL_CHDIR] = (syscall_ptr_t) k_chdir,
    [SYSCALL_UNLINK] = (syscall_ptr_t) k_unlink,
    [SYSCALL_READLINK] = (syscall_ptr_t) k_readlink,
    [SYSCALL_GETRUSAGE] = (syscall_ptr_t) k_getrusage,
    [SYSCALL_GETPPID] = (syscall_ptr_t) k_getppid,
    [SYSCALL_GETDENTS64] = (syscall_ptr_t) k_readdir,
    [SYSCALL_CLOCK_GETTIME] = (syscall_ptr_t) k_getclock,
    [SYSCALL_OPENAT] = (syscall_ptr_t) k_openat,
    [SYSCALL_NEWFSTATAT] = (syscall_ptr_t) k_fstatat,
    [SYSCALL_FACCESSAT] = (syscall_ptr_t) k_faccessat,
    [SYSCALL_DUP3] = (syscall_ptr_t) k_dup3,
    [SYSCALL_PRLIMIT64] = (syscall_ptr_t) k_prlimit64,
    [SYSCALL_GETRANDOM] = (syscall_ptr_t) k_getentropy,
    [SYSCALL_DEBUGLOG] = (syscall_ptr_t) k_debug_log,
    [SYSCALL_SET_FS_BASE] = (syscall_ptr_t) k_set_fs_base,
    [SYSCALL_MEMINFO] = (syscall_ptr_t) k_meminfo,
    [SYSCALL_RUNCMD] = (syscall_ptr_t) k_runcmd,
    [SYSCALL_CHMOD] = (syscall_ptr_t) k_chmod,
    [SYSCALL_FUTEX_WAIT] = (syscall_ptr_t) k_futex_wait,
    [SYSCALL_FUTEX_WAKE] = (syscall_ptr_t) k_futex_wake,
    [SYSCALL_EP_CREATE] = (syscall_ptr_t) k_ep_create,
    [SYSCALL_IPC_SEND] = (syscall_ptr_t) k_ipc_send,
    [SYSCALL_IPC_RECV] = (syscall_ptr_t) k_ipc_recv,
    [SYSCALL_IPC_CALL] = (syscall_ptr_t) k_ipc_call,
    [SYSCALL_IPC_REPLY] = (syscall_ptr_t) k_ipc_reply,
    [SYSCALL_MEM_ALLOC] = (syscall_ptr_t) k_mem_alloc,
    [SYSCALL_MEM_MAP] = (syscall_ptr_t) k_mem_map,
    [SYSCALL_IRQ_BIND] = (syscall_ptr_t) k_irq_bind,
    [SYSCALL_IRQ_ACK] = (syscall_ptr_t) k_irq_ack,
    [SYSCALL_HANDLE_CLOSE] = (syscall_ptr_t) k_handle_close,
    [SYSCALL_IOPORT_ACCESS] = (syscall_ptr_t) k_ioport_access,
    [SYSCALL_BOOTINFO] = (syscall_ptr_t) k_bootinfo,
    [SYSCALL_IPC_RECV_NB] = (syscall_ptr_t) k_ipc_recv_nb,
    [SYSCALL_IPC_RECV_TIMEOUT] = (syscall_ptr_t) k_ipc_recv_timeout,
    [SYSCALL_MEM_UNMAP] = (syscall_ptr_t) k_mem_unmap,
    [SYSCALL_HANDLE_DUP] = (syscall_ptr_t) k_handle_dup,
    [SYSCALL_SERIAL_WRITE] = (syscall_ptr_t) k_serial_write,
    [SYSCALL_PROC_SPAWN] = (syscall_ptr_t) k_proc_spawn,
    [SYSCALL_PROC_MAP] = (syscall_ptr_t) k_proc_map,
    [SYSCALL_PROC_SET_ENTRY] = (syscall_ptr_t) k_proc_set_entry,
    [SYSCALL_PROC_START] = (syscall_ptr_t) k_proc_start,
    [SYSCALL_SOCKET_CLOSE] = (syscall_ptr_t) k_socket_close,
    [SYSCALL_MEM_PHYS] = (syscall_ptr_t) k_mem_phys
};

void syscall_init(void)
{
    write_msr(MSR_EFER, read_msr(MSR_EFER) | 1);        /* Enable syscall */

    uint64_t star = (uint64_t) DEFAULT_KMODE_CODE << 32;
    star |= (uint64_t) (DEFAULT_KMODE_DATA | 3) << 48;

    write_msr(MSR_STAR, star);

    write_msr(MSR_LSTAR, (uint64_t) & syscall_handler);
    write_msr(MSR_SFMASK, X86_EFLAGS_TF | X86_EFLAGS_DF | X86_EFLAGS_IF
              | X86_EFLAGS_IOPL | X86_EFLAGS_AC | X86_EFLAGS_NT);

    klogi("SYSCALL: MSR_EFER=0x%016lx MSR_STAR=0x%016lx MSR_LSTAR=0x%016lx\n",
          read_msr(MSR_EFER), read_msr(MSR_STAR), read_msr(MSR_LSTAR));
}
