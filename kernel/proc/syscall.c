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

    size_t pf = VMM_FLAG_PRESENT | VMM_FLAGS_USERMODE | VMM_FLAG_READWRITE;
    uint64_t ptr;
    if (flags & MAP_FIXED) {
        ptr = (uint64_t)hint;

        uint64_t phys_ptr = VIRT_TO_PHYS(kmalloc(NUM_PAGES(length) * PAGE_SIZE));
        /* TODO: How to handle the first information page???  */
        if (as != NULL) {
            vmm_map(as, ptr, phys_ptr, NUM_PAGES(length), pf, false);
            klogd("k_vm_map: 0x%x map 0x%x to 0x%x with %d pages\n",
                  as, phys_ptr, ptr, NUM_PAGES(length)); 
        } else {
            kpanic("k_vm_map: address space manager does not exist\n");
        }

        mem_map_t m;
        m.vaddr = ptr;
        m.paddr = phys_ptr;
        m.np = NUM_PAGES(length);
        m.flags = pf; 

        vec_push_back(&t->mmap_list, m); 

        return ptr;
    } else {
        ptr = VIRT_TO_PHYS(kmalloc(NUM_PAGES(length) * PAGE_SIZE));
        if (as != NULL) {
            vmm_map(as, ptr, ptr, NUM_PAGES(length), pf, false);
            klogd("k_vm_map: 0x%x map 0x%x to 0x%x with %d pages\n",
                  as, ptr, ptr, NUM_PAGES(length)); 
        } else {
            kpanic("k_vm_map: address space manager does not exist\n");
        }

        mem_map_t m;
        m.vaddr = ptr;
        m.paddr = ptr;
        m.np = NUM_PAGES(length);
        m.flags = pf; 

        vec_push_back(&t->mmap_list, m); 

        return (uint64_t)ptr;
    }
}

static int get_full_path(int64_t dirfd, const char *path, char *full_path)
{
    /* Clean the full path buffer */
    full_path[0] = '\0';

    if ((int32_t)dirfd == (int32_t)VFS_FDCWD) {
        /* Get the parent path name from TCB (task control block) */
        task_t *t = sched_get_current_task();
        if (t != NULL) {
            if (path[0] != '/' ) strcpy(full_path, t->cwd);
        } else {
            cpu_set_errno(EINVAL);
            return -1;
        }
    } else if ((int32_t)dirfd >= (int32_t)0) {
        /* Get the parent path name from dirfd */
        vfs_node_desc_t *tnode = vfs_handle_to_fd((vfs_handle_t)dirfd);
        if (tnode != NULL) {
            if (path[0] == '.') strcpy(full_path, tnode->path);
        } else {
            cpu_set_errno(EINVAL);
            return -1;
        }
    }

    if (strcmp(path, ".") == 0) {
        return 0;
    }

    if (path[0] == '/') {
        strcpy(full_path, "/");
    }

    /* Extracted folder name one by one */
    char temp_path[VFS_MAX_PATH_LEN] = {0};
    char *curr = NULL, *child = NULL;

    strcpy(temp_path, path);
    curr = temp_path;

    while (true) {
        child = strchr(curr, '/');
        if (child != NULL) {
            *child = '\0';
            child++;
        }
        if (strcmp(curr, "..") == 0) {
            /* Change full path to parent folder */
            bool succ = false;
            if (strlen(full_path) > 0) {
                size_t fpl = strlen(full_path);
                if (fpl > 0 && full_path[fpl - 1] == '/') {
                    full_path[fpl - 1] = '\0';
                }
                fpl = strlen(full_path);
                if (fpl > 0) {
                    for (size_t i = fpl - 1; ; i--) {
                        if (full_path[i] == '/') {
                            full_path[(i > 0) ? i : (i + 1)] = '\0';
                            succ = true;
                            break;
                        }
                        if (i == 0) break;
                    }
                }
            }
            if (!succ) {
                cpu_set_errno(EINVAL);
                return -1;
            }
        } else if (strcmp(curr, ".") == 0) {
            /* Do nothing */
        } else if (strlen(curr) == 0) {
            /* Do nothing */
        } else {
            /* Make sure the parent path name ends with '/' */
            size_t fpl = strlen(full_path);
            if (fpl > 0) {
                if (full_path[fpl - 1] != '/') strcat(full_path, "/");
            } else {
                strcpy(full_path, "/");
            }
            strcat(full_path, curr);
        }

        /* Move to next folder */
        if (child != NULL) {
            curr = child;
        } else {
            break;
        }
    }

    return 0;
}

