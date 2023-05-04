/**-----------------------------------------------------------------------------

 @file    vfs.c
 @brief   Implementation of VFS related functions
 @details
 @verbatim

  Main VFS functions are implemented in this file. 

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <fs/vfs.h>
#include <fs/filebase.h>
#include <fs/fat32.h>
#include <fs/ramfs.h>
#include <fs/ttyfs.h>
#include <lib/klog.h>
#include <lib/kmalloc.h>
#include <lib/lock.h>
#include <lib/string.h>
#include <lib/memutils.h>
#include <lib/vector.h>

static bool vfs_initialized = false;

/* VFS wide lock */
lock_t vfs_lock = {0};

/* Stat structure related definitions */
lock_t dev_lock = {0};
lock_t ino_lock = {0};

static dev_t next_new_dev_id = 1;
static ino_t next_new_ino_id = 1;

/* Root node */
vfs_tnode_t vfs_root = {0};

/* List of installed filesystems */
vec_new_static(vfs_fsinfo_t*, vfs_fslist);

/* List of opened files */
vec_new(vfs_node_desc_t*, vfs_openfiles);

/* Stat structure related function implementations */
dev_t vfs_new_dev_id(void)
{
    dev_t dev_id;

    lock_lock(&dev_lock);
    dev_id = next_new_dev_id;
    next_new_dev_id++;
    lock_release(&dev_lock);

    return dev_id;    
}

ino_t vfs_new_ino_id(void)
{
    ino_t ino_id;

    lock_lock(&ino_lock);
    ino_id = next_new_ino_id;
    next_new_ino_id++;
    lock_release(&ino_lock);

    return ino_id; 
}

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
    if (vfs_initialized) return;
    vfs_initialized = true;

    /* Initialize the root folder */
    vfs_root.inode = vfs_alloc_inode(VFS_NODE_FOLDER, 0777, 0, NULL, NULL);
    vfs_root.st.st_dev = vfs_new_dev_id();
    vfs_root.st.st_ino = vfs_new_ino_id();

    /* Register all file systems which will be used */
    vfs_register_fs(&fat32);
    vfs_register_fs(&ramfs);
    vfs_register_fs(&ttyfs);

    char* fn = "/";

    /* Mount RAMFS without device name (NULL) */
    vfs_mount(NULL, fn, "ramfs");

    /* Call vsf_refrsh() to load all RAMFS files */
    vfs_handle_t f = vfs_open(fn, VFS_MODE_READWRITE);
    if (f != VFS_INVALID_HANDLE) {
        vfs_refresh(f);
        vfs_close(f);
    }

    /* Create directory for mounting devices in the future */
    vfs_path_to_node("/disk", CREATE, VFS_NODE_FOLDER);
    vfs_path_to_node("/dev", CREATE, VFS_NODE_FOLDER);

    /* Mount TTYFS with device name "/dev/tty" */
    vfs_path_to_node("/dev/tty", CREATE, VFS_NODE_FOLDER);
    vfs_mount("tty", "/dev/tty", "ttyfs");
    ttyfh = vfs_open("/dev/tty", VFS_MODE_READWRITE);

    klogi("VFS initialization finished\n");
}

/* Creates a node with specified type */
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

/* Changes permissions of node */
int64_t vfs_chmod(vfs_handle_t handle, int32_t newperms)
{
    vfs_node_desc_t* fd = vfs_handle_to_fd(handle);
    if (!fd)
        return -1;

    /* Opened in read only mode */
    if (fd->mode == VFS_MODE_READ) {
        kloge("Opened as read-only\n");
        return -1;
    }

    /* Set new permissions and sync */
    fd->inode->perms = newperms;
    fd->inode->fs->sync(fd->inode);
    return 0;
}

int64_t vfs_ioctl(vfs_handle_t handle, int64_t request, int64_t arg)
{
    vfs_node_desc_t* fd = vfs_handle_to_fd(handle);
    if (!fd)
        return -1; 

    if (fd->inode->fs->ioctl != NULL) {
        return fd->inode->fs->ioctl(fd->inode, request, arg);
    }

    return -1;
}

