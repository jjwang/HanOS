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
#include <libc/string.h>
#include <libc/errno.h>
#include <libc/numeric.h>

#include <sys/cpu.h>
#include <sys/idt.h>
#include <sys/apic.h>
#include <sys/panic.h>
#include <sys/pci.h>
#include <sys/isr_base.h>
#include <base/klog.h>
#include <base/vector.h>
#include <base/kmalloc.h>
#include <mm/uaccess.h>
#include <ipc/ipc.h>
#include <ipc/irq.h>
#include <proc/process.h>
#include <proc/sched.h>
#include <proc/wait.h>
#include <proc/syscall.h>
#include <proc/signal.h>
#include <fs/filebase.h>
#include <fs/vfs.h>
#include <fs/ttyfs.h>
#include <device/keyboard/keyboard.h>
#include <device/display/term.h>
#include <device/display/gfx.h>

#define MMAP_ANON_BASE      0x80000000000

extern int64_t syscall_handler();

typedef int64_t(*syscall_ptr_t) (void);

extern spinlock_t vfs_lock;

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
    for (int i = 0; kargv != NULL && kargv[i] != NULL; i++)
        kmfree(kargv[i]);
}

/* Copy a NULL-terminated array of user strings into kernel memory. The caller
 * must provide room for EXEC_MAX_ARGS + 1 entries. Returns 0 on success. */
static int copy_exec_argv(const char *uarr[], char **karr)
{
    for (int i = 0; i <= EXEC_MAX_ARGS; i++)
        karr[i] = NULL;

    if (uarr == NULL)
        return 0;

    for (int i = 0; i < EXEC_MAX_ARGS; i++) {
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

    /* TODO: How to handle the first information page???  */

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
             vec_length(&t->mmap_list), as, as->PML4, phys_ptr, ptr, np,
             prot, flags);
    }

    mem_map_t m = { 0 };

    m.vaddr = ptr;
    m.paddr = phys_ptr;
    m.np = NUM_PAGES(length);
    m.flags = pf;

    vec_push_back(&t->mmap_list, m);

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

