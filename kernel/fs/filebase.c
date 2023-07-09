/**-----------------------------------------------------------------------------

 @file    filebase.c
 @brief   Implementation of fundamental VFS file node related functions
 @details
 @verbatim

  This file contains implementation of all inode (index node) and tnode (tree
  node) related functions, e.g., alloc, free and node to fd (file descriptor),
  path to node conversions.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <fs/filebase.h>
#include <lib/kmalloc.h>
#include <lib/memutils.h>
#include <lib/string.h>
#include <lib/errno.h>
#include <sys/hpet.h>
#include <sys/cmos.h>

/* List of opened files */
vec_extern(vfs_node_desc_t*, vfs_openfiles);

/* Allocate a tnode in memory */
vfs_tnode_t *vfs_alloc_tnode(const char *name, vfs_inode_t *inode,
                             vfs_inode_t* parent)
{
    vfs_tnode_t* tnode = (vfs_tnode_t*)kmalloc(sizeof(vfs_tnode_t));

    memset(tnode, 0, sizeof(vfs_tnode_t));
    memcpy(tnode->name, name, sizeof(tnode->name));

    tnode->inode = inode;
    tnode->parent = parent;
    tnode->st.st_dev = vfs_new_dev_id();
    tnode->st.st_ino = vfs_new_ino_id();

    return tnode;
}

/* Allocate an inode in memory */
vfs_inode_t *vfs_alloc_inode(vfs_node_type_t type, uint32_t perms,
                             uint32_t uid, vfs_fsinfo_t* fs,
                             vfs_tnode_t* mountpoint)
{
    vfs_inode_t* inode = (vfs_inode_t*)kmalloc(sizeof(vfs_inode_t));
    memset(inode, 0, sizeof(vfs_inode_t));
    *inode = (vfs_inode_t) {
        .type = type,
        .perms = perms,
        .uid = uid,
        .fs = fs,
        .ident = NULL,
        .mountpoint = mountpoint,
        .refcount = 0,
        .size = 0
    };
    return inode;
}

/* Free a tnode, and the inode if needed */
void vfs_free_nodes(vfs_tnode_t* tnode)
{
    vfs_inode_t* inode = tnode->inode;
    if (inode->refcount <= 0)
        kmfree(inode);
    kmfree(tnode);
}

/* Return the node descriptor for a handle */
vfs_node_desc_t* vfs_handle_to_fd(vfs_handle_t handle)
{
    if ((size_t)handle >= vfs_openfiles.len + VFS_MIN_HANDLE
        || (size_t)handle < VFS_MIN_HANDLE
        || !(vfs_openfiles.data[handle - VFS_MIN_HANDLE])) {
        kloge("Invalid file handle %d\n", handle);
        return NULL;
    }
    return vfs_openfiles.data[handle - VFS_MIN_HANDLE];
}

/* Convert a path to a node, creates the node if required */
vfs_tnode_t* vfs_path_to_node(
    const char* path, uint8_t mode, vfs_node_type_t create_type)
{
    static char tmpbuff[VFS_MAX_PATH_LEN];
    vfs_tnode_t* curr = &vfs_root;

    /*  Only work with absolute paths */
    if (path[0] != '/') {
        kloge("'%s' is not an absolute path\n", path);
        return NULL;
    }
    path++; /* Skip the leading slash */

    size_t pathlen = strlen(path), curr_index;
    bool foundnode = true;
    for (curr_index = 0; curr_index < pathlen;) {
        /* Extract next token from the path */
        size_t i;
        for (i = 0; curr_index + i < pathlen; i++) {
            if (path[curr_index + i] == '/')
                break;
            tmpbuff[i] = path[curr_index + i];
        }
        tmpbuff[i] = '\0';
        curr_index += i + 1;

        /* Search for token in children of current node */
        foundnode = false;
        if (!IS_TRAVERSABLE(curr->inode))
            break;
        for (size_t i = 0; i < curr->inode->child.len; i++) {
            vfs_tnode_t* child = vec_at(&(curr->inode->child), i);
            if (strncmp(child->name, tmpbuff, sizeof(child->name)) == 0) {
                foundnode = true;
                curr = child;
                break;
            }
        }
    }

    /* Should we create the node */
    if (!foundnode) {
        /* Only folders can contain files */
        if (!IS_TRAVERSABLE(curr->inode)) {
            kloge("'%s' does not reside inside a folder\n", path);
            return NULL;
        }

        /* Create the node if CREATE was specified and
         * the node to be created is the last one in the path
         */
        if (mode & CREATE && curr_index > pathlen && IS_TRAVERSABLE(curr->inode)) {
            vfs_inode_t* new_inode = vfs_alloc_inode(
                create_type, 0/*777*/, 0, curr->inode->fs, curr->inode->mountpoint);

            uint64_t now_sec = hpet_get_nanos() / 1000000000;

            time_t boot_time = cmos_boot_time();
            time_t now_time = now_sec + boot_time;
            localtime(&now_time, &(new_inode->tm));
            
            vfs_tnode_t* new_tnode = 
                vfs_alloc_tnode(tmpbuff, new_inode, curr->inode);

            vec_push_back(&(curr->inode->child), new_tnode);
            if (curr->inode->fs != NULL) curr->inode->fs->mknode(new_tnode);
            if (strncmp(path, "usr/local", 9) == 0
                || strncmp(path, "/usr/bin", 8) == 0)
            {
                klogd("VFS: Create \"%s\" node\n", path);
            }

            /* Set the file mode and type */
            switch(create_type) {
            case VFS_NODE_FOLDER:
                new_tnode->st.st_mode |= S_IFDIR;
                break;
            case VFS_NODE_FILE:
                new_tnode->st.st_mode |= S_IFREG;
                break;
            case VFS_NODE_SYMLINK:
                new_tnode->st.st_mode |= S_IFLNK;
                break;
            default:
                break;
            }

            return new_tnode;
        } else {
            klogd("VFS: \"%s\" doesn't exist\n", path);
            cpu_set_errno(ENOENT);
            return NULL;
        }
    }
    /* The node should not have existed */
    else if (mode & ERR_ON_EXIST) {
        klogw("'%s' already exists\n", path);
        return NULL;
    }

    /* Found the node, return it */
    return curr;
}