/* Mounts a block device with specified filesystem at a path */
int64_t vfs_mount(char* device, char* path, char* fsname)
{
    lock_lock(&vfs_lock);

    /* Get the fs info */
    vfs_fsinfo_t* fs = vfs_get_fs(fsname);
    if (!fs)
        goto fail;

    /* Get the block device if needed */
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

    /* Get the node where it is to be mounted (should be an empty folder) */
    vfs_tnode_t* at = vfs_path_to_node(path, NO_CREATE, 0);
    if (!at)
        goto fail;
    if (at->inode->type != VFS_NODE_FOLDER || at->inode->child.len != 0) {
        kloge("'%s' is not an empty folder\n", path);
        goto fail;
    }
    kmfree(at->inode);

    /* Mount the fs */
    at->inode = fs->mount(dev ? dev->inode : NULL);
    at->inode->mountpoint = at;

    klogi("Mounted %s at %s as %s\n", device ? device : "<no-device>", path, fsname);
    lock_release(&vfs_lock);
    return 0;
fail:
    lock_release(&vfs_lock);
    return -1;
}

/* Get the length of a file */
int64_t vfs_tell(vfs_handle_t handle)
{
    vfs_node_desc_t* fd = vfs_handle_to_fd(handle);

    if (!fd) { 
        return 0;
    } else {
        vfs_inode_t* inode = fd->inode;
        return inode->size;
    }
}

/* Read specified number of bytes from a file */
int64_t vfs_read(vfs_handle_t handle, size_t len, void* buff)
{
    vfs_node_desc_t* fd = vfs_handle_to_fd(handle);
    if (!fd) {
        return 0;
    }

    lock_lock(&vfs_lock);

    vfs_inode_t* inode = fd->inode;

    /*
     * 1. Truncate if asking for more data than available
     * 2. Return directly if remaining length is zero except tty device
     */
    if (fd->seek_pos + len > inode->size
        && handle != ttyfh)
    {
        len = inode->size - fd->seek_pos;
        if (len == 0)
            goto end;
    }

    int64_t status = fd->inode->fs->read(fd->inode, fd->seek_pos, len, buff);
    if (status == -1)
        len = 0;

    fd->seek_pos += len;
end:
    lock_release(&vfs_lock);
    return (int64_t)len;
}

