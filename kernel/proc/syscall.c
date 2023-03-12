/* Ref: Page 2994 in Intel® 64 and IA-32 Architectures Software Developer’s
 * Manual Combined Volumes: 1, 2A, 2B, 2C, 2D, 3A, 3B, 3C, 3D, and 4
 */
#include <sys/cpu.h>
#include <sys/idt.h>
#include <sys/apic.h>
#include <sys/panic.h>
#include <sys/isr_base.h>
#include <lib/klog.h>
#include <lib/memutils.h>
#include <lib/string.h>
#include <lib/errno.h>
#include <proc/task.h>
#include <proc/sched.h>
#include <proc/syscall.h>
#include <proc/eventbus.h>
#include <fs/filebase.h>
#include <fs/vfs.h>
#include <fs/ttyfs.h>
#include <device/keyboard/keyboard.h>
#include <device/display/term.h>

extern int64_t syscall_handler();

typedef int64_t (*syscall_ptr_t)(void);

int64_t k_not_implemented()
{
    kpanic("SYSCALL: unimplemented\n");
    return -1;
}

int64_t k_debug_log(char *message)
{
    char *s = strchr(message, '\n');

    if (s != NULL && (*(s + 1) == '\0')) {
        klogd("%s", message);
    } else {
        klogd("%s\n", message);
    }

    return strlen(message);
}

uint64_t k_vm_map(uint64_t *hint, uint64_t length, uint64_t prot,
                  uint64_t flags, uint64_t fd, uint64_t offset)
{
    /* TODO: review the parameters and implement the missing parts */
    (void)prot;
    (void)flags;
    (void)fd;
    (void)offset;

    task_t *t = sched_get_current_task();
    addrspace_t *as = NULL;

    if (t != NULL) {
        if (t->tid < 1) kpanic("SYSCALL: %s meets corrupted tid\n", __func__);
        as = t->addrspace;
    }

    size_t pf = VMM_FLAG_PRESENT | VMM_FLAG_USER | VMM_FLAG_READWRITE;
    uint64_t ptr;
    if (flags & MAP_FIXED) {
        ptr = (uint64_t)hint;

        uint64_t phys_ptr = pmm_get(NUM_PAGES(length) + 1, 0x0);
        /* TODO: How to handle the first information page???  */
        if (as != NULL) {
            vmm_map(as, ptr, phys_ptr + PAGE_SIZE, NUM_PAGES(length), pf, false);
        }

        memory_metadata_t *alloc = (memory_metadata_t*)PHYS_TO_VIRT(phys_ptr);
        alloc->numpages = NUM_PAGES(length);
        alloc->size = length;

        return ptr;
    } else {
        ptr = pmm_get(NUM_PAGES(length) + 1, 0x0);
        vmm_map(NULL, ptr, ptr, NUM_PAGES(length) + 1, pf, false);
        if (as != NULL) {
            vmm_map(as, ptr, ptr, NUM_PAGES(length) + 1, pf, false);
        }

        memory_metadata_t *alloc = (memory_metadata_t*)ptr;
        alloc->numpages = NUM_PAGES(length);
        alloc->size = length;

        return (uint64_t)alloc + PAGE_SIZE;
    }
}

static void get_full_path(int64_t dirfd, const char *path, char *full_path)
{
    if ((int32_t)dirfd == (int32_t)VFS_FDCWD) {
        /* Get the parent path name from TCB (task control block) */
        task_t *t = sched_get_current_task();
        if (t != NULL) strcat(full_path, t->cwd);
    } else if ((int32_t)dirfd >= (int32_t)0) {
        /* Get the parent path name from dirfd */
        vfs_node_desc_t *tnode = vfs_handle_to_fd((vfs_handle_t)dirfd);
        if (tnode != NULL) {
            strcpy(full_path, tnode->path);
        }
    }

    if (!strcmp(path, ".")) {
        return;
    }

    if (path[0] != '/') {
        /* Make sure the parent path name ends with '/' */
        size_t fpl = strlen(full_path);
        if (fpl > 0) {
            if (full_path[fpl - 1] != '/') strcat(full_path, "/");
        } else {
            strcpy(full_path, "/");
        }
    } else {
        /* Make sure the parent path name does not end with '/' */
        size_t fpl = strlen(full_path);
        if (fpl > 0) {
            full_path[fpl - 1] = '\0';
        }
    }

    strcat(full_path, path);
}