int64_t k_openat(int64_t dirfh, char *path, int64_t flags, int64_t mode)
{
    /* "mode" is always zero */
    (void) mode;
    cpu_set_errno(0);

    /* Copy the user path into the kernel before touching it. */
    char kpath[VFS_MAX_PATH_LEN] = { 0 };
    if (path == NULL || strncpy_from_user(kpath, path, sizeof(kpath)) < 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    path = kpath;

    char full_path[VFS_MAX_PATH_LEN] = { 0 };
    if (vfs_get_full_path(dirfh, path, full_path, sizeof(full_path)) < 0) {
        klogv("k_openat: cannot get full path for \"%s\"\n", path);
        cpu_set_errno(EINVAL);
        return -1;
    } else {
        /* Check whether folder exists or not, e.g. filename is "1/txt" */
        uint64_t len = strlen(full_path);
        if (len == 0) {
            klogv("k_openat: full path of \"%s\" is null\n", path);
            cpu_set_errno(EINVAL);
            return -1;
        }
        for (int64_t i = len - 1; i >= 0; i--) {
            if (full_path[i] == '/') {
                full_path[i] = '\0';
                break;
            }
        }
        if (strlen(full_path) > 0) {
            vfs_tnode_t *tnode = vfs_path_to_node(full_path, NO_CREATE, 0);
            if (tnode == NULL) {
                klogv("k_openat: directory \"%s\" doesn't exist\n",
                      full_path);
                cpu_set_errno(ENOENT);
                return -1;
            }
        }
        if (vfs_get_full_path(dirfh, path, full_path, sizeof(full_path)) < 0) {
            klogv("k_openat: full path of \"%s\" cannot be got\n", path);
            cpu_set_errno(EINVAL);
            return -1;
        }
        klogi("k_openat: continue opening \"%s\"\n", full_path);
    }

    vfs_openmode_t openmode = VFS_MODE_READWRITE;
    int32_t perms = 0;
    switch (flags & 0x7) {
    case O_EXEC:
        openmode = VFS_MODE_READ;
        perms = S_IRUSR | S_IXUSR;
        break;
    case O_RDONLY:
        openmode = VFS_MODE_READ;
        perms = S_IRUSR;
        break;
    case O_WRONLY:
        openmode = VFS_MODE_WRITE;
        perms = S_IWUSR;
        break;
    case O_RDWR:
    default:
        openmode = VFS_MODE_READWRITE;
        perms = S_IRUSR | S_IWUSR;
        break;
    }

    if (flags & O_CREAT) {
        int64_t ret = vfs_create(full_path, VFS_NODE_FILE);
        if (ret < 0) {
            klogv("k_openat: creating file for \"%s\" failed\n", path);
            cpu_set_errno(EEXIST);
            return ret;
        } else {
            vfs_handle_t fh = vfs_open(full_path, VFS_MODE_WRITE);
            if (fh != VFS_INVALID_HANDLE) {
                vfs_chmod(fh, perms | S_IRUSR);
                vfs_close(fh);
            }
        }
    }

    klogd("k_openat: dirfh 0x%016lx, path %s and flags 0x%016lx\n", dirfh, path,
          flags);
    return vfs_open(full_path, openmode);
}

int64_t k_chmod(char *path, int64_t flags)
{
    cpu_set_errno(0);

    char kpath[VFS_MAX_PATH_LEN] = { 0 };
    if (!copy_user_path(path, kpath, sizeof(kpath))) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    path = kpath;

    klogi("k_chmod: \"%s\" with flags 0x%016lx\n", path, flags);

    vfs_handle_t fh = k_openat(VFS_FDCWD, path, O_RDWR, 0);
    if (fh != VFS_INVALID_HANDLE) {
        int32_t perms = 0;
        switch (flags & 0x7) {
        case O_EXEC:
            perms = S_IRUSR | S_IXUSR;
            break;
        case O_RDONLY:
            perms = S_IRUSR;
            break;
        case O_WRONLY:
            perms = S_IWUSR;
            break;
        case O_RDWR:
        default:
            perms = S_IRUSR | S_IWUSR;
            break;
        }
        vfs_chmod(fh, perms | S_IRUSR);
        vfs_close(fh);
        return 0;
    }

    cpu_set_errno(ENOENT);
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

    char full_path[VFS_MAX_PATH_LEN] = { 0 };
    if (vfs_get_full_path(VFS_FDCWD, path, full_path, sizeof(full_path)) < 0) {
        cpu_set_errno(EINVAL);
        return -1;
    } else {
        /* Check whether folder exists or not, e.g. filename is "1/txt" */
        uint64_t len = strlen(full_path);
        if (len == 0) {
            cpu_set_errno(EINVAL);
            return -1;
        }
        for (int64_t i = len - 1; i >= 0; i--) {
            if (full_path[i] == '/') {
                full_path[i] = '\0';
                break;
            }
        }
        if (strlen(full_path) > 0) {
            vfs_tnode_t *tnode = vfs_path_to_node(full_path, NO_CREATE, 0);
            if (tnode == NULL) {
                klogd("k_openat: directory \"%s\" doesn't exist\n",
                      full_path);
                cpu_set_errno(ENOENT);
                return -1;
            }
        }
        if (vfs_get_full_path(VFS_FDCWD, path, full_path, sizeof(full_path)) <
            0) {
            cpu_set_errno(EINVAL);
            return -1;
        }
    }

    vfs_tnode_t *tnode = vfs_path_to_node(full_path, NO_CREATE, 0);
    if (tnode == NULL) {
        cpu_set_errno(ENOENT);
        return -1;
    }

    vfs_inode_t *pi = tnode->parent;
    for (uint64_t i = 0; i < vec_length(&(pi->child)); i++) {
        if (vec_at(&(pi->child), i) == tnode) {
            if (tnode->inode->refcount == 0) {
                vec_erase(&(pi->child), i);
                return 0;
            } else {
                klogw
                    ("k_unlink: failed because of refcount of \"%s\" is %ld\n",
                     path, tnode->inode->refcount);
                cpu_set_errno(EINVAL);
                return -1;
            }
        }
    }

    cpu_set_errno(ENOENT);
    return -1;
}

int64_t k_seek(int64_t fh, int64_t offset, int64_t whence)
{
    cpu_set_errno(0);

    if (fh == STDIN || fh == STDOUT || fh == STDERR) {
        klogv("k_seek: fh %ld(0x%016lx), offset %ld, whence %ld\n",
              fh, fh, offset, whence);
        return 0;
    }

    int64_t ret = vfs_seek(fh, offset, whence);

    klogd("k_seek: fh %ld(0x%016lx), offset %ld, whence %ld and return %ld\n",
          fh, fh, offset, whence, ret);
    if (ret < 0)
        cpu_set_errno(EINVAL);

    return ret;
}

int64_t k_close(int64_t fh)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    klogd("k_close: close file handle %ld\n", fh);

    if (t != NULL) {
        spinlock_acquire(&vfs_lock);
        /* Check whether there is file redirection */
        for (uint64_t i = 0; i < vec_length(&t->dup_list); i++) {
            file_dup_t dup = vec_at(&t->dup_list, i);
            if (dup.newfh == fh) {
                /* Close original file and delete from dup list */
                if (dup.fh != STDIN && dup.fh != STDOUT
                    && dup.fh != STDERR) {
                    klogd("k_close: close dup file handle %ld\n", dup.fh);
                    /* BUGFIX: we must release vfs_lock here before calling
                     * vfs_close() to avoid dead lock.
                     */
                    spinlock_release(&vfs_lock);
                    vfs_close(dup.fh);
                    spinlock_acquire(&vfs_lock);
                }
                vec_erase(&t->dup_list, i);
                break;
            }
            if (dup.fh == fh) {
                spinlock_release(&vfs_lock);
                /* Do not close if mapping to another file handle */
                klogd("k_close: do not close dup file handle %ld <- %ld\n",
                      fh, dup.newfh);
                cpu_set_errno(EINVAL);
                return -1;
            }
        }
        spinlock_release(&vfs_lock);
    }

    if (fh == STDIN || fh == STDOUT || fh == STDERR) {
        return 0;
    }

    return vfs_close(fh);
}

