#include <fs/vfs.h>
#include <fs/filebase.h>
#include <fs/fat32.h>
#include <lib/klog.h>
#include <lib/kmalloc.h>
#include <lib/lock.h>
#include <lib/string.h>
#include <lib/memutils.h>
#include <lib/vector.h>

/* vfs-wide lock */
lock_t vfs_lock;

/* root node */
vfs_tnode_t vfs_root;

/* list of installed filesystems */
vec_new_static(vfs_fsinfo_t*, vfs_fslist);

/* list of opened files */
vec_new(vfs_node_desc_t*, vfs_openfiles);

static void dumpnodes_helper(vfs_tnode_t* from, int lvl)
{
    for (int i = 0; i < 1 + lvl; i++)
        kprintf(" ");
    kprintf(" %d: [%s] -> %x inode (%d refs)\n", lvl, from->name, from->inode, from->inode->refcount);

    if (IS_TRAVERSABLE(from->inode))
        for (size_t i = 0; i < from->inode->child.len; i++)
            dumpnodes_helper(vec_at(&(from->inode->child), i), lvl + 1);
}

void vfs_debug()
{
    kprintf("Dumping VFS nodes:\n");
    dumpnodes_helper(&vfs_root, 0);
    kprintf("Dumping done.\n");
}

void vfs_register_fs(vfs_fsinfo_t* fs)
{
    vec_push_back(&vfs_fslist, fs);
}

vfs_fsinfo_t* vfs_get_fs(char* name)
{
    for (size_t i = 0; i < vfs_fslist.len; i++)
        if (strncmp(name, vfs_fslist.data[i]->name, sizeof(((vfs_fsinfo_t) { 0 }).name)) == 0)
            return vfs_fslist.data[i];

    kloge("Filesystem %s not found\n", name);
    return NULL;
}

void vfs_init()
{
    /* initialize the root folder */
    vfs_root.inode = vfs_alloc_inode(VFS_NODE_FOLDER, 0777, 0, NULL, NULL);

    vfs_register_fs(&fat32);

    vfs_path_to_node("/dev", CREATE, VFS_NODE_FOLDER);
    vfs_path_to_node("/disk", CREATE, VFS_NODE_FOLDER);

    klogi("VFS initialization finished\n");
}

/* creates a node with specified type */
int64_t vfs_create(char* path, vfs_node_type_t type)
{
    int64_t status = 0;
    lock_lock(&vfs_lock);

    vfs_tnode_t* node = vfs_path_to_node(path, CREATE | ERR_ON_EXIST, type);
    if (!node)
        status = -1;

    lock_release(&vfs_lock);
    return status;
}

/* changes permissions of node */
int64_t vfs_chmod(vfs_handle_t handle, int32_t newperms)
{
    vfs_node_desc_t* fd = vfs_handle_to_fd(handle);
    if (!fd)
        return -1;

    /* opened in read only mode */
    if (fd->mode == VFS_MODE_READ) {
        kloge("Opened as read-only\n");
        return -1;
    }

    /* set new permissions and sync */
    fd->inode->perms = newperms;
    fd->inode->fs->sync(fd->inode);
    return 0;
}

/* mounts a block device with specified filesystem at a path */
int64_t vfs_mount(char* device, char* path, char* fsname)
{
    lock_lock(&vfs_lock);

    /* get the fs info */
    vfs_fsinfo_t* fs = vfs_get_fs(fsname);
    if (!fs)
        goto fail;

    /* get the block device if needed */
    vfs_tnode_t* dev = NULL;
    if (!fs->istemp) {
        dev = vfs_path_to_node(device, NO_CREATE, 0);
        if (!dev)
            goto fail;
        if (dev->inode->type != VFS_NODE_BLOCK_DEVICE) {
            kloge("%s is not a block device\n", device);
            goto fail;
        }
    }

    /* get the node where it is to be mounted (should be an empty folder) */
    vfs_tnode_t* at = vfs_path_to_node(path, NO_CREATE, 0);
    if (!at)
        goto fail;
    if (at->inode->type != VFS_NODE_FOLDER || at->inode->child.len != 0) {
        kloge("'%s' is not an empty folder\n", path);
        goto fail;
    }
    kmfree(at->inode);

    /* mount the fs */
    at->inode = fs->mount(dev ? dev->inode : NULL);
    at->inode->mountpoint = at;

    klogi("Mounted %s at %s as %s\n", device ? device : "<no-device>", path, fsname);
    lock_release(&vfs_lock);
    return 0;
fail:
    lock_release(&vfs_lock);
    return -1;
}

/* read specified number of bytes from a file */
int64_t vfs_read(vfs_handle_t handle, size_t len, void* buff)
{
    vfs_node_desc_t* fd = vfs_handle_to_fd(handle);
    if (!fd) {
        return 0;
    }

    lock_lock(&vfs_lock);
    vfs_inode_t* inode = fd->inode;

    klogw("VFS: %s file size %d\n", fd->tnode->name, inode->size);

    /* truncate if asking for more data than available */
    if (fd->seek_pos + len > inode->size) {
        len = inode->size - fd->seek_pos;
        if (len == 0)
            goto end;
    }

    int64_t status = fd->inode->fs->read(fd->inode, fd->seek_pos, len, buff);
    if (status == -1)
        len = 0;

end:
    lock_release(&vfs_lock);
    return (int64_t)len;
}

