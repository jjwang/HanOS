/**-----------------------------------------------------------------------------

 @file    filebase.h
 @brief   Path and file-descriptor helpers for the syscall layer

 @details
 @verbatim

   Declares the two VFS helpers left in the kernel: absolute path building and
   handle-to-descriptor resolution. The namespace itself is in user space.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <lib/spinlock.h>
#include <lib/klog.h>
#include <proc/sched.h>
#include <fs/vfs.h>

int64_t vfs_get_full_path(int64_t dirfh, const char *path, char *full_path,
                          uint64_t full_path_size);

vfs_node_desc_t *vfs_handle_to_fd(vfs_handle_t handle, const char *func);
