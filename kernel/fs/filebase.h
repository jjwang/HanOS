/**-----------------------------------------------------------------------------

 @file    filebase.h
 @brief   Definition of fundamental VFS file node related functions
 @details
 @verbatim

  This file contains definition of inode (index node) and tnode (tree node)
  related functions, e.g., alloc, free and node to fd (file descriptor), path
  to node conversions.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <base/lock.h>
#include <base/klog.h>
#include <proc/sched.h>
#include <fs/vfs.h>

#define IS_TRAVERSABLE(x) ((x)->type == VFS_NODE_FOLDER || (x)->type == VFS_NODE_MOUNTPOINT)

/* Modes for path to node conversion */
#define NO_CREATE       0b0001U
#define CREATE          0b0010U
#define ERR_ON_EXIST    0b0100U

extern lock_t vfs_lock;
extern vfs_tnode_t vfs_root;

vfs_tnode_t* vfs_alloc_tnode(const char* name, vfs_inode_t* inode, vfs_inode_t* parent);
vfs_inode_t* vfs_alloc_inode(vfs_node_type_t type, uint32_t perms, uint32_t uid, vfs_fsinfo_t* fs, vfs_tnode_t* mnt);
void vfs_free_nodes(vfs_tnode_t* tnode);
vfs_node_desc_t* vfs_handle_to_fd(vfs_handle_t handle);
vfs_tnode_t* vfs_path_to_node(const char* path, uint8_t mode, vfs_node_type_t create_type);
