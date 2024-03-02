/* Ref: Page 2994 in Intel® 64 and IA-32 Architectures Software Developer’s
 * Manual Combined Volumes: 1, 2A, 2B, 2C, 2D, 3A, 3B, 3C, 3D, and 4
 */
#include <libc/string.h>
#include <libc/errno.h>
#include <libc/numeric.h>

#include <sys/cpu.h>
#include <sys/idt.h>
#include <sys/apic.h>
#include <sys/panic.h>
#include <sys/isr_base.h>
#include <base/klog.h>
#include <base/vector.h>
#include <proc/task.h>
#include <proc/sched.h>
#include <proc/syscall.h>
#include <proc/eventbus.h>
#include <fs/filebase.h>
#include <fs/vfs.h>
#include <fs/ttyfs.h>
#include <device/keyboard/keyboard.h>
#include <device/display/term.h>

#define MMAP_ANON_BASE      0x80000000000

extern int64_t syscall_handler();

typedef int64_t (*syscall_ptr_t)(void);

extern lock_t vfs_lock;
extern lock_t sched_lock;

static bool debug_info = false;

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
        klogd("debug: %s", message);
    } else {
        klogd("debug: %s\n", message);
    }

    return strlen(message);
}

/*
 * Need to use prot parameter - PROT_READ (0x01), PROT_WRITE (0x02),
 * PROT_EXEC (0x04).
 */