int64_t k_openat(int64_t dirfd, char *path, int64_t flags, int64_t mode)
{
    /* "mode" is always zero */
    (void)mode;
    cpu_set_errno(0);

    char full_path[VFS_MAX_PATH_LEN] = {0};
    if (get_full_path(dirfd, path, full_path) < 0) {
        return -1;
    }

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
        cpu_set_errno(EBADF);
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
    task_t *t = sched_get_current_task();
    klogd("k_set_fs_base: task #%d set to 0x%x\n",
          t == NULL ? 0 : t->tid, val);
    write_msr(MSR_FS_BASE, val);
    if (t != NULL) t->fs_base = val;
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

int64_t k_fstatat(int64_t dirfd, const char *path, int64_t statbuf, int64_t flags)
{
    (void)flags;

    char full_path[VFS_MAX_PATH_LEN] = {0};
    if (get_full_path(dirfd, path, full_path) < 0) {
        return -1;
    }

    klogd("k_fstatat: dirfd 0x%x and path %s(%s)\n", dirfd, full_path, path);

    vfs_tnode_t *node = vfs_path_to_node(full_path, NO_CREATE, 0);

    if (node != NULL) {
        vfs_stat_t *st = (vfs_stat_t*)statbuf;
        memcpy(st, &(node->st), sizeof(vfs_stat_t));
        cpu_set_errno(0); 
        return 0;
    } else {
        cpu_set_errno(ENOENT);
        return -1;
    }
}

int64_t k_fstat(int64_t handle, int64_t statbuf)
{
    vfs_node_desc_t *fd = vfs_handle_to_fd(handle);
    cpu_set_errno(0);

    if (fd != NULL) {
        vfs_stat_t *st = (vfs_stat_t*)statbuf;
        memcpy(st, &(fd->tnode->st), sizeof(vfs_stat_t));
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
        klogd("k_getpid: task #%d\n", t->tid);
        if (t->tid >= 1) return t->tid;
    }

    cpu_set_errno(EINVAL);
    return -1;
}

int64_t k_chdir(char *dir)
{
    task_t *t = sched_get_current_task();
    cpu_set_errno(0);

    if (dir == NULL) {
        cpu_set_errno(EINVAL);
        goto err_exit;
    }

    if (strlen(dir) == 0) {
        cpu_set_errno(ENOENT);
        goto err_exit;
    }

    if (t == NULL) {
        cpu_set_errno(ENODEV);
        goto err_exit;
    }

    if (t->tid < 1) {
        cpu_set_errno(ESRCH);
        goto err_exit;
    }

    char cwd[VFS_MAX_PATH_LEN] = {0};
    char parent[VFS_MAX_PATH_LEN] = {0};
    char currdir[VFS_MAX_PATH_LEN] = {0};

    int64_t ret = vfs_get_parent_dir(dir, parent, currdir);

    if (ret < 0) {
        strcpy(cwd, dir);
    } else {
        if (strcmp(currdir, ".") == 0) {
            strcpy(cwd, parent);
        } else if (strcmp(currdir, "..") == 0) {
            char top_path[VFS_MAX_PATH_LEN] = {0};
            ret = vfs_get_parent_dir(parent, top_path, currdir);
            if (ret < 0) {
                strcpy(cwd, "/");
            } else {
                strcpy(cwd, top_path);
            }
        } else {
            strcpy(cwd, dir);
        }
    }

    if (vfs_path_to_node(cwd, NO_CREATE, 0) == NULL) {
        cpu_set_errno(ENOENT);
        goto err_exit;
    }

    strcpy(t->cwd, cwd);
    return 0;
err_exit:
    return -1; 
}

int64_t k_readdir(int64_t handle, uint64_t buff)
{
    dirent_t *de = (dirent_t*)buff;
    vfs_node_desc_t *fd = vfs_handle_to_fd(handle);
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

    strcpy(de->d_name, fd->curr_dir_ent->name);
   
    de->d_ino = fd->curr_dir_ent->st.st_ino;
    de->d_off = 0;
    de->d_reclen = sizeof(dirent_t);
    de->d_type = DT_UNKNOWN;
 
    return 0;
err_exit:
    cpu_set_errno(errno);
    return -1; 
}

int64_t k_fork()
{
    task_t *t = sched_get_current_task();
    cpu_set_errno(0);

    if (t == NULL) {
        cpu_set_errno(ENODEV);
        goto err_exit;
    }   

    if (t->tid < 1) {
        cpu_set_errno(ESRCH);
        goto err_exit;
    }   

    task_id_t tid_child = sched_fork();
    task_t *curr_task = sched_get_current_task();

    klogd("k_fork: parent task id #%d, current task id #%d, PML4 0x%x, "
          "sched_fork() returns #%d\n",
          t->tid, sched_get_tid(), curr_task->addrspace->PML4, tid_child);

    if (tid_child == TID_MAX) {
        cpu_set_errno(ECHILD);
        return -1;
    } else if (t->tid == sched_get_tid()) {
        /*
         * This should be parent process and returns child task id, but
         * currently it returns parent task id
         */
        return tid_child;
    } else {
        /* This should be child process and returns 0 */
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
    klogd("k_fcntl: fd 0x%x, request 0x%x, arg 0x%x\n", fd, request, arg);
    cpu_set_errno(ENOSYS);
    return -1; 
}

int64_t k_waitpid(int64_t pid, int64_t *status, int64_t flags)
{
    task_t *t = sched_get_current_task();
    if ((int32_t)pid == (int32_t)(-1) && t != NULL) {
        klogd("k_waitpid: tid %d waits pid 0x%x status 0x%x flags 0x%x\n",
              t->tid, pid, status, flags);

        cpu_set_errno(0);

        bool all_dead = true;
        size_t len = vec_length(&(t->child_list));

        for (size_t i = 0; i < len; i++) {
            task_id_t tid_child = vec_at(&(t->child_list), i);
            if (sched_get_task_status(tid_child) == TASK_DEAD) {
                klogd("     tid %d : child tid %d DEAD\n", t->tid, tid_child);
                if (status != NULL) *status = (int64_t)NULL;
                return tid_child;
            }
            if (sched_get_task_status(tid_child) != TASK_UNKNOWN) {
                all_dead = false;
                klogd("     tid %d : child tid %d ACTIVE\n", t->tid, tid_child);
            }
        }
  
        if (!all_dead) {
            sched_sleep(200);
            klogd("k_waitpid: tid %d waiting pid 0x%x returns with "
                  "active children\n", t->tid, pid);
            return 0;
        } else {
            klogd("k_waitpid: tid %d waiting pid 0x%x returns without "
                  "children\n", t->tid, pid);
            cpu_set_errno(ECHILD);
            return -1;
        }
    } else {
        klogd("k_waitpid: waiting pid 0x%x with invalid parameters\n", pid);
        cpu_set_errno(ECHILD);
        return -1;
    }
}

void k_exit(int64_t status)
{
    task_t *t = sched_get_current_task();
    if (t != NULL) {
        klogd("k_exit: task %d exit with status %d\n", t->tid, status);
    }
    sched_exit(status);
}

int k_getcwd(char *buffer, size_t size)
{
    task_t *t = sched_get_current_task();
    cpu_set_errno(0);

    if (buffer == NULL || size <= 0) {
        cpu_set_errno(EINVAL);
        goto err_exit;
    }   

    if (t == NULL) {
        cpu_set_errno(ENODEV);
        goto err_exit;
    }

    if (t->tid < 1) {
        cpu_set_errno(ESRCH);
        goto err_exit;
    }

    size_t len = strlen(t->cwd);
    if (len < size - 1) {
        strcpy(buffer, t->cwd);
    } else {
        cpu_set_errno(ENAMETOOLONG);
        goto err_exit;
    }

    return 0;

err_exit:
    return -1;
}

int k_getrusage(int64_t who, uint64_t usage) {
    rusage_t *u = (rusage_t*)usage;

    klogi("SYSCALL: get %d rusage\n", who);
    memset(u, sizeof(rusage_t), 0);

    return 0;
}

int64_t k_execve(const char *path, const char *argv[], const char *envp[])
{
    char *cwd = NULL;
    task_t *t = sched_get_current_task();
    if (t != NULL) cwd = t->cwd;

    if (sched_execve(path, argv, envp, cwd) != NULL) {
        sched_exit(0);
        cpu_set_errno(0);
        return 0;
    } else {
        cpu_set_errno(EINVAL);
        return -1;
    }
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
    [SYSCALL_FORK]          = (syscall_ptr_t)k_fork,
    [SYSCALL_EXECVE]        = (syscall_ptr_t)k_execve,
    (syscall_ptr_t)k_not_implemented,                           /* 16 */
    [SYSCALL_FSTATAT]       = (syscall_ptr_t)k_fstatat,
    [SYSCALL_FSTAT]         = (syscall_ptr_t)k_fstat,
    [SYSCALL_GETPPID]       = (syscall_ptr_t)k_getppid,
    [SYSCALL_FCNTL]         = (syscall_ptr_t)k_fcntl,
    (syscall_ptr_t)k_not_implemented,
    [SYSCALL_WAITPID]       = (syscall_ptr_t)k_waitpid,
    [SYSCALL_EXIT]          = (syscall_ptr_t)k_exit,
    [SYSCALL_READDIR]       = (syscall_ptr_t)k_readdir,
    (syscall_ptr_t)k_not_implemented,                           /* 25 */
    [SYSCALL_GETCWD]        = (syscall_ptr_t)k_getcwd,
    [SYSCALL_GETRUSAGE]     = (syscall_ptr_t)k_getrusage,
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