int64_t k_read(int64_t fh, void *buf, uint64_t count)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    if (buf != NULL && count > 0 && !user_range_ok(t, buf, count)) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    klogd("k_read: read %ld from file handle %ld\n", count, fh);

    if (fh == STDIN) {
        bool found = false;
        vfs_handle_t oldfh = -1;
        if (t != NULL) {
            spinlock_acquire(&vfs_lock);
            /* Check whether it is redirected from some file */
            for (uint64_t i; i < vec_length(&t->dup_list); i++) {
                file_dup_t dup = vec_at(&t->dup_list, i);
                if (dup.newfh == fh) {
                    oldfh = dup.fh;
                    found = true;
                    break;
                } else if (dup.fh == fh) {
                    oldfh = dup.newfh;
                    found = true;
                }
            }
            spinlock_release(&vfs_lock);
        }
        if (found) {
            int64_t ret = vfs_read(oldfh, count, buf);
            klogd("k_read: read from handle %ld instead of %ld"
                  " and return %ld bytes\n", oldfh, fh, ret);
            return ret;
        } else {
            vfs_handle_t ttyfh = vfs_open("/dev/tty", VFS_MODE_READWRITE);
            if (ttyfh != VFS_INVALID_HANDLE) {
                int64_t len = vfs_read(ttyfh, count, buf);
                vfs_close(ttyfh);
                return len;
            }
        }
        cpu_set_errno(EINVAL);
        return -1;
    } else if (fh >= VFS_MIN_HANDLE) {
        int64_t len = vfs_read(fh, count, buf);
        klogd
            ("k_read: try to read %ld bytes from file %ld and return %ld bytes\n",
             count, fh, len);
        return len;
    } else {
        cpu_set_errno(EBADF);
        return -1;
    }
}

static pid_t last_write_pid = 0;
static uint64_t last_write_nanos = 0;

int64_t k_write(int64_t fh, const void *buf, uint64_t count)
{
    process_t *t = sched_get_current_process();

    cpu_set_errno(0);

    if (buf != NULL && count > 0 && !user_range_ok(t, buf, count)) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    if (fh == STDOUT || fh == STDERR) {
        bool found = false;
        vfs_handle_t oldfh = -1;
        if (t != NULL) {
            spinlock_acquire(&vfs_lock);
            /* Check whether it is redirected from some file */
            for (uint64_t i; i < vec_length(&t->dup_list); i++) {
                file_dup_t dup = vec_at(&t->dup_list, i);
                if (dup.newfh == fh) {
                    oldfh = dup.fh;
                    found = true;
                    break;
                } else if (dup.fh == fh) {
                    oldfh = dup.newfh;
                    found = true;
                    break;
                }
            }
            spinlock_release(&vfs_lock);
        }
        if (found) {
            klogd("k_write: write %ld bytes to oldfh %ld <- fh %ld\n",
                  count, oldfh, fh);
            int64_t ret = vfs_write(oldfh, count, buf);
            return ret;
        } else {
            if (debug_info) {
                for (uint64_t i = 0; i < count; i++) {
                    char c = ((char *) buf)[i];
                    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
                        || (c >= 'A' && c <= 'Z') || c == '[') {
                        klogd("k_write: write [%c]\n", c);
                    } else {
                        klogd("k_write: write [0x%02x]\n", c);
                    }
                }
            }

            if (last_write_pid != t->pid && last_write_nanos != 0) {
                while (hpet_get_nanos() - last_write_nanos
                       <= MILLIS_TO_NANOS(250)) {
                    sched_sleep(100);
                }
            }

            spinlock_acquire(&vfs_lock);
            last_write_pid = t->pid;
            last_write_nanos = hpet_get_nanos();
            spinlock_release(&vfs_lock);

            vfs_handle_t ttyfh = vfs_open("/dev/tty", VFS_MODE_READWRITE);
            if (ttyfh != VFS_INVALID_HANDLE) {
                int64_t len = vfs_write(ttyfh, count, buf);
                vfs_close(ttyfh);
                return len;
            }
            return 0;
        }
    }

    if (fh < 3) {
        kloge("k_write: invalid file handler fh=%ld\n", fh);
        cpu_set_errno(EPERM);
        return -1;
    }

    return vfs_write(fh, count, buf);
}