uint64_t k_vm_map(uint64_t *hint, uint64_t length, uint64_t prot,
                  uint64_t flags, uint64_t fd, uint64_t offset)
{
    (void)fd;
    (void)offset;

    cpu_set_errno(0);

    task_t *t = sched_get_current_task();
    addrspace_t *as = NULL;

    if (t != NULL) {
        if (t->tid < 1) kpanic("SYSCALL: %s meets corrupted tid\n", __func__);
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

    size_t pf = VMM_FLAGS_DEFAULT | VMM_FLAGS_USERMODE;
    uint64_t ptr = (uint64_t)hint;
    uint64_t np = NUM_PAGES(length);

    /* TODO: How to handle the first information page???  */

    /* Unmap before mapping to a new malloc-ed memory block */
    if (ptr != (uint64_t)NULL) vmm_unmap(as, ptr, np);

    uint64_t phys_ptr = VIRT_TO_PHYS(kmalloc(np * PAGE_SIZE));

    /* On QEMU, the memory will be set to zero. But on real hardaware,
     * maybe they will not be set to zero. Need to do this!
     */
    memset((void*)PHYS_TO_VIRT(phys_ptr), 0, np * PAGE_SIZE);

    if (!(flags & MAP_FIXED)) {
        ptr = phys_ptr + MMAP_ANON_BASE;
    }

    vmm_map(as, ptr, phys_ptr, NUM_PAGES(length), pf);

    if (debug_info) {
        klogi("k_vm_map: tid %d #%d 0x%x(PML4 0x%x) map 0x%x to 0x%x with %d "
              "pages, prot 0x%x, flags 0x%x\n",
              t->tid, vec_length(&t->mmap_list), as, as->PML4, phys_ptr, ptr,
              np, prot, flags);
    }

    mem_map_t m = {0};

    m.vaddr = ptr;
    m.paddr = phys_ptr;
    m.np = NUM_PAGES(length);
    m.flags = pf;

    lock_lock(&sched_lock);
    vec_push_back(&t->mmap_list, m);
    lock_release(&sched_lock);

    return ptr;

err_exit:
    kloge("k_vm_map: tid %d 0x%x(PML4 0x%x) returns NULL in malloc()\n",
          t->tid, as, as->PML4);
    return (uint64_t)NULL;
}

int64_t k_vm_unmap(void *ptr, size_t size)
{
    /* Need to implement memory free */
    cpu_set_errno(0);

    task_t *t = sched_get_current_task();
    addrspace_t *as = NULL;

    if (t != NULL) {
        if (t->tid < 1) kpanic("SYSCALL: %s meets corrupted tid\n", __func__);
        as = t->addrspace;
    }

    if (size == 0) {
        cpu_set_errno(EINVAL);
        goto err_exit;
    }

    uint64_t np = NUM_PAGES(size);
    vmm_unmap(as, (uint64_t)ptr, np);

    if (debug_info) {
        klogi("k_vm_unmap: 0x%x(PML4 0x%x) unmap 0x%x with %d pages\n",
              as, as->PML4, ptr, np);
    }

    return 0;

err_exit:
    cpu_set_errno(EINVAL);
    return -1;
}

int get_full_path(int64_t dirfh, const char *path, char *full_path)
{
    /* Clean the full path buffer */
    full_path[0] = '\0';

    if ((int32_t)dirfh == (int32_t)VFS_FDCWD) {
        /* Get the parent path name from TCB (task control block) */
        task_t *t = sched_get_current_task();
        if (t != NULL) {
            if (path[0] != '/' ) strcpy(full_path, t->cwd);
        } else {
            cpu_set_errno(EINVAL);
            return -1;
        }
    } else if ((int32_t)dirfh >= (int32_t)0) {
        /* Get the parent path name from dirfh */
        vfs_node_desc_t *tnode = vfs_handle_to_fd((vfs_handle_t)dirfh);
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

int64_t k_openat(int64_t dirfh, char *path, int64_t flags, int64_t mode)
{
    /* "mode" is always zero */
    (void)mode;
    cpu_set_errno(0);

    char full_path[VFS_MAX_PATH_LEN] = {0};
    if (get_full_path(dirfh, path, full_path) < 0) {
        kloge("k_openat: cannot get full path for \"%s\"\n", path);
        cpu_set_errno(EINVAL);
        return -1;
    } else {
        /* Check whether folder exists or not, e.g. filename is "1/txt" */
        size_t len = strlen(full_path);
        if (len == 0) {
            kloge("k_openat: full path of \"%s\" is null\n", path);
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
                kloge("k_openat: directory \"%s\" doesn't exist\n", full_path);
                cpu_set_errno(ENOENT);
                return -1;
            }
        }
        if (get_full_path(dirfh, path, full_path) < 0) {
            kloge("k_openat: full path of \"%s\" cannot be got\n", path);
            cpu_set_errno(EINVAL);
            return -1;
        }
    }

    vfs_openmode_t openmode = VFS_MODE_READWRITE;
    int32_t perms = 0;
    switch(flags & 0x7) {
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
            kloge("k_openat: creating file for \"%s\" failed\n", path);
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

    klogi("k_openat: dirfh 0x%x, path %s and flags 0x%x\n", dirfh, path, flags);
    return vfs_open(full_path, openmode);
}

int64_t k_unlink(char *path)
{
    cpu_set_errno(0);

    klogi("k_unlink: %s\n", path);

    char full_path[VFS_MAX_PATH_LEN] = {0};
    if (get_full_path(VFS_FDCWD, path, full_path) < 0) {
        cpu_set_errno(EINVAL);
        return -1;
    } else {
        /* Check whether folder exists or not, e.g. filename is "1/txt" */
        size_t len = strlen(full_path);
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
                klogd("k_openat: directory \"%s\" doesn't exist\n", full_path);
                cpu_set_errno(ENOENT);
                return -1;
            }
        }
        if (get_full_path(VFS_FDCWD, path, full_path) < 0) {
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
    for (size_t i = 0; i < vec_length(&(pi->child)); i++) {
        if (vec_at(&(pi->child), i) == tnode) {
            vec_erase(&(pi->child), i);
            return 0;
        }
    }

    cpu_set_errno(ENOENT);
    return -1;
}

int64_t k_seek(int64_t fh, int64_t offset, int64_t whence)
{
    cpu_set_errno(0);

    if (fh == STDIN || fh == STDOUT || fh == STDERR) {
        klogv("k_seek: fh %d(0x%x), offset %d, whence %d\n",
              fh, fh, offset, whence);
        return 0;
    }

    int64_t ret = vfs_seek(fh, offset, whence);

    klogd("k_seek: fh %d(0x%x), offset %d, whence %d and return %d\n",
          fh, fh, offset, whence, ret);
    if (ret < 0) cpu_set_errno(EINVAL);

    return ret;
}

int64_t k_close(int64_t fh)
{
    task_t *t = sched_get_current_task();
    klogd("k_close: close file handle %d\n", fh);

    if (fh == STDIN || fh == STDOUT || fh == STDERR) {
        return 0;
    }

    if (t != NULL) {
        lock_lock(&vfs_lock);
        /* Check whether there is file redirection */
        for (size_t i = 0; i < vec_length(&t->dup_list); i++) {
            file_dup_t dup = vec_at(&t->dup_list, i); 
            if (dup.newfh == fh) {
                vec_erase(&t->dup_list, i);
                break;
            }   
        }
        lock_release(&vfs_lock);
    }
 
    return vfs_close(fh);
}

int64_t k_read(int64_t fh, void* buf, size_t count)
{
    task_t *t = sched_get_current_task();
    cpu_set_errno(0);

    if (fh == STDIN) {
        bool found = false;
        vfs_handle_t oldfh = -1; 
        if (t != NULL) {
            lock_lock(&vfs_lock);
            /* Check whether it is redirected from some file */
            for (size_t i; i < vec_length(&t->dup_list); i++) {
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
            lock_release(&vfs_lock);
        }
        if (found) {
            int64_t ret = vfs_read(oldfh, count, buf);
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
        klogd("k_read: try to read %d bytes from file %d and return %d bytes\n",
              count, fh, len);
        return len;
    } else {
        cpu_set_errno(EBADF);
        return -1;
    }
}

int64_t k_write(int64_t fh, const void* buf, size_t count)
{
    task_t *t = sched_get_current_task();
    cpu_set_errno(0);

    if (fh == STDOUT || fh == STDERR) {
        bool found = false;
        vfs_handle_t oldfh = -1; 
        if (t != NULL) {
            lock_lock(&vfs_lock);
            /* Check whether it is redirected from some file */
            for (size_t i; i < vec_length(&t->dup_list); i++) {
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
            lock_release(&vfs_lock);
        }   
        if (found) {
            klogd("k_write: write %d bytes to oldfh %d <- fh %d\n",
                  count, oldfh, fh); 
            int64_t ret = vfs_write(oldfh, count, buf);
            return ret;
        } else {
            if (debug_info) {
                for (size_t i = 0; i < count; i++) {
                    char c = ((char*)buf)[i];
                    if ((c >= '0' && c <= '9') || (c >= 'a' && c <= 'z')
                        || (c >= 'A' && c <= 'Z') || c == '[')
                    {
                        klogd("k_write: write [%c]\n", c);
                    } else {
                        klogd("k_write: write [0x%2x]\n", c);
                    }
                }
            }
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
        kloge("k_write: invalid file handler fh=%d\n", fh);
        cpu_set_errno(EPERM);
        return -1;
    }

    return vfs_write(fh, count, buf);
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

int64_t k_fstatat(int64_t dirfh, const char *path, int64_t statbuf, int64_t flags)
{
    (void)flags;

    char full_path[VFS_MAX_PATH_LEN] = {0};
    if (get_full_path(dirfh, path, full_path) < 0) {
        return -1;
    }

    vfs_tnode_t *node = vfs_path_to_node(full_path, NO_CREATE, 0);

    if (node != NULL) {
        vfs_stat_t *st = (vfs_stat_t*)statbuf;
        memcpy(st, &(node->st), sizeof(vfs_stat_t));
        klogd("k_fstatat: success with dirfh 0x%x and path %s(%s), size %d\n",
              dirfh, full_path, path, st->st_size);
        cpu_set_errno(0); 
        return 0;
    } else {
        klogd("k_fstatat: fail with dirfh 0x%x and path %s(%s)\n",
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
        vfs_stat_t *st = (vfs_stat_t*)statbuf;
        memset(st, 0, sizeof(vfs_stat_t));
        klogd("k_fstat: success with file handle %d\n", handle);
        return 0;
    }
 
    vfs_node_desc_t *fd = vfs_handle_to_fd(handle);
    cpu_set_errno(0);

    if (fd != NULL) {
        vfs_stat_t *st = (vfs_stat_t*)statbuf;
        memcpy(st, &(fd->tnode->st), sizeof(vfs_stat_t));
        klogd("k_fstat: success with file handle %d and size %d\n",
              handle, st->st_size);
        return 0;
    } else {
        kloge("k_fstat: fail with file handle %d\n", handle);
        cpu_set_errno(EINVAL);
        return -1;
    }
}

/* TODO: Currently skip the parameter - flags */
int64_t k_faccessat(int64_t dirfh, const char *path, uint64_t mode, uint64_t flags)
{
    (void)flags;

    cpu_set_errno(0);

    char full_path[VFS_MAX_PATH_LEN] = {0};
    if (get_full_path(dirfh, path, full_path) < 0) {
        cpu_set_errno(EBADF);
        return -1; 
    }

    klogi("k_faccessat: open \"%s\" at mode 0x%x and flags 0x%x\n",
          full_path, mode, flags);

    vfs_tnode_t *node = vfs_path_to_node(full_path, NO_CREATE, 0);
    
    if (node != NULL) {
        uint32_t perms  = node->inode->perms;
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

    if (t->tid < 1) {
        cpu_set_errno(ESRCH);
        goto err_exit;
    }

    char fullpath[VFS_MAX_PATH_LEN] = {0};
    char parent[VFS_MAX_PATH_LEN] = {0};
    char currdir[VFS_MAX_PATH_LEN] = {0};

    size_t k = 0;
    size_t len = strlen(dir);

    strcpy(fullpath, t->cwd);

    /* Note that "i" will loop to last character "\0" */
    for (size_t i = 0; i < len; i++) {
        if (dir[i] != '/') {
            currdir[k++] = dir[i];
            if (i != len - 1) continue;
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
            size_t fpl = strlen(fullpath);
            if (fpl > 0) {
                if (fullpath[fpl - 1] != '/') strcat(fullpath, "/");
            }
            strcat(fullpath, currdir);
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

int64_t k_meminfo()
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

    pmm_dump_usage();
    return 0;

err_exit:
    return -1;
}

int64_t k_pipe(int32_t *fh, uint32_t flags)
{
    (void)flags;

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

    char path[VFS_MAX_PATH_LEN] = {0};
    strcpy(path, "/dev/pipe/");

    size_t len = strlen(path);
    itoa(rand(sched_get_ticks() % 1000, 1, 1000),
         &path[len], VFS_MAX_PATH_LEN - len - 1,
         10);

    vfs_create(path, VFS_NODE_CHAR_DEVICE);

    /* fh[0] is the reading port, fh[1] is the writing port */
    fh[0] = vfs_open(path, VFS_MODE_READ);
    fh[1] = vfs_open(path, VFS_MODE_WRITE);

    klogd("k_pipe: return reading port %d and writing port %d\n", fh[0], fh[1]);

    return 0;

err_exit:
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

int64_t k_waitpid(int64_t pid, int32_t *status, int32_t flags)
{
    task_t *t = sched_get_current_task();
    if (status != NULL) *status = 0;

    if ((int32_t)pid == (int32_t)(-1) && t != NULL) {
        klogv("k_waitpid: tid %d waits pid 0x%x status 0x%x flags 0x%x\n",
              t->tid, pid, status, flags);

        cpu_set_errno(0);

        bool all_dead = true;
        size_t len = vec_length(&(t->child_list));

        for (size_t i = 0; i < len; i++) {
            task_id_t tid_child = vec_at(&(t->child_list), i);
            task_status_t status_child = sched_get_task_status(tid_child);
            if (status_child == TASK_DEAD) {
                klogw("     tid %d : child tid %d DEAD\n", t->tid, tid_child);
                return tid_child;
            } else if (status_child != TASK_UNKNOWN) {
                all_dead = false;
                klogv("     tid %d : child tid %d ACTIVE\n", t->tid, tid_child);
            }
        }
  
        if (!all_dead) {
            sched_sleep(200);
            klogv("k_waitpid: tid %d waiting pid 0x%x returns with "
                  "active children\n", t->tid, pid);
            return 0;
        } else {
            sched_sleep(200);
            klogd("k_waitpid: tid %d waiting pid 0x%x returns without "
                  "children\n", t->tid, pid);
            cpu_set_errno(ECHILD);
            return -1;
        }
    } else if (t->tid == (task_id_t)pid) {
        /* It is ourselves, return immediately */
        return 0;
    } else {
        /* Retry for 20 times */
        for (size_t i = 0; ; i++) {
            task_status_t status = sched_get_task_status(pid);
            if (status != TASK_DEAD && status != TASK_UNKNOWN) {
                sched_sleep(200);
                if (i == 19) {
                    kloge("k_waitpid: waiting pid 0x%x which is still active\n", pid);
                    cpu_set_errno(EBUSY);
                    return -1;
                }
            }
        }
        klogd("k_waitpid: waiting pid 0x%x which is not active and exit\n", pid);
        return 0;
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

    /* When gcc is launched, it will call getrusage(). We need to dive into
     * gcc to know the purpose of this function call.
     */
    sched_sleep(1000);
    klogw("SYSCALL: get %d rusage\n", who);
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

int k_getclock(void *_, int64_t which, vfs_timespec_t *out) {
    (void)_;

    int64_t ret = -1; 
    cpu_set_errno(0);

    uint64_t now_sec = hpet_get_nanos() / 1000000000;
    uint64_t now_ns = hpet_get_nanos();

    time_t boot_time = cmos_boot_time();

    switch (which) {
        case CLOCK_REALTIME:
        case CLOCK_REALTIME_COARSE:
            *out = (vfs_timespec_t)
                        {.tv_sec = now_sec + boot_time,
                         .tv_nsec = now_ns + boot_time * 1000000000};
            ret = 0;
            goto cleanup;
        case CLOCK_BOOTTIME:
        case CLOCK_MONOTONIC:
        case CLOCK_MONOTONIC_RAW:
        case CLOCK_MONOTONIC_COARSE:
            *out = (vfs_timespec_t){.tv_sec = now_sec, .tv_nsec = now_ns};
            ret = 0;
            goto cleanup;
        case CLOCK_PROCESS_CPUTIME_ID:
        case CLOCK_THREAD_CPUTIME_ID:
            *out = (vfs_timespec_t){.tv_sec = 0, .tv_nsec = 0}; 
            ret = 0;
            goto cleanup;
    }

    cpu_set_errno(EINVAL);

cleanup:
    return ret;
}

int64_t k_readlink(int64_t dirfh, const char *path, void *buffer, size_t max_size)
{
    cpu_set_errno(0);

    char full_path[VFS_MAX_PATH_LEN] = {0};
    get_full_path(dirfh, path, full_path);

    vfs_tnode_t* tnode = vfs_path_to_node(full_path, NO_CREATE, 0);

    if (tnode == NULL)                          goto err_exit;
    if (tnode->inode->type != VFS_NODE_SYMLINK) goto err_exit;

    if ((size_t)strlen(tnode->inode->link) < max_size) {
        klogd("k_readlink: %s -> %s\n", full_path, tnode->inode->link);
        strcpy(buffer, tnode->inode->link);
    } else {
        goto err_exit;
    }
    return strlen(buffer);

err_exit:
    cpu_set_errno(EINVAL);
    return -1;
}

void k_uname(void)
{
}

int64_t k_dup3(int64_t fh, int64_t newfh, int64_t flags)
{
    task_t *t = sched_get_current_task();
    cpu_set_errno(0);

    if (t == NULL) {
        cpu_set_errno(ENOSYS);
        return -1;
    }

    klogd("k_dup3: tid %d fh %d <- newfh %d, flags 0x%x\n",
          t->tid, fh, newfh, flags);

    lock_lock(&vfs_lock);
    file_dup_t dup = {.fh = fh, .newfh = newfh};
    vec_push_back(&t->dup_list, dup);
    lock_release(&vfs_lock);

    return 0;
}

/* TODO: need to add futex implementation */
int64_t k_futex_wait(int64_t *ptr, vfs_timespec_t *tv, int64_t expected)
{
    klogi("k_futex_wait: time spec (%d, %d) with ptr 0x%x, val %d and "
          "expected %d\n", tv->tv_sec, tv->tv_nsec, ptr, *ptr, expected);

    return 0;
}

int64_t k_futex_wake(int64_t *ptr)
{
    klogi("k_futex_wake: ptr 0x%x and val %d\n", ptr, *ptr);

    return 0;
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
    [SYSCALL_FACCESSAT]     = (syscall_ptr_t)k_faccessat,       /* 16 */
    [SYSCALL_FSTATAT]       = (syscall_ptr_t)k_fstatat,
    [SYSCALL_FSTAT]         = (syscall_ptr_t)k_fstat,
    [SYSCALL_GETPPID]       = (syscall_ptr_t)k_getppid,
    [SYSCALL_FCNTL]         = (syscall_ptr_t)k_fcntl,           /* 20 */
    [SYSCALL_DUP3]          = (syscall_ptr_t)k_dup3,
    [SYSCALL_WAITPID]       = (syscall_ptr_t)k_waitpid,
    [SYSCALL_EXIT]          = (syscall_ptr_t)k_exit,
    [SYSCALL_READDIR]       = (syscall_ptr_t)k_readdir,
    [SYSCALL_MUNMAP]        = (syscall_ptr_t)k_vm_unmap,        /* 25 */
    [SYSCALL_GETCWD]        = (syscall_ptr_t)k_getcwd,
    [SYSCALL_GETCLOCK]      = (syscall_ptr_t)k_getclock,
    [SYSCALL_READLINK]      = (syscall_ptr_t)k_readlink,
    [SYSCALL_GETRUSAGE]     = (syscall_ptr_t)k_getrusage,       /* 29 */
    (syscall_ptr_t)k_not_implemented,
    [SYSCALL_UNAME]         = (syscall_ptr_t)k_uname,
    [SYSCALL_FUTEX_WAIT]    = (syscall_ptr_t)k_futex_wait,
    [SYSCALL_FUTEX_WAKE]    = (syscall_ptr_t)k_futex_wake,
    [SYSCALL_MEMINFO]       = (syscall_ptr_t)k_meminfo,         /* 34 */
    [SYSCALL_PIPE]          = (syscall_ptr_t)k_pipe,
    [SYSCALL_UNLINK]        = (syscall_ptr_t)k_unlink,          /* 36 */
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