int64_t k_openat(int64_t dirfd, char *path, int64_t flags, int64_t mode)
{
    /* "mode" is always zero */
    (void)mode;
    cpu_set_errno(0);

    char full_path[VFS_MAX_PATH_LEN] = {0};
    get_full_path(dirfd, path, full_path);

    vfs_openmode_t openmode = VFS_MODE_READWRITE;
    switch(flags & 0x7) {
    case O_RDONLY:
        openmode = VFS_MODE_READ;
        break;
    case O_WRONLY:
        openmode = VFS_MODE_WRITE;
        break;
    case O_RDWR:
    default:
        openmode = VFS_MODE_READWRITE;
        break;
    }

    return vfs_open(full_path, openmode);
}

int64_t k_seek(int64_t fd, int64_t offset, int64_t whence)
{
    if (fd == STDIN || fd == STDOUT || fd == STDERR) {
        return 0;
    }

    return vfs_seek(fd, offset, whence);
}

int64_t k_close(int64_t fd)
{
    if (fd == STDIN || fd == STDOUT || fd == STDERR) {
        return 0;
    }

    return vfs_close(fd);
}

int64_t k_read(int64_t fd, void* buf, size_t count)
{
    cpu_set_errno(0);

    if (fd == STDIN) {
        if (ttyfh != VFS_INVALID_HANDLE) {
            return vfs_read(ttyfh, count, buf);
        }
        return -1;
    } else if (fd >= VFS_MIN_HANDLE) {
        int64_t len = vfs_read(fd, count, buf);
        return len;
    } else {
        cpu_set_errno(-EBADF);
        return -1;
    }
}

int64_t k_write(int64_t fd, const void* buf, size_t count)
{
    if (fd == STDOUT || fd == STDERR) {
        if (ttyfh != VFS_INVALID_HANDLE) {
            return vfs_write(ttyfh, count, buf);
        } else {
            return 0;
        }
    }

    if (fd < 3) {
        klogd("k_write: invalid file descriptor fd=%d\n", fd);
        return -EPERM;
    }

    return -EBADF;
}

void k_set_fs_base(uint64_t val)
{
    write_msr(MSR_FS_BASE, val);
}

int64_t k_ioctl(int64_t fd, int64_t request, int64_t arg)
{
    cpu_set_errno(0);

    if (fd == STDIN || fd == STDOUT || fd == STDERR) {
        if (ttyfh != VFS_INVALID_HANDLE) {
            return vfs_ioctl(ttyfh, request, arg);
        }   
    }

    /* This can return error code for bash's error message: cannot set
     * terminal process group
     * TODO: Need to consider how to support this?
     */
    cpu_set_errno(EINVAL);
    return -1;
}

int64_t k_fstatat(
    int32_t dirfd, const char *path, int64_t statbuf, int64_t flags)
{
    (void)flags;

    char full_path[VFS_MAX_PATH_LEN] = {0};
    get_full_path(dirfd, path, full_path);

    vfs_tnode_t *node = vfs_path_to_node(full_path, NO_CREATE, 0);

    if (node != NULL) {
        vfs_stat_t *st = (vfs_stat_t*)statbuf;
        *st = node->st;
        cpu_set_errno(0); 
        return 0;
    } else {
        cpu_set_errno(EINVAL);
        return -1;
    }
}

int64_t k_fstat(int64_t handle, int64_t statbuf)
{
    vfs_node_desc_t *fd = vfs_handle_to_fd(handle);
    cpu_set_errno(0);

    if (fd != NULL) {
        vfs_stat_t *st = (vfs_stat_t*)statbuf;
        *st = fd->tnode->st;
        return 0;
    } else {
        cpu_set_errno(EINVAL);
        return -1;
    }
}

int64_t k_getpid()
{
    task_t *t = sched_get_current_task();
    cpu_set_errno(0);

    if (t != NULL) {
        if (t->tid >= 1) return t->tid;
    }

    cpu_set_errno(EINVAL);
    return -1;
}