/* write specified number of bytes to file */
int64_t vfs_write(vfs_handle_t handle, size_t len, const void* buff)
{
    vfs_node_desc_t* fd = vfs_handle_to_fd(handle);
    if (!fd)
        return 0;

    /* cannot write to read-only files */
    if (fd->mode == VFS_MODE_READ) {
        kloge("File handle %d is read only\n", handle);
        return 0;
    }

    lock_lock(&vfs_lock);
    vfs_inode_t* inode = fd->inode;

    /* expand file if writing more data than its size */
    if (fd->seek_pos + len > inode->size) {
        inode->size = fd->seek_pos + len;
        inode->fs->sync(inode);
    }

    int64_t status = inode->fs->write(inode, fd->seek_pos, len, buff);
    if (status == -1)
        len = 0;

    lock_release(&vfs_lock);
    return (int64_t)len;
}

/* seek to specified position in file */
int64_t vfs_seek(vfs_handle_t handle, size_t pos)
{
    vfs_node_desc_t* fd = vfs_handle_to_fd(handle);
    if (!fd)
        return -1;

    /* seek position is out of bounds and mode is read only */
    if (pos >= fd->inode->size && fd->mode == VFS_MODE_READ) {
        klogw("Seek position out of bounds\n");
        return -1;
    }

    fd->seek_pos = pos;
    return 0;
}

void vfs_get_parent_dir(const char* path, char* parent)
{
    strcpy(parent, path);

    uint64_t idx = strlen(parent) - 1;
    if (idx > 0) {
        if (parent[idx] == '/') idx--;
    }   

    while(idx > 0) {
        if (parent[idx] == '/') {
            parent[idx] = '\0';
            break;
        }   
        idx--;
    }   
}

vfs_handle_t vfs_open(char* path, vfs_openmode_t mode)
{
    lock_lock(&vfs_lock);

    /* find the node */
    vfs_tnode_t* req = vfs_path_to_node(path, NO_CREATE, 0);
    if (!req) {
        vfs_tnode_t* pn = NULL;
        char curpath[VFS_MAX_PATH_LEN] = {0}, parent[VFS_MAX_PATH_LEN] = {0};
        strcpy(curpath, path);
        while (true) {
            vfs_get_parent_dir(curpath, parent);
            if (strcmp(curpath, parent) == 0) break;
            pn = vfs_path_to_node(parent, NO_CREATE, 0);
            if (pn) break;
            strcpy(curpath, parent);
        }
        if (pn->inode->fs != NULL) {
            klogw("VFS: Can not open %s, visit back to %s\n", path, parent);
            req = pn->inode->fs->open(pn->inode, path);
        }
        if (!req) goto fail;
    }
    req->inode->refcount++;

    /* create node descriptor */
    vfs_node_desc_t* nd = (vfs_node_desc_t*)kmalloc(sizeof(vfs_node_desc_t));
    nd->tnode = req;
    nd->inode = req->inode;
    nd->seek_pos = 0;
    nd->mode = mode;

    /* add to current task */
    vec_push_back(&(vfs_openfiles), nd);

    /* return the handle */
    lock_release(&vfs_lock);

    vfs_handle_t h = ((vfs_handle_t)(vfs_openfiles.len - 1));
    klogv("VFS: Open %s and return handle %d\n", path, h);
    return h;
fail:
    lock_release(&vfs_lock);
    return -1;
}

int64_t vfs_close(vfs_handle_t handle)
{
    klogv("VFS: close file handle %d\n", handle);

    lock_lock(&vfs_lock);

    vfs_node_desc_t* fd = vfs_handle_to_fd(handle);
    if (!fd)
        goto fail;

    fd->inode->refcount--;
    kmfree(fd);
    vfs_openfiles.data[handle] = NULL;

    lock_release(&vfs_lock);
    return 0;
fail:
    lock_release(&vfs_lock);
    return -1;
}

/* get next directory entry */
int64_t vfs_getdent(vfs_handle_t handle, vfs_dirent_t* dirent) {
    int64_t status;
    vfs_node_desc_t* fd = vfs_handle_to_fd(handle);
    if (!fd)
        return -1;

    lock_lock(&vfs_lock);

    /* can only traverse folders */
    if (!IS_TRAVERSABLE(fd->inode)) {
        kloge("Node not traversable\n");
        status = -1;
        goto done;
    }

    /* we've reached the end */
    if (fd->seek_pos >= fd->inode->child.len) {
        status = 0;
        goto done;
    }

    /* initialize the dirent */
    vfs_tnode_t* entry = vec_at(&(fd->inode->child), fd->seek_pos);
    dirent->type = entry->inode->type;
    memcpy(dirent->name, entry->name, sizeof(entry->name));

    /* we're done here, advance the offset */
    status = 1;
    fd->seek_pos++;

done:
    lock_release(&vfs_lock);
    return status;
}

