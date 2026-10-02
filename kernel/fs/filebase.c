/**-----------------------------------------------------------------------------

 @file    filebase.c
 @brief   Path and file-descriptor helpers for the syscall layer

 @details
 @verbatim

   The filesystem lives in userspace. The kernel keeps only two helpers: build
   an absolute path from a working directory and a relative path, and resolve a
   handle to the server-side descriptor it names.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <string.h>
#include <errno.h>

#include <fs/filebase.h>
#include <fs/vfs.h>
#include <proc/syscall.h>
#include <srv/process_srv.h>
#include <arch/x64/smp.h>

int64_t vfs_get_full_path(int64_t dirfh, const char *path, char *full_path,
                          uint64_t full_path_size)
{
    /* Clean the full path buffer */
    full_path[0] = '\0';

    if ((int32_t) dirfh == (int32_t) VFS_FDCWD) {
        /* Get the parent path name from TCB (process control block) */
        process_t *t = sched_get_current_process();
        if (t != NULL) {
            if (path[0] != '/')
                strcpy(full_path, t->cwd);
        } else {
            cpu_set_errno(EINVAL);
            return -1;
        }
    } else if ((int32_t) dirfh >= (int32_t) 0) {
        /* Get the parent path name from dirfh */
        vfs_node_desc_t *tnode =
            vfs_handle_to_fd((vfs_handle_t) dirfh, __func__);
        if (tnode != NULL) {
            if (path[0] == '.')
                strcpy(full_path, tnode->path);
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
    char temp_path[VFS_MAX_PATH_LEN] = { 0 };
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
                uint64_t fpl = strlen(full_path);
                if (fpl > 0 && full_path[fpl - 1] == '/') {
                    full_path[fpl - 1] = '\0';
                }
                fpl = strlen(full_path);
                if (fpl > 0) {
                    for (uint64_t i = fpl - 1;; i--) {
                        if (full_path[i] == '/') {
                            full_path[(i > 0) ? i : (i + 1)] = '\0';
                            succ = true;
                            break;
                        }
                        if (i == 0)
                            break;
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
            uint64_t fpl = strlen(full_path);
            if (fpl > 0) {
                if (full_path[fpl - 1] != '/')
                    strncat(full_path, "/", full_path_size);
            } else {
                strcpy(full_path, "/");
            }
            strncat(full_path, curr, full_path_size);
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

/* Per-CPU scratch descriptor. The fd table lives in the process server, so a
 * handle is resolved there and copied here just for the current operation. */
static vfs_node_desc_t transient_fd[CPU_MAX];

/* Return the node descriptor for a handle */
vfs_node_desc_t *vfs_handle_to_fd(vfs_handle_t handle, const char *func)
{
    process_t *t = sched_get_current_process();
    if (t == NULL)
        return NULL;

    int kind = 0, svc = 0;
    int64_t sfd = 0;
    uint64_t size = 0, seek = 0;

    if (process_fd_get((int) handle, &kind, &svc, &sfd, &size, &seek) != 0) {
        klogw
            ("VFS: %s() cannot locate %ld (0x%016lx) in file list of process %ld\n",
             func, (long) handle, (long) handle, (long) t->pid);
        return NULL;
    }

    vfs_node_desc_t *fd = &transient_fd[smp_get_current_cpu_id()];

    memset(fd, 0, sizeof(*fd));
    fd->server = true;
    fd->svc = svc;
    fd->server_fd = sfd;
    fd->server_size = size;
    fd->seek_pos = seek;
    return fd;
}
