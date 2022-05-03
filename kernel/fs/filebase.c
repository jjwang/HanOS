/*
    Common functions required by many VFS components
*/

#include <fs/filebase.h>
#include <lib/kmalloc.h>
#include <lib/memutils.h>
#include <lib/string.h>

/* list of opened files */
vec_extern(vfs_node_desc_t*, vfs_openfiles);

/* allocates a tnode in memory */
vfs_tnode_t* vfs_alloc_tnode(const char* name, vfs_inode_t* inode, vfs_inode_t* parent)
{
    vfs_tnode_t* tnode = (vfs_tnode_t*)kmalloc(sizeof(vfs_tnode_t));
    memset(tnode, 0, sizeof(vfs_tnode_t));
    memcpy(tnode->name, name, sizeof(tnode->name));
    tnode->inode = inode;
    tnode->parent = parent;
    return tnode;
}

/* allocates an inode in memory */
vfs_inode_t* vfs_alloc_inode(vfs_node_type_t type, uint32_t perms, uint32_t uid,
    vfs_fsinfo_t* fs, vfs_tnode_t* mountpoint)
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
        .refcount = 0
    };
    return inode;
}

/* frees a tnode, and the inode if needed */
void vfs_free_nodes(vfs_tnode_t* tnode)
{
    vfs_inode_t* inode = tnode->inode;
    if (inode->refcount <= 0)
        kmfree(inode);
    kmfree(tnode);
}

/* returns the node descriptor for a handle */
vfs_node_desc_t* vfs_handle_to_fd(vfs_handle_t handle)
{
    if ((size_t)handle >= vfs_openfiles.len || !(vfs_openfiles.data[handle])) {
        kloge("Invalid file handle %d\n", handle);
        return NULL;
    }
    return vfs_openfiles.data[handle];
}

/* converts a path to a node, creates the node if required */
vfs_tnode_t* vfs_path_to_node(const char* path, uint8_t mode, vfs_node_type_t create_type)
{
    static char tmpbuff[VFS_MAX_PATH_LEN];
    vfs_tnode_t* curr = &vfs_root;

    /*  only work with absolute paths */
    if (path[0] != '/') {
        kloge("'%s' is not an absolute path\n", path);
        return NULL;
    }
    path++; /* skip the leading slash */

    size_t pathlen = strlen(path), curr_index;
    bool foundnode = true;
    for (curr_index = 0; curr_index < pathlen;) {
        /* extract next token from the path */
        size_t i;
        for (i = 0; curr_index + i < pathlen; i++) {
            if (path[curr_index + i] == '/')
                break;
            tmpbuff[i] = path[curr_index + i];
        }
        tmpbuff[i] = '\0';
        curr_index += i + 1;

        /* search for token in children of current node */
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

    /* should we create the node */
    if (!foundnode) {
        /* only folders can contain files */
        if (!IS_TRAVERSABLE(curr->inode)) {
            kloge("'%s' does not reside inside a folder\n", path);
            return NULL;
        }

        /* create the node if CREATE was specified and
         * the node to be created is the last one in the path
         */
        if (mode & CREATE && curr_index > pathlen && IS_TRAVERSABLE(curr->inode)) {
            vfs_inode_t* new_inode = vfs_alloc_inode(create_type, 0777, 0, curr->inode->fs, curr->inode->mountpoint);
            vfs_tnode_t* new_tnode = vfs_alloc_tnode(tmpbuff, new_inode, curr->inode);

            vec_push_back(&(curr->inode->child), new_tnode);
            if (curr->inode->fs != NULL) curr->inode->fs->mknode(new_tnode);
            return new_tnode;
        } else {
            klogw("\"%s\" doesn't exist\n", path);
            return NULL;
        }
    }
    /* the node should not have existed */
    else if (mode & ERR_ON_EXIST) {
        klogw("'%s' already exists\n", path);
        return NULL;
    }

    /* found the node, return it */
    return curr;
}