int64_t k_chdir(char *dir)
{
    task_t *t = sched_get_current_task();
    cpu_set_errno(0);

    if (t != NULL) {
        if (t->tid >= 1 && dir != NULL) {
            klogd("SYSCALL: chdir %s\n", dir);
            strcpy(t->cwd, dir);
            return 0;
        }
    }   

    cpu_set_errno(EINVAL);
    return -1; 
}

int64_t k_getppid()
{
    cpu_set_errno(EINVAL);
    return -1; 
}

int64_t k_fcntl(int64_t fd, int64_t request, int64_t arg)
{
    klogd("k_fcntl: fd 0x%x, request 0x%x, arg 0x%x\n", fd, request, arg);
    cpu_set_errno(-1);
    return -1; 
}

void k_exit(int64_t status)
{
    klogd("k_exit: exit with status %d\n", status);
    sched_exit(status);
}

syscall_ptr_t syscall_funcs[] = {
    [SYSCALL_DEBUGLOG]      = (syscall_ptr_t)k_debug_log,
    [SYSCALL_MMAP]          = (syscall_ptr_t)k_vm_map,
    [SYSCALL_OPENAT]        = (syscall_ptr_t)k_openat,
    [SYSCALL_READ]          = (syscall_ptr_t)k_read,
    [SYSCALL_WRITE]         = (syscall_ptr_t)k_write,
    [SYSCALL_SEEK]          = (syscall_ptr_t)k_seek,
    [SYSCALL_CLOSE]         = (syscall_ptr_t)k_close,
    [SYSCALL_SET_FS_BASE]   = (syscall_ptr_t)k_set_fs_base,
    [SYSCALL_IOCTL]         = (syscall_ptr_t)k_ioctl,           /* 8 */
    [SYSCALL_GETPID]        = (syscall_ptr_t)k_getpid,
    [SYSCALL_CHDIR]         = (syscall_ptr_t)k_chdir,
    (syscall_ptr_t)k_not_implemented,
    (syscall_ptr_t)k_not_implemented,
    (syscall_ptr_t)k_not_implemented,
    (syscall_ptr_t)k_not_implemented,
    (syscall_ptr_t)k_not_implemented,
    (syscall_ptr_t)k_not_implemented,                           /* 16 */
    [SYSCALL_FSTATAT]       = (syscall_ptr_t)k_fstatat,
    [SYSCALL_FSTAT]         = (syscall_ptr_t)k_fstat,
    [SYSCALL_GETPPID]       = (syscall_ptr_t)k_getppid,
    [SYSCALL_FCNTL]         = (syscall_ptr_t)k_fcntl,
    (syscall_ptr_t)k_not_implemented,
    (syscall_ptr_t)k_not_implemented,
    [SYSCALL_EXIT]          = (syscall_ptr_t)k_exit,
    (syscall_ptr_t)k_not_implemented,
    (syscall_ptr_t)k_not_implemented,
    (syscall_ptr_t)k_not_implemented,
    (syscall_ptr_t)k_not_implemented,
    (syscall_ptr_t)k_not_implemented,
    (syscall_ptr_t)k_not_implemented,
    (syscall_ptr_t)k_not_implemented,
    (syscall_ptr_t)k_not_implemented,
    (syscall_ptr_t)k_not_implemented,
    (syscall_ptr_t)k_not_implemented,
    (syscall_ptr_t)k_not_implemented,
    (syscall_ptr_t)k_not_implemented
};

void syscall_init(void)
{
    write_msr(MSR_EFER, read_msr(MSR_EFER) | 1); /* Enable syscall */

    uint64_t star = (uint64_t)DEFAULT_KMODE_CODE << 32;
    star |= (uint64_t)(DEFAULT_KMODE_DATA | 3) << 48;

    write_msr(MSR_STAR, star);

    write_msr(MSR_LSTAR, (uint64_t)&syscall_handler);
    write_msr(MSR_SFMASK, X86_EFLAGS_TF | X86_EFLAGS_DF | X86_EFLAGS_IF
        | X86_EFLAGS_IOPL | X86_EFLAGS_AC | X86_EFLAGS_NT);

    klogi("SYSCALL: MSR_EFER=0x%016x MSR_STAR=0x%016x MSR_LSTAR=0x%016x\n",
          read_msr(MSR_EFER),
          read_msr(MSR_STAR),
          read_msr(MSR_LSTAR));
}
