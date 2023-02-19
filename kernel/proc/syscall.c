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
#include <proc/task.h>
#include <proc/sched.h>
#include <proc/syscall.h>
#include <proc/eventbus.h>
#include <fs/vfs.h>
#include <device/keyboard/keyboard.h>

extern int64_t syscall_handler();

typedef int64_t (*syscall_ptr_t)(void);

int64_t k_not_implemented()
{
    kpanic("Syscall: unimplemented\n");
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

uint64_t k_vm_map(uint64_t *hint, uint64_t length, uint64_t prot, uint64_t flags,
    uint64_t fd, uint64_t offset)
{
    /* TODO: review the parameters and implement the missing parts */
    (void)hint;
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
        vmm_map(NULL, ptr, phys_ptr, NUM_PAGES(length) + 1, pf, false);
        vmm_map(as, ptr, phys_ptr, NUM_PAGES(length) + 1, pf, false);
    } else {
        ptr = pmm_get(NUM_PAGES(length) + 1, 0x0);
        vmm_map(as, ptr, ptr, NUM_PAGES(length) + 1, pf, false);
    }

    memory_metadata_t *alloc = (memory_metadata_t*)ptr;

    alloc->numpages = NUM_PAGES(length);
    alloc->size = length;

    klogd("SYSCALL: malloc %d bytes with address 0x%x and PML4 0x%x\n",
          length, alloc, (as == NULL) ? NULL : as->PML4);

    return (uint64_t)alloc + PAGE_SIZE;
}

int64_t k_openat(int32_t dirfd, char *path, int64_t flags, int64_t mode)
{
    (void)dirfd;
    (void)flags;
    (void)mode;

    char full_path[VFS_MAX_PATH_LEN] = {0};
    if (dirfd == -100) {
        klogi("SYSCALL: open at current directory\n");
    }
    if (path[0] != '/') strcat(full_path, "/");
    strcat(full_path, path);

    klogd("SYSCALL: enter open at %s with dirfd 0x%x, flags 0x%x, mode 0x%x\n",
          full_path, dirfd, flags, mode);

    int64_t fd = vfs_open(full_path, VFS_MODE_READWRITE);

    if (fd == VFS_INVALID_HANDLE) {
        cpu_set_errno(-1);        
    } else {
        cpu_set_errno(0);
    }
    cpu_debug();

    return fd;
}

int64_t k_seek(int64_t fd, int64_t offset, int64_t whence)
{
    klogd("SYSCALL: seek offset %d bytes from %d mode %d\n",
          offset, fd, whence);

    return vfs_seek(fd, offset);
}
int64_t k_close(int64_t fd)
{
    return vfs_close(fd);
}

int64_t k_read(int64_t fd, void* buf, size_t count)
{
    if (fd == STDIN) {
        if (count <= 0) {
            return 0;
        }

        event_para_t para = 0;
        if (eb_subscribe(sched_get_tid(), EVENT_KEY_PRESSED, &para)) {
            uint8_t keycode = para & 0xFF;
            if (keycode) {
                ((uint8_t*)buf)[0] = keycode;
                return 1;
            }
        }

        return 0;
    }

    if (fd < 3) {
        klogd("k_read: invalid file descriptor fd=%d\n", fd);
        return -EPERM;
    }

    klogd("SYSCALL: read %d bytes from %d with file length %d\n",
          count, fd, vfs_tell(fd));

    if (fd >= VFS_MIN_HANDLE) {
        int64_t len = vfs_read(fd, count, buf);
        klogd("SYSCALL: read %d bytes from %d\n", len, fd);
        return len;
    }

    return -EBADF;
}

int64_t k_write(int64_t fd, const void* buf, size_t count)
{
    if (fd == STDOUT || fd == STDERR) {
        char *msg = (char*)kmalloc(count + 1);
        if (msg != NULL) {
            msg[count] = '\0';
            memcpy(msg, buf, count);

            cursor_visible = CURSOR_HIDE;
            term_set_cursor(' ');
            term_refresh(TERM_MODE_CLI, false);

            kprintf("%s", msg);
            cursor_visible = CURSOR_INVISIBLE;

            kmfree(msg);
            return count;
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
    klogd("k_ioctl: fd 0x%x, request 0x%x, arg 0x%x\n", fd, request, arg);
    cpu_set_errno(-1);
    return -1;
}

int64_t k_fstatat(
    int32_t dirfd, const char *path, int64_t statbuf, int64_t flags)
{
    klogd("k_fstatat: dirfd %d, path %s, statbuf 0x%x, flags 0x%x\n",
          dirfd, path, statbuf, flags);
    return -1;
}

int64_t k_fstat(int64_t fd, int64_t statbuf)
{
    klogd("k_fstat: fd 0x%x, statbuf 0x%x\n", fd, statbuf);
    return -1;
}

int64_t k_getpid()
{
    task_t *t = sched_get_current_task();

    if (t != NULL) {
        if (t->tid >= 1) return t->tid;
    }

    return -1;
}

int64_t k_getppid()
{
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
    (syscall_ptr_t)k_not_implemented,
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