void k_set_fs_base(uint64_t val)
{
    process_t *t = sched_get_current_process();
    klogd("k_set_fs_base: process #%ld set to 0x%016lx\n",
          t == NULL ? 0 : t->pid, val);
    write_msr(MSR_FS_BASE, val);
    if (t != NULL)
        t->fs_base = val;
}

int64_t k_ioctl(int64_t fd, int64_t request, int64_t arg)
{
    cpu_set_errno(0);

    if (fd == STDIN || fd == STDOUT || fd == STDERR) {
        vfs_handle_t ttyfh = vfs_open("/dev/tty", VFS_MODE_READWRITE);
        if (ttyfh != VFS_INVALID_HANDLE) {
            int64_t ret = vfs_ioctl(ttyfh, request, arg);
            vfs_close(ttyfh);
            return ret;
        }
    }

    /* This can return error code for bash's error message: cannot set
     * terminal process group
     * TODO: Need to consider how to support this?
     */
    cpu_set_errno(EINVAL);
    return -1;
}

int64_t k_fstatat(int64_t dirfh, const char *path, int64_t statbuf,
                  int64_t flags)
{
    (void) flags;

    char kpath[VFS_MAX_PATH_LEN] = { 0 };
    if (!copy_user_path(path, kpath, sizeof(kpath))) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    path = kpath;

    char full_path[VFS_MAX_PATH_LEN] = { 0 };
    if (vfs_get_full_path(dirfh, path, full_path, sizeof(full_path)) < 0) {
        return -1;
    }

    vfs_tnode_t *node = vfs_path_to_node(full_path, NO_CREATE, 0);

    if (node != NULL && node->st.st_nlink > 0) {
        if (copy_to_user((void *) statbuf, &(node->st),
                         sizeof(vfs_stat_t)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
        klogd
            ("k_fstatat: success with dirfh 0x%016lx and path %s(%s), size %ld\n",
             dirfh, full_path, path, node->st.st_size);
        cpu_set_errno(0);
        return 0;
    } else {
        klogd("k_fstatat: fail with dirfh 0x%016lx and path %s(%s)\n",
              dirfh, full_path, path);
        cpu_set_errno(ENOENT);
        return -1;
    }
}

int64_t k_fstat(int64_t handle, int64_t statbuf)
{
    if (handle == STDIN || handle == STDOUT || handle == STDERR) {
        /*
         * Set the file stat buffer to zero. If we do nothing here, maybe it
         * will cause crash in some apps, e.g., cat in coreutils.
         */
        if (clear_user((void *) statbuf, sizeof(vfs_stat_t)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
        klogd("k_fstat: success with file handle %ld\n", handle);
        return 0;
    }

    vfs_node_desc_t *fd = vfs_handle_to_fd(handle, __func__);
    cpu_set_errno(0);

    if (fd != NULL) {
        if (copy_to_user((void *) statbuf, &(fd->tnode->st),
                         sizeof(vfs_stat_t)) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
        klogd("k_fstat: success with file handle %ld and size %ld\n",
              handle, fd->tnode->st.st_size);
        return 0;
    } else {
        kloge("k_fstat: fail with file handle %ld\n", handle);
        cpu_set_errno(EINVAL);
        return -1;
    }
}

/* TODO: Currently skip the parameter - flags */
int64_t k_faccessat(int64_t dirfh, const char *path, uint64_t mode,
                    uint64_t flags)
{
    (void) flags;

    cpu_set_errno(0);

    char kpath[VFS_MAX_PATH_LEN] = { 0 };
    if (!copy_user_path(path, kpath, sizeof(kpath))) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    path = kpath;

    char full_path[VFS_MAX_PATH_LEN] = { 0 };
    if (vfs_get_full_path(dirfh, path, full_path, sizeof(full_path)) < 0) {
        cpu_set_errno(EBADF);
        return -1;
    }

    klogi("k_faccessat: open \"%s\" at mode 0x%016lx and flags 0x%016lx\n",
          full_path, mode, flags);

    vfs_tnode_t *node = vfs_path_to_node(full_path, NO_CREATE, 0);

    if (node != NULL) {
        uint32_t perms = node->inode->perms;
        if ((mode & R_OK) && !(perms & S_IRUSR)) {
            cpu_set_errno(EACCES);
            return -1;
        }
        if ((mode & W_OK) && !(perms & S_IWUSR)) {
            cpu_set_errno(EACCES);
            return -1;
        }
        if ((mode & X_OK) && !(perms & S_IXUSR)) {
            cpu_set_errno(EACCES);
            return -1;
        }
        if (mode & F_OK) {
            return 0;
        }
        return 0;
    } else {
        cpu_set_errno(EBADF);
        return -1;
    }
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

    /* TODO: Need to add bound check */
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

    if (vfs_path_to_node(fullpath, NO_CREATE, 0) == NULL) {
        cpu_set_errno(ENOENT);
        goto err_exit;
    }

    strcpy(t->cwd, fullpath);
    return 0;
  err_exit:
    return -1;
}

int64_t k_readdir(int64_t handle, uint64_t buff)
{
    vfs_node_desc_t *fd = vfs_handle_to_fd(handle, __func__);
    int64_t errno = 0;

    cpu_set_errno(errno);

    if (fd == NULL) {
        errno = EINVAL;
        goto err_exit;
    }

    if (!(fd->inode->type == VFS_NODE_FOLDER
          || fd->inode->type == VFS_NODE_MOUNTPOINT)) {
        errno = ENOTDIR;
        goto err_exit;
    }

    if (fd->curr_dir_ent == NULL) {
        if (vec_length(&fd->inode->child) == 0) {
            /* End of dir */
            goto err_exit;
        }
        fd->curr_dir_ent = vec_at(&fd->inode->child, 0);
        fd->curr_dir_idx = 0;
    } else {
        if (fd->curr_dir_idx >= vec_length(&fd->inode->child) - 1) {
            /* End of dir */
            fd->curr_dir_ent = NULL;
            goto err_exit;
        }
        fd->curr_dir_ent = vec_at(&fd->inode->child, fd->curr_dir_idx + 1);
        fd->curr_dir_idx++;
    }

    dirent_t kde;
    memset(&kde, 0, sizeof(kde));
    strncpy(kde.d_name, fd->curr_dir_ent->name, sizeof(kde.d_name) - 1);
    kde.d_ino = fd->curr_dir_ent->st.st_ino;
    kde.d_off = 0;
    kde.d_reclen = sizeof(dirent_t);
    kde.d_type = DT_UNKNOWN;

    if (copy_to_user((void *) buff, &kde, sizeof(kde)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    return 0;
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

int64_t k_pipe(int32_t * fh, uint32_t flags)
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

    char path[VFS_MAX_PATH_LEN] = { 0 };
    strcpy(path, "/dev/pipe/");

    uint64_t len = strlen(path);
    itoa(rand(sched_get_ticks() % 1000, 1, 1000),
         &path[len], VFS_MAX_PATH_LEN - len - 1, 10);

    vfs_create(path, VFS_NODE_CHAR_DEVICE);

    /* fh[0] is the reading port, fh[1] is the writing port */
    int32_t kfh[2];
    kfh[0] = vfs_open(path, VFS_MODE_READ);
    kfh[1] = vfs_open(path, VFS_MODE_WRITE);

    if (fh == NULL || copy_to_user(fh, kfh, sizeof(kfh)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    klogi("k_pipe: return reading port %ld and writing port %ld\n", kfh[0],
          kfh[1]);

    return 0;

  err_exit:
    return -1;
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
        /*
         * This should be parent process and returns child process id, but
         * currently it returns parent process id
         */
        klogd("k_fork: return %ld from parent process #%ld\n", tid_child,
              t->pid);
        return tid_child;
    } else {
        /* This should be child process and returns 0 */
        klogd("k_fork: return 0 from child process #%ld\n", tid_child);
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
    klogd("k_fcntl: fd 0x%016lx, request 0x%016lx, arg 0x%016lx\n", fd, request, arg);
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
    bool wait_any = (wpid == -1 || wpid == 0);
    bool nohang = (flags & WNOHANG) != 0;

    while (true) {
        pid_t *children = NULL;
        uint64_t len;

        spinlock_acquire(&parent->child_lock);
        len = vec_length(&(parent->child_list));
        if (len > 0) {
            children = kmalloc(len * sizeof(pid_t));
            if (children != NULL) {
                for (uint64_t i = 0; i < len; i++)
                    children[i] = vec_at(&(parent->child_list), i);
            }
        }
        spinlock_release(&parent->child_lock);

        if (len > 0 && children == NULL) {
            if (nohang) {
                cpu_set_errno(ENOMEM);
                return -1;
            }
            sched_wait_child(10);
            continue;
        }

        bool have_child = false;
        bool reaped = false;
        pid_t reaped_pid = PID_NONE;
        int64_t exit_status = 0;

        for (uint64_t i = 0; i < len; i++) {
            if (!wait_any && (int64_t) children[i] != (int64_t) wpid)
                continue;

            have_child = true;

            int64_t st = 0;
            int rc = sched_reap(children[i], &st);
            if (rc == 1) {
                reaped = true;
                reaped_pid = children[i];
                exit_status = st;
                break;
            }
            if (rc == -1) {
                /* The child was already reaped by an idle core. */
                reaped = true;
                reaped_pid = children[i];
                break;
            }
        }

        if (children != NULL)
            kmfree(children);

        if (reaped) {
            spinlock_acquire(&parent->child_lock);
            for (uint64_t i = 0; i < vec_length(&(parent->child_list)); i++) {
                if (vec_at(&(parent->child_list), i) == reaped_pid) {
                    vec_erase(&(parent->child_list), i);
                    break;
                }
            }
            spinlock_release(&parent->child_lock);

            if (status != NULL) {
                int32_t st32 = (int32_t) exit_status;
                if (copy_to_user(status, &st32, sizeof(st32)) != 0) {
                    cpu_set_errno(EFAULT);
                    return -1;
                }
            }
            cpu_set_errno(0);
            return reaped_pid;
        }

        if (!have_child) {
            cpu_set_errno(ECHILD);
            return -1;
        }

        if (nohang) {
            cpu_set_errno(0);
            return 0;
        }

        sched_wait_child(10);
    }
}

void k_exit(int64_t status)
{
    process_t *t = sched_get_current_process();
    if (t != NULL) {
        klogi("k_exit: process %ld exit with status %ld\n", t->pid, status);
    } else {
        goto normal_exit;
    }

    /* Close all open files */
    for (uint64_t i = 0; i < t->open_files_table.size; i++) {
        int64_t fh = t->open_files_table.array[i].key;
        if (fh >= 0) {
            vfs_close(fh);
        }
    }

  normal_exit:
    /* Exit from scheduler */
    sched_exit(status);
}

int k_getcwd(char *buffer, uint64_t size)
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

int k_getrusage(int64_t who, uint64_t usage)
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

    if (sched_execve(kpath, kargv_p, kenvp_p, cwd) != NULL) {
        klogi("k_execve: run \"%s\" and exit from process %ld\n", kpath,
              t != NULL ? t->pid : 0);
        free_exec_argv(kargv);
        free_exec_argv(kenvp);
        sched_exit(0);
        cpu_set_errno(0);
        return 0;
    }

    free_exec_argv(kargv);
    free_exec_argv(kenvp);
    cpu_set_errno(EINVAL);
    return -1;
}

int k_getclock(void *_, int64_t which, vfs_timespec_t * out)
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

int64_t k_readlink(int64_t dirfh, const char *path, void *buffer,
                   uint64_t max_size)
{
    cpu_set_errno(0);

    char kpath[VFS_MAX_PATH_LEN] = { 0 };
    if (!copy_user_path(path, kpath, sizeof(kpath))) {
        cpu_set_errno(EFAULT);
        return -1;
    }
    path = kpath;

    char full_path[VFS_MAX_PATH_LEN] = { 0 };
    vfs_get_full_path(dirfh, path, full_path, sizeof(full_path));

    vfs_tnode_t *tnode = vfs_path_to_node(full_path, NO_CREATE, 0);

    if (tnode == NULL)
        goto err_exit;
    if (tnode->inode->type != VFS_NODE_SYMLINK)
        goto err_exit;

    uint64_t link_len = strlen(tnode->inode->link);
    if (link_len < max_size) {
        klogd("k_readlink: %s -> %s\n", full_path, tnode->inode->link);
        if (copy_to_user(buffer, tnode->inode->link, link_len + 1) != 0) {
            cpu_set_errno(EFAULT);
            return -1;
        }
    } else {
        goto err_exit;
    }
    return (int64_t) link_len;

  err_exit:
    cpu_set_errno(EINVAL);
    return -1;
}

void k_uname(void)
{
}

int64_t k_dup3(int64_t fh, int64_t newfh, int64_t flags)
{
    process_t *t = sched_get_current_process();
    cpu_set_errno(0);

    if (t == NULL) {
        cpu_set_errno(ENOSYS);
        return -1;
    }

    klogd("k_dup3: pid %ld fh %ld <- newfh %ld, flags 0x%016lx\n",
          t->pid, fh, newfh, flags);

    spinlock_acquire(&vfs_lock);
    file_dup_t dup = {.fh = fh,.newfh = newfh };
    vec_push_back(&t->dup_list, dup);
    spinlock_release(&vfs_lock);

    return 0;
}

/* TODO: need to add futex implementation */
int64_t k_futex_wait(int64_t * ptr, vfs_timespec_t * tv, int64_t expected)
{
    int64_t val = 0;
    vfs_timespec_t ktv = { 0 };

    if (ptr == NULL || copy_from_user(&val, ptr, sizeof(val)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    if (tv != NULL
        && copy_from_user(&ktv, tv, sizeof(ktv)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    klogi("k_futex_wait: time spec (%ld, %ld) with ptr 0x%016lx, val %ld and "
          "expected %ld\n", ktv.tv_sec, ktv.tv_nsec, ptr, val, expected);

    return 0;
}

int64_t k_futex_wake(int64_t * ptr)
{
    int64_t val = 0;

    if (ptr == NULL || copy_from_user(&val, ptr, sizeof(val)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    klogi("k_futex_wake: ptr 0x%016lx and val %ld\n", ptr, val);

    return 0;
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
                              HANDLE_RIGHT_SEND | HANDLE_RIGHT_RECV);
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

int64_t k_ipc_send(int64_t handle, void *umsg)
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

int64_t k_ipc_recv(int64_t handle, void *umsg)
{
    endpoint_t *ep = k_ipc_resolve(handle, HANDLE_RIGHT_RECV);
    if (ep == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    ipc_msg_t m;
    if (ipc_recv(ep, &m) != 0) {
        cpu_set_errno(EAGAIN);
        return -1;
    }

    if (copy_to_user(umsg, &m, sizeof(m)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    cpu_set_errno(0);
    return 0;
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

    ipc_msg_t m;
    if (ipc_recv_timeout(ep, &m, 0) != 0) {
        cpu_set_errno(EAGAIN);
        return -1;
    }

    if (copy_to_user(umsg, &m, sizeof(m)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    cpu_set_errno(0);
    return 0;
}

int64_t k_ipc_recv_timeout(int64_t handle, void *umsg, int64_t timeout)
{
    endpoint_t *ep = k_ipc_resolve(handle, HANDLE_RIGHT_RECV);
    if (ep == NULL) {
        cpu_set_errno(EINVAL);
        return -1;
    }

    ipc_msg_t m;
    if (ipc_recv_timeout(ep, &m, (time_t) timeout) != 0) {
        cpu_set_errno(EAGAIN);
        return -1;
    }

    if (copy_to_user(umsg, &m, sizeof(m)) != 0) {
        cpu_set_errno(EFAULT);
        return -1;
    }

    cpu_set_errno(0);
    return 0;
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

syscall_ptr_t syscall_funcs[] = {
    [SYSCALL_DEBUGLOG] = (syscall_ptr_t) k_debug_log,
    [SYSCALL_MMAP] = (syscall_ptr_t) k_vm_map,
    [SYSCALL_OPENAT] = (syscall_ptr_t) k_openat,
    [SYSCALL_READ] = (syscall_ptr_t) k_read,
    [SYSCALL_WRITE] = (syscall_ptr_t) k_write,
    [SYSCALL_SEEK] = (syscall_ptr_t) k_seek,
    [SYSCALL_CLOSE] = (syscall_ptr_t) k_close,
    [SYSCALL_SET_FS_BASE] = (syscall_ptr_t) k_set_fs_base,
    [SYSCALL_IOCTL] = (syscall_ptr_t) k_ioctl,  /* 8 */
    [SYSCALL_GETPID] = (syscall_ptr_t) k_getpid,
    [SYSCALL_CHDIR] = (syscall_ptr_t) k_chdir,
    (syscall_ptr_t) k_not_implemented,
    (syscall_ptr_t) k_not_implemented,
    (syscall_ptr_t) k_not_implemented,
    [SYSCALL_FORK] = (syscall_ptr_t) k_fork,
    [SYSCALL_EXECVE] = (syscall_ptr_t) k_execve,
    [SYSCALL_FACCESSAT] = (syscall_ptr_t) k_faccessat,  /* 16 */
    [SYSCALL_FSTATAT] = (syscall_ptr_t) k_fstatat,
    [SYSCALL_FSTAT] = (syscall_ptr_t) k_fstat,
    [SYSCALL_GETPPID] = (syscall_ptr_t) k_getppid,
    [SYSCALL_FCNTL] = (syscall_ptr_t) k_fcntl,  /* 20 */
    [SYSCALL_DUP3] = (syscall_ptr_t) k_dup3,
    [SYSCALL_WAITPID] = (syscall_ptr_t) k_waitpid,
    [SYSCALL_EXIT] = (syscall_ptr_t) k_exit,
    [SYSCALL_READDIR] = (syscall_ptr_t) k_readdir,
    [SYSCALL_MUNMAP] = (syscall_ptr_t) k_vm_unmap,      /* 25 */
    [SYSCALL_GETCWD] = (syscall_ptr_t) k_getcwd,
    [SYSCALL_GETCLOCK] = (syscall_ptr_t) k_getclock,
    [SYSCALL_READLINK] = (syscall_ptr_t) k_readlink,
    [SYSCALL_GETRUSAGE] = (syscall_ptr_t) k_getrusage,  /* 29 */
    (syscall_ptr_t) k_not_implemented,
    [SYSCALL_UNAME] = (syscall_ptr_t) k_uname,
    [SYSCALL_FUTEX_WAIT] = (syscall_ptr_t) k_futex_wait,
    [SYSCALL_FUTEX_WAKE] = (syscall_ptr_t) k_futex_wake,
    [SYSCALL_MEMINFO] = (syscall_ptr_t) k_meminfo,      /* 34 */
    [SYSCALL_PIPE] = (syscall_ptr_t) k_pipe,
    [SYSCALL_UNLINK] = (syscall_ptr_t) k_unlink,        /* 36 */
    (syscall_ptr_t) k_not_implemented,
    (syscall_ptr_t) k_not_implemented,
    [SYSCALL_CHMOD] = (syscall_ptr_t) k_chmod,  /* 39 */
    [SYSCALL_RUNCMD] = (syscall_ptr_t) k_runcmd,
    [SYSCALL_GETENTROPY] = (syscall_ptr_t) k_getentropy,
    [SYSCALL_SIGPROCMASK] = (syscall_ptr_t) k_sigprocmask,      /* 42 */
    [SYSCALL_SIGACTION] = (syscall_ptr_t) k_sigaction,
    (syscall_ptr_t) k_not_implemented,
    (syscall_ptr_t) k_not_implemented,
    [SYSCALL_EP_CREATE] = (syscall_ptr_t) k_ep_create,  /* 50 */
    [SYSCALL_IPC_SEND] = (syscall_ptr_t) k_ipc_send,
    [SYSCALL_IPC_RECV] = (syscall_ptr_t) k_ipc_recv,
    [SYSCALL_IPC_CALL] = (syscall_ptr_t) k_ipc_call,
    [SYSCALL_IPC_REPLY] = (syscall_ptr_t) k_ipc_reply,
    [SYSCALL_IRQ_BIND] = (syscall_ptr_t) k_irq_bind,
    [SYSCALL_IRQ_ACK] = (syscall_ptr_t) k_irq_ack,
    [SYSCALL_HANDLE_CLOSE] = (syscall_ptr_t) k_handle_close,     /* 60 */
    [SYSCALL_IOPORT_ACCESS] = (syscall_ptr_t) k_ioport_access,
    [SYSCALL_BOOTINFO] = (syscall_ptr_t) k_bootinfo,              /* 63 */
    [SYSCALL_IPC_RECV_NB] = (syscall_ptr_t) k_ipc_recv_nb,
    [SYSCALL_IPC_RECV_TIMEOUT] = (syscall_ptr_t) k_ipc_recv_timeout
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