/* Write specified number of bytes to file */
int64_t vfs_write(vfs_handle_t handle, size_t len, const void* buff)
{
    vfs_node_desc_t* fd = vfs_handle_to_fd(handle);
    if (!fd)
        return 0;

    /* Cannot write to read-only files */
    if (fd->mode == VFS_MODE_READ) {
        kloge("File handle %d is read only\n", handle);
        return 0;
    }

    lock_lock(&vfs_lock);
    vfs_inode_t* inode = fd->inode;

    /* Expand file if writing more data than its size */
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

/* Seek to specified position in file */
int64_t vfs_seek(vfs_handle_t handle, size_t pos, int64_t whence)
{
    vfs_node_desc_t* fd = vfs_handle_to_fd(handle);
    if (!fd)
        return -1;

    lock_lock(&vfs_lock);

    int64_t offset = -1;
    switch (whence) {
    case SEEK_SET: /* 3 */
        offset = pos;
        break;
    case SEEK_CUR: /* 1 */
        offset = fd->seek_pos + pos;
        break;
    case SEEK_END: /* 2 */
        offset = fd->inode->size - pos;
        break;
    }

    /* Seek position is out of bounds */
    if (offset > (int64_t)fd->inode->size || offset < 0)
    {
        klogw("Seek position out of bounds (%d:%d in len %d with offset %d)\n",
              pos, whence, fd->inode->size, fd->seek_pos);
        return -1; 
    }

    int64_t ret = -1;
    if (offset >= 0 && offset < (int64_t)fd->inode->size) {
        fd->seek_pos = offset;
        ret = 0;
    }

    lock_release(&vfs_lock);
    return ret;
}

int64_t vfs_get_parent_dir(const char *path, char *parent, char *currdir)
{
    if (path == NULL || parent == NULL) {
        return -1;
    }

    strcpy(parent, path);

    int64_t idx = strlen(parent) - 1;
    while (idx >= 0) {
        if (parent[idx] == '/') {
            parent[idx] = '\0';
            idx--;
        }
        if (parent[idx] != '/') break;
    }

    /* Do not have parent directory */
    if (idx <= 0) {
        parent[0] = '\0';
        return -1;
    }

    /* Have parent directory */
    while(idx >= 0) {
        if (parent[idx] == '/') {
            parent[idx] = '\0';
            break;
        }   
        idx--;
    }

    if (currdir != NULL && idx >= 0) {
        strcpy(currdir, &(parent[idx + 1]));
    }
    if (strlen(parent) == 0) strcpy(parent, "/");

    return 0;
}

vfs_handle_t vfs_open(char* path, vfs_openmode_t mode)
{
    lock_lock(&vfs_lock);

    /* Find the node */
    vfs_tnode_t* req = vfs_path_to_node(path, NO_CREATE, 0);
    if (!req) {
        vfs_tnode_t* pn = NULL;
        char curpath[VFS_MAX_PATH_LEN] = {0}, parent[VFS_MAX_PATH_LEN] = {0};
        strcpy(curpath, path);
        while (true) {
            vfs_get_parent_dir(curpath, parent, NULL);
            if (strcmp(curpath, parent) == 0) break;
            pn = vfs_path_to_node(parent, NO_CREATE, 0);
            if (pn) break;
            strcpy(curpath, parent);
        }
        if (pn != NULL && pn->inode->fs != NULL) {
            klogw("VFS: Can not open %s, visit back to %s\n", path, parent);
            req = pn->inode->fs->open(pn->inode, path);
        }
        if (!req) goto fail;
    } else {
        if (req->inode->fs != NULL) {
            klogw("VFS: inode for %s already exists\n", path);
            req = req->inode->fs->open(req->inode, path);
        }
    }

    req->inode->refcount++;

    /* Create node descriptor */
    vfs_node_desc_t* nd = (vfs_node_desc_t*)kmalloc(sizeof(vfs_node_desc_t));
    memset(nd, 0, sizeof(vfs_node_desc_t));

    strcpy(nd->path, path);
    nd->tnode = req;
    nd->inode = req->inode;
    nd->seek_pos = 0;
    nd->mode = mode;

    /* If this is a symlink, we should set the real file size */
    /* TODO: Need to consider in the future */
    nd->tnode->st.st_size = req->inode->size;

    /* Add to current task */
    /* TODO: Loop for NULL pointer position in open file list */
    vec_push_back(&vfs_openfiles, nd);

    /* Return the handle */
    lock_release(&vfs_lock);

    vfs_handle_t fh = ((vfs_handle_t)(vfs_openfiles.len - 1)) + VFS_MIN_HANDLE;
    klogv("VFS: Open %s and return handle %d\n", path, fh);
    return fh;
fail:
    lock_release(&vfs_lock);
    return VFS_INVALID_HANDLE;
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

int64_t vfs_refresh(vfs_handle_t handle)
{
    vfs_node_desc_t* fd = vfs_handle_to_fd(handle);
    if (!fd)
        return -1; 

    lock_lock(&vfs_lock);
    fd->inode->fs->refresh(fd->inode);
    for (size_t i = 0; ; i++) {
        vfs_dirent_t de;
        if (fd->inode->fs->getdent(fd->inode, i, &de)) break;

        char path[VFS_MAX_PATH_LEN] = {0};
        strcpy(path, fd->path);
        strcat(path, "/");
        strcat(path, de.name);
        vfs_tnode_t* tn = vfs_path_to_node(path, CREATE, de.type);
        memcpy(&tn->inode->tm, &de.tm, sizeof(tm_t));
        tn->inode->size = de.size;
    }
    lock_release(&vfs_lock);

    return 0;
}

/* Get next directory entry */
int64_t vfs_getdent(vfs_handle_t handle, vfs_dirent_t* dirent) {
    int64_t status;
    vfs_node_desc_t* fd = vfs_handle_to_fd(handle);
    if (!fd)
        return -1;

    lock_lock(&vfs_lock);

    /* Can only traverse folders */
    if (!IS_TRAVERSABLE(fd->inode)) {
        kloge("Node not traversable\n");
        status = -1;
        goto done;
    }

    /* Need to make sure that we alreay load all children here */

    /* We've reached the end */
    if (fd->seek_pos >= fd->inode->child.len) {
        status = 0;
        goto done;
    }

    /* Initialize the dirent */
    vfs_tnode_t* entry = vec_at(&(fd->inode->child), fd->seek_pos);
    dirent->type = entry->inode->type;
    memcpy(dirent->name, entry->name, sizeof(entry->name));
    memcpy(&dirent->tm, &entry->inode->tm, sizeof(tm_t));

    /* We're done here, advance the offset */
    status = 1;
    fd->seek_pos++;

done:
    lock_release(&vfs_lock);
    return status;
}

