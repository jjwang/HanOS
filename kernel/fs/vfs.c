/**-----------------------------------------------------------------------------

 @file    vfs.c
 @brief   Implementation of VFS related functions
 @details
 @verbatim

  VFS is an abstraction layer that provides a unified interface for various
  physical file systems. This allows users to access the file system through
  standard file operation functions without knowing the details of the
  underlying physical file system. 

  Like all Unix-like system, inode is the fundmental data structure of VFS which
  stores file index information. All children node pointers will be stored in
  inode. tnode is used to store tree information, e.g., parent node. node_desc
  data structure is used for every file operation, from fopen, fread to fclose. 


  History:
    Jan 1, 2026  The spinlock currently in use is inefficient and needs to be
                 optimized to improve its running speed on physical machines.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <string.h>
#include <protocol.h>
#include <fs/vfs.h>
#include <fs/filebase.h>
#include <lib/hash.h>
#include <lib/kmalloc.h>
#include <mm/memobj.h>
#include <mm/ipc_buf.h>
#include <mm/mm.h>
#include <ipc/object.h>
#include <srv/fat32_srv.h>
#include <srv/process_srv.h>
#include <router/router.h>
#include <proc/sched.h>

/* Maximum one VFS request reads/writes. */
#define VFS_SERVER_IO_MAX   4096

/* Internal sentinel: the pipe server would block; the caller retries. */
#define VFS_IO_AGAIN        (-2)
#include <fs/initrd.h>
#include <lib/klog.h>
#include <lib/klib.h>
#include <lib/kmalloc.h>
#include <lib/spinlock.h>
#include <lib/vector.h>
#include <lib/hash.h>

#include <arch/x64/atomic_ops.h>

static bool vfs_initialized = false;

/* VFS wide lock */
spinlock_t vfs_lock;

/* Available dev & ino new id */
static dev_t next_new_dev_id = 1;
static ino_t next_new_ino_id = 1;

/* Root node */
vfs_tnode_t vfs_root = { 0 };

/* List of installed filesystems */
vec_new_static(vfs_fsinfo_t *, vfs_fslist);

/* Stat structure related function implementations */
dev_t vfs_new_dev_id(void)
{
    return atomic_inc64(&next_new_dev_id);
}

ino_t vfs_new_ino_id(void)
{
    return atomic_inc64(&next_new_ino_id);
}

static void dumpnodes_helper(vfs_tnode_t * from, int lvl)
{
    for (int i = 0; i < 1 + lvl; i++)
        kprintf(" ");
    kprintf(" %ld: [%s] -> %016lx inode (%ld refs)\n",
            lvl, from->name, from->inode, from->inode->refcount);

    if (IS_TRAVERSABLE(from->inode))
        for (uint64_t i = 0; i < from->inode->child.len; i++)
            dumpnodes_helper(vec_at(&(from->inode->child), i), lvl + 1);
}

void vfs_debug()
{
    kprintf("Dumping VFS nodes:\n");
    dumpnodes_helper(&vfs_root, 0);
    kprintf("Dumping done.\n");
}

void vfs_register_fs(vfs_fsinfo_t * fs)
{
    vec_push_back(&vfs_fslist, fs);
}

vfs_fsinfo_t *vfs_get_fs(char *name)
{
    for (uint64_t i = 0; i < vfs_fslist.len; i++) {
        if (strncmp(name, vfs_fslist.data[i]->name, sizeof(((vfs_fsinfo_t) {
                                                            0}
                                                           ).name)) == 0) {
            return vfs_fslist.data[i];
        }
    }

    kloge("Filesystem %s not found\n", name);
    return NULL;
}

void vfs_init()
{
    if (vfs_initialized)
        return;
    vfs_initialized = true;

    /* Only the root inode stays; paths are served from user space. The initrd
     * is read directly by the ELF loader until the VFS server registers. */
    vfs_root.inode = vfs_alloc_inode(VFS_NODE_FOLDER, 0777, 0, NULL, NULL);
    vfs_root.st.st_dev = vfs_new_dev_id();
    vfs_root.st.st_ino = vfs_new_ino_id();
    vfs_root.st.st_mode |= S_IFDIR;
    vfs_root.st.st_nlink = 1;

    klogi("VFS initialization finished\n");
}

/* Creates a node with specified type */
int64_t vfs_create(char *path, vfs_node_type_t type)
{
    int64_t status = 0;
    spinlock_acquire(&vfs_lock);

    vfs_tnode_t *tnode = vfs_path_to_node(path, CREATE, type);
    if (tnode == NULL) {
        status = -1;
    } else {
        /* Set the file time */
        uint64_t now_sec = hpet_get_nanos() / 1000000000;
        time_t boot_time = cmos_boot_time();

        time_t file_time = now_sec + boot_time;

        tnode->st.st_atim.tv_sec = file_time;
        tnode->st.st_mtim.tv_sec = file_time;
        tnode->st.st_ctim.tv_sec = file_time;

        tnode->st.st_atim.tv_nsec = 0;
        tnode->st.st_mtim.tv_nsec = 0;
        tnode->st.st_ctim.tv_nsec = 0;
    }

    spinlock_release(&vfs_lock);
    return status;
}

/* Changes permissions of node */
int64_t vfs_chmod(vfs_handle_t handle, int32_t newperms)
{
    vfs_node_desc_t *fd = vfs_handle_to_fd(handle, __func__);
    if (!fd)
        return -1;

    /* Opened in read only mode */
    if (fd->mode == VFS_MODE_READ) {
        kloge("Opened as read-only\n");
        return -1;
    }

    /* Set new permissions and sync */
    fd->inode->perms = newperms & (S_IRWXU | S_IRWXG | S_IRWXO);
    fd->tnode->st.st_mode |= fd->inode->perms;
    fd->inode->fs->sync(fd->inode);
    return 0;
}

int64_t vfs_ioctl(vfs_handle_t handle, int64_t request, int64_t arg)
{
    vfs_node_desc_t *fd = vfs_handle_to_fd(handle, __func__);
    if (!fd)
        return -1;

    if (fd->inode->fs->ioctl != NULL) {
        return fd->inode->fs->ioctl(fd->inode, request, arg);
    }

    return -1;
}

/* Mounts a block device with specified filesystem at a path */
int64_t vfs_mount(char *device, char *path, char *fsname)
{
    spinlock_acquire(&vfs_lock);

    /* Get the fs info */
    vfs_fsinfo_t *fs = vfs_get_fs(fsname);
    if (!fs)
        goto fail;

    /* Get the block device if needed */
    vfs_tnode_t *dev = NULL;
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
    vfs_tnode_t *at = vfs_path_to_node(path, NO_CREATE, 0);
    if (!at)
        goto fail;
    if (at->inode->type != VFS_NODE_FOLDER || at->inode->child.len != 0) {
        kloge("\"%s\" is not an empty folder\n", path);
        goto fail;
    }
    kmfree(at->inode);

    /* Mount the fs */
    at->inode = fs->mount(dev ? dev->inode : NULL);
    at->inode->mountpoint = at;

    spinlock_release(&vfs_lock);

    klogi("Mounted %s at %s as %s\n", device ? device : "<no-device>",
          path, fsname);
    return 0;

  fail:
    spinlock_release(&vfs_lock);
    return -1;
}

/* Get the length of a file */
uint64_t vfs_tell(vfs_handle_t handle)
{
    vfs_node_desc_t *fd = vfs_handle_to_fd(handle, __func__);

    if (!fd) {
        kloge("VFS: cannot get fd for file %ld\n", handle);
        return 0;
    } else if (fd->server) {
        return fd->server_size;
    } else {
        vfs_inode_t *inode = fd->inode;
        return inode->size;
    }
}

/* Move a memory object's contents to/from a kernel buffer. */
static void memobj_copy_out(memobj_t * mo, void *dst, uint64_t len,
                            uint64_t off)
{
    uint64_t done = 0;

    while (done < len) {
        uint64_t pos = off + done;
        uint64_t chunk = PAGE_SIZE - (pos & (PAGE_SIZE - 1));

        if (chunk > len - done)
            chunk = len - done;
        memcpy((uint8_t *) dst + done,
               (uint8_t *) PHYS_TO_VIRT(memobj_page(mo, pos / PAGE_SIZE))
               + (pos & (PAGE_SIZE - 1)), chunk);
        done += chunk;
    }
}

static void memobj_copy_in(memobj_t * mo, const void *src, uint64_t len,
                           uint64_t off)
{
    uint64_t done = 0;

    while (done < len) {
        uint64_t pos = off + done;
        uint64_t chunk = PAGE_SIZE - (pos & (PAGE_SIZE - 1));

        if (chunk > len - done)
            chunk = len - done;
        memcpy((uint8_t *) PHYS_TO_VIRT(memobj_page(mo, pos / PAGE_SIZE))
               + (pos & (PAGE_SIZE - 1)), (const uint8_t *) src + done,
               chunk);
        done += chunk;
    }
}

/* Allocate a memory object of len bytes with a transferable handle in the
 * current process. The caller keeps the creation reference for its own use. */
static memobj_t *server_memobj(uint64_t len, handle_t * out)
{
    process_t *t = sched_get_current_process();

    if (t == NULL || len == 0)
        return NULL;

    memobj_t *mo = memobj_create(len);
    if (mo == NULL)
        return NULL;

    handle_t h = handle_alloc(&t->handles, memobj_object(mo),
                              HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE
                              | HANDLE_RIGHT_MAP | HANDLE_RIGHT_TRANSFER);
    if (h == HANDLE_INVALID) {
        memobj_unref(mo);
        return NULL;
    }

    *out = h;
    return mo;                  /* creation ref kept by the caller */
}

static int64_t vfs_server_read(int64_t sfd, uint64_t len, void *buff)
{
    if (len > VFS_SERVER_IO_MAX)
        len = VFS_SERVER_IO_MAX;

    handle_t mh;
    memobj_t *mo = server_memobj(len, &mh);
    if (mo == NULL)
        return -1;

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = VFS_READ;
    req.words[0] = (uint64_t) sfd;
    req.words[1] = len;
    req.xfer[0] = mh;
    req.xfer_count = 1;

    if (!router_forward(SVC_FS, &req, &rep) || (int64_t) rep.words[0] < 0) {
        memobj_unref(mo);
        return -1;
    }

    uint64_t n = rep.words[1];
    if (n > len)
        n = len;
    memobj_copy_out(mo, buff, n, 0);
    memobj_unref(mo);
    return (int64_t) n;
}

static int64_t vfs_server_write(int64_t sfd, uint64_t len, const void *buff)
{
    if (len > VFS_SERVER_IO_MAX)
        len = VFS_SERVER_IO_MAX;

    handle_t mh;
    memobj_t *mo = server_memobj(len, &mh);
    if (mo == NULL)
        return -1;

    memobj_copy_in(mo, buff, len, 0);

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = VFS_WRITE;
    req.words[0] = (uint64_t) sfd;
    req.words[1] = len;
    req.xfer[0] = mh;
    req.xfer_count = 1;

    if (!router_forward(SVC_FS, &req, &rep) || (int64_t) rep.words[0] < 0) {
        memobj_unref(mo);
        return -1;
    }

    memobj_unref(mo);
    return (int64_t) rep.words[1];
}

static int64_t vfs_server_close(int svc, int64_t sfd)
{
    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = (svc == SVC_PIPE) ? PIPE_CLOSE : VFS_CLOSE;
    req.words[0] = (uint64_t) sfd;

    if (!router_forward(svc, &req, &rep))
        return -1;
    return (int64_t) rep.words[0];
}

/* One bounded read/write against the pipe server. Returns the byte count, 0 at
 * EOF (read only), VFS_IO_AGAIN when it would block, or -1 on error. Small
 * transfers travel inline in the message words to avoid a memory object (and a
 * page allocation) per byte. */
static int64_t vfs_pipe_xfer(int64_t sfd, uint64_t tag, uint64_t len,
                             void *buff)
{
    if (len > VFS_SERVER_IO_MAX)
        len = VFS_SERVER_IO_MAX;

    bool inline_data = (len <= PIPE_INLINE_MAX);
    handle_t mh = 0;
    memobj_t *mo = NULL;

    if (!inline_data) {
        mo = server_memobj(len ? len : 1, &mh);
        if (mo == NULL)
            return -1;
        if (tag == PIPE_WRITE)
            memobj_copy_in(mo, buff, len, 0);
    }

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = tag;
    req.words[0] = (uint64_t) sfd;
    req.words[1] = len;

    if (inline_data) {
        if (tag == PIPE_WRITE && len > 0)
            memcpy(&req.words[2], buff, len);
    } else {
        req.xfer[0] = mh;
        req.xfer_count = 1;
    }

    if (!router_forward(SVC_PIPE, &req, &rep)) {
        if (mo != NULL)
            memobj_unref(mo);
        return -1;
    }

    if ((int64_t) rep.words[0] < 0) {
        int64_t rc = (int64_t) rep.words[0];

        if (mo != NULL)
            memobj_unref(mo);
        return (rc == PIPE_EAGAIN) ? VFS_IO_AGAIN : -1;
    }

    int64_t n = (int64_t) rep.words[1];

    if (tag == PIPE_READ && n > 0) {
        if (inline_data)
            memcpy(buff, &rep.words[2], n);
        else
            memobj_copy_out(mo, buff, n, 0);
    }

    if (mo != NULL)
        memobj_unref(mo);
    return n;
}

/* Blocking pipe read/write. The pipe server holds a request that cannot make
 * progress and answers it once the other end moves data or closes, so a single
 * call blocks until the operation completes. */
static int64_t vfs_pipe_rw(int64_t sfd, uint64_t tag, uint64_t len, void *buff)
{
    return vfs_pipe_xfer(sfd, tag, len, buff);
}

/* Ask the server for the stat of a path. The path and the result share one
 * buffer memory object: the path at offset 0, the stat at VFS_IO_DATA_OFF.
 * out must be a kernel buffer of at least sizeof(vfs_stat_t). */
int64_t vfs_server_stat_path(const char *path, void *out)
{
    handle_t mh;
    memobj_t *mo = server_memobj(VFS_IO_BUF_SIZE, &mh);

    if (mo == NULL)
        return -1;

    uint64_t plen = strlen(path) + 1;
    if (plen > VFS_IO_DATA_OFF)
        plen = VFS_IO_DATA_OFF;
    memobj_copy_in(mo, path, plen, 0);

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = VFS_FSTATAT;
    req.xfer[0] = mh;
    req.xfer_count = 1;

    if (!router_forward(SVC_FS, &req, &rep) || (int64_t) rep.words[0] < 0) {
        memobj_unref(mo);
        return -1;
    }

    memobj_copy_out(mo, out, sizeof(vfs_stat_t), VFS_IO_DATA_OFF);
    memobj_unref(mo);
    return 0;
}

/* Stat an open server fd. out must be a kernel buffer of at least
 * sizeof(vfs_stat_t). */
int64_t vfs_server_fstat(int64_t sfd, void *out)
{
    handle_t mh;
    memobj_t *mo = server_memobj(VFS_IO_BUF_SIZE, &mh);

    if (mo == NULL)
        return -1;

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = VFS_FSTAT;
    req.words[0] = (uint64_t) sfd;
    req.xfer[0] = mh;
    req.xfer_count = 1;

    if (!router_forward(SVC_FS, &req, &rep) || (int64_t) rep.words[0] < 0) {
        memobj_unref(mo);
        return -1;
    }

    memobj_copy_out(mo, out, sizeof(vfs_stat_t), VFS_IO_DATA_OFF);
    memobj_unref(mo);
    return 0;
}

/* Read the next directory entry of a server-backed directory fd. out must be a
 * kernel buffer of at least sizeof(dirent_t). */
int64_t vfs_server_readdir(vfs_handle_t handle, void *out)
{
    vfs_node_desc_t *fd = vfs_handle_to_fd(handle, __func__);

    if (fd == NULL || !fd->server)
        return -1;

    handle_t mh;
    memobj_t *mo = server_memobj(VFS_IO_BUF_SIZE, &mh);

    if (mo == NULL)
        return -1;

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = VFS_READDIR;
    req.words[0] = (uint64_t) fd->server_fd;
    req.xfer[0] = mh;
    req.xfer_count = 1;

    if (!router_forward(SVC_FS, &req, &rep) || (int64_t) rep.words[0] < 0) {
        memobj_unref(mo);
        return -1;
    }

    memobj_copy_out(mo, out, sizeof(dirent_t), VFS_IO_DATA_OFF);
    memobj_unref(mo);
    return 0;
}

/* Move a server-backed file descriptor's read offset. */
static int64_t vfs_server_seek(int64_t sfd, uint64_t pos, int64_t whence)
{
    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = VFS_SEEK;
    req.words[0] = (uint64_t) sfd;
    req.words[1] = pos;
    req.words[2] = (uint64_t) whence;

    if (!router_forward(SVC_FS, &req, &rep) || (int64_t) rep.words[0] < 0)
        return -1;
    return (int64_t) rep.words[1];
}

/* Ask the server to remove a path. */
int64_t vfs_server_unlink(const char *path)
{
    handle_t mh;
    memobj_t *mo = server_memobj(VFS_IO_BUF_SIZE, &mh);

    if (mo == NULL)
        return -1;

    uint64_t plen = strlen(path) + 1;
    if (plen > VFS_IO_DATA_OFF)
        plen = VFS_IO_DATA_OFF;
    memobj_copy_in(mo, path, plen, 0);

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = VFS_UNLINK;
    req.xfer[0] = mh;
    req.xfer_count = 1;

    if (!router_forward(SVC_FS, &req, &rep) || (int64_t) rep.words[0] < 0) {
        memobj_unref(mo);
        return -1;
    }

    memobj_unref(mo);
    return 0;
}

/* Add a reference to a server fd's open file description (used by fork). */
void vfs_server_ref_fd(int svc, int64_t sfd)
{
    if (router_lookup(svc) == NULL)
        return;

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = VFS_FD_FORK;
    req.words[0] = (uint64_t) sfd;
    router_forward(svc, &req, &rep);
}

int64_t vfs_read(vfs_handle_t handle, uint64_t len, void *buff)
{
    vfs_node_desc_t *fd = vfs_handle_to_fd(handle, __func__);
    if (!fd) {
        return 0;
    }

    if (fd->server) {
        if (fd->svc == SVC_PIPE)
            return vfs_pipe_rw(fd->server_fd, PIPE_READ, len, buff);
        if (fd->svc == SVC_FAT) {
            /* The FAT server keeps its own offset. */
            return fat32_read_fd(fd->server_fd, len, buff);
        }

        /* The server moves at most VFS_SERVER_IO_MAX per request; loop so a
         * caller asking for the whole file (e.g. the ELF loader) gets it. */
        uint64_t done = 0;

        while (done < len) {
            int64_t n = vfs_server_read(fd->server_fd, len - done,
                                        (uint8_t *) buff + done);

            if (n <= 0)
                break;
            done += (uint64_t) n;
        }
        return (int64_t) done;
    }

    spinlock_acquire(&vfs_lock);

    vfs_inode_t *inode = fd->inode;

    /* Truncate if asking for more data than available. */
    if (fd->seek_pos + len > inode->size) {
        len = inode->size - fd->seek_pos;
        if (len == 0)
            goto end;
    }

    int64_t ret = fd->inode->fs->read(fd->inode, fd->seek_pos, len, buff);
    if (ret < 0)                /* Error occurs */
        len = 0;
    else                        /* Actual reading length */
        len = ret;

    fd->seek_pos += len;
  end:
    spinlock_release(&vfs_lock);
    return (int64_t) len;
}

/* Unlink the file, reduce link count (nlink in st of tnode). If the reference
 * count is equal to zero, we will delete this file.
 */
int64_t vfs_unlink(char *path)
{
    klogd("VFS: unlink %s\n", path);

    spinlock_acquire(&vfs_lock);

    /* Find the node and set st_nlink parameter */
    vfs_tnode_t *req = vfs_path_to_node(path, NO_CREATE, 0);
    if (!req) {
        klogd("VFS: Cannot find tnode for %s\n", path);
        goto fail;
    } else {
        if (req->st.st_nlink > 1) {
            klogd
                ("VFS: \"%s\" has links which should be removed firstly\n",
                 path);
            goto fail;
        } else if (req->st.st_nlink == 0) {
            klogd("VFS: \"%s\" should have one link by itself\n", path);
            goto fail;
        }
        req->st.st_nlink = 0;
    }

    /* Remove this file if needed */
    if (req->inode->refcount == 0) {
        if (req->inode->fs->rmnode != NULL) {
            req->inode->fs->rmnode(req);
        }
    }

    spinlock_release(&vfs_lock);
    return 0;

  fail:
    spinlock_release(&vfs_lock);
    return -1;
}

/* Write specified number of bytes to file */
int64_t vfs_write(vfs_handle_t handle, uint64_t len, const void *buff)
{
    vfs_node_desc_t *fd = vfs_handle_to_fd(handle, __func__);
    if (!fd)
        return 0;

    if (fd->server) {
        if (fd->svc == SVC_PIPE)
            return vfs_pipe_rw(fd->server_fd, PIPE_WRITE, len, (void *) buff);
        return vfs_server_write(fd->server_fd, len, buff);
    }

    /* Cannot write to read-only files */
    if (fd->mode == VFS_MODE_READ) {
        kloge("File handle %ld is read only, nd = 0x%016lx\n", handle, fd);
        return 0;
    }

    spinlock_acquire(&vfs_lock);
    vfs_inode_t *inode = fd->inode;

    /* Expand file if writing more data than its size */
    if (fd->seek_pos + len > inode->size) {
        inode->size = fd->seek_pos + len;
        if (inode->fs->sync != NULL)
            inode->fs->sync(inode);
    }

    int64_t status = inode->fs->write(inode, fd->seek_pos, len, buff);
    if (status == -1) {
        len = 0;
    } else {
        /* Move seek position to the tail of writing area */
        fd->seek_pos += len;
    }

    /* Set file size to stat data structure */
    fd->tnode->st.st_size = fd->inode->size;
    fd->tnode->st.st_blocks =
        DIV_ROUNDUP(fd->tnode->st.st_size, VFS_BLOCK_SIZE);

    spinlock_release(&vfs_lock);
    return (int64_t) len;
}

/* Seek to specified position in file */
int64_t vfs_seek(vfs_handle_t handle, uint64_t pos, int64_t whence)
{
    vfs_node_desc_t *fd = vfs_handle_to_fd(handle, __func__);
    if (!fd)
        return -1;

    if (fd->server) {
        if (fd->svc == SVC_PIPE)
            return -1;          /* pipes are not seekable */
        if (fd->svc == SVC_FAT)
            return fat32_seek_fd(fd->server_fd, pos, whence);
        return vfs_server_seek(fd->server_fd, pos, whence);
    }

    spinlock_acquire(&vfs_lock);

    int64_t offset = -1;
    switch (whence) {
    case SEEK_SET:             /* 3 */
        offset = pos;
        break;
    case SEEK_CUR:             /* 1 */
        offset = fd->seek_pos + pos;
        break;
    case SEEK_END:             /* 2 */
        offset = fd->inode->size - pos;
        break;
    }

    /* For writing mode, it can enlarge file size */
    if ((fd->mode == VFS_MODE_WRITE || fd->mode == VFS_MODE_READWRITE)
        && offset > (int64_t) fd->inode->size) {
        fd->inode->size = offset;
        if (fd->inode->fs->sync != NULL)
            fd->inode->fs->sync(fd->inode);
    }

    /* Seek position is out of bounds */
    if (offset > (int64_t) fd->inode->size || offset < 0) {
        klogd("Seek position out of bounds: %ld(0x%016lx):%ld in len %ld with "
              "offset %ld\n",
              pos, pos, whence, fd->inode->size, fd->seek_pos);
        spinlock_release(&vfs_lock);
        return -1;
    }

    int64_t ret = -1;
    if (offset >= 0 && offset <= (int64_t) fd->inode->size) {
        fd->seek_pos = offset;
        ret = offset;
    }

    spinlock_release(&vfs_lock);
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
        if (parent[idx] != '/')
            break;
    }

    /* Do not have parent directory */
    if (idx <= 0) {
        parent[0] = '\0';
        return -1;
    }

    /* Have parent directory */
    while (idx >= 0) {
        if (parent[idx] == '/') {
            parent[idx] = '\0';
            break;
        }
        idx--;
    }

    if (currdir != NULL && idx >= 0) {
        strcpy(currdir, &(parent[idx + 1]));
    }
    if (strlen(parent) == 0)
        strcpy(parent, "/");

    return 0;
}

vfs_handle_t vfs_open(char *path, vfs_openmode_t mode)
{
    (void) mode;
    /* File descriptors live in the process server now; the only kernel file
     * access (the ELF loader) uses vfs_load_file(). */
    klogd("VFS: in-kernel open \"%s\" is no longer available\n", path);
    return VFS_INVALID_HANDLE;
}

vfs_handle_t vfs_open_server_svc(int64_t server_fd, const char *path,
                                 vfs_openmode_t mode, uint64_t size, int svc)
{
    (void) path;

    /* The process server owns the fd table; it allocates the fd. */
    int64_t fd = process_fd_open(svc, server_fd, size, (int64_t) mode);

    return (fd < 0) ? VFS_INVALID_HANDLE : (vfs_handle_t) fd;
}

vfs_handle_t vfs_open_server(int64_t server_fd, const char *path,
                             vfs_openmode_t mode, uint64_t size)
{
    return vfs_open_server_svc(server_fd, path, mode, size, SVC_FS);
}

/* Open a path through the userspace server when it is registered. The server
 * returns the size in words[2] so vfs_tell() works without a stat. */
static vfs_handle_t vfs_open_via_server(const char *path, vfs_openmode_t mode)
{
    handle_t ph;

    if (ipc_buf_from_kernel(path, strlen(path) + 1, &ph) != 0)
        return VFS_INVALID_HANDLE;

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = VFS_OPENAT;
    req.words[0] = (uint64_t) ((mode == VFS_MODE_READ) ? 2 : 1);
    req.xfer[0] = ph;
    req.xfer_count = 1;

    if (!router_forward(SVC_FS, &req, &rep) || (int64_t) rep.words[0] < 0)
        return VFS_INVALID_HANDLE;

    return vfs_open_server((int64_t) rep.words[1], path, mode, rep.words[2]);
}

vfs_handle_t vfs_open_routed(const char *path, vfs_openmode_t mode)
{
    if (router_lookup(SVC_FS) != NULL)
        return vfs_open_via_server(path, mode);

    return vfs_open((char *) path, mode);
}

static int64_t vfs_load_via_server(const char *path, uint8_t **out_buf,
                                   uint64_t *out_len)
{
    handle_t ph;

    if (ipc_buf_from_kernel(path, strlen(path) + 1, &ph) != 0)
        return -1;

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = VFS_OPENAT;
    req.words[0] = 2;           /* read */
    req.xfer[0] = ph;
    req.xfer_count = 1;

    if (!router_forward(SVC_FS, &req, &rep) || (int64_t) rep.words[0] < 0)
        return -1;

    int64_t sfd = (int64_t) rep.words[1];
    uint64_t size = rep.words[2];
    uint8_t *buf = NULL;

    if (size > 0) {
        buf = (uint8_t *) kmalloc_chunk(size, __func__, __LINE__);
        if (buf == NULL) {
            vfs_server_close(SVC_FS, sfd);
            return -1;
        }
    }

    uint64_t done = 0;

    while (done < size) {
        int64_t n = vfs_server_read(sfd, size - done, buf + done);

        if (n <= 0)
            break;
        done += (uint64_t) n;
    }
    vfs_server_close(SVC_FS, sfd);

    if (done != size) {
        if (buf != NULL)
            kmfree(buf);
        return -1;
    }

    *out_buf = buf;
    *out_len = size;
    return 0;
}

int64_t vfs_load_file(const char *path, uint8_t **out_buf, uint64_t *out_len)
{
    *out_buf = NULL;
    *out_len = 0;

    if (router_lookup(SVC_FS) != NULL)
        return vfs_load_via_server(path, out_buf, out_len);

    /* Early boot: read the file straight from the initrd image. */
    return initrd_load(path, out_buf, out_len);
}

int64_t vfs_close(vfs_handle_t handle)
{
    int kind = 0, svc = 0;
    int64_t sfd = 0;

    /* The process server removes the fd and returns where it lived so the
     * owning server can drop its open file description. */
    if (process_fd_close((int) handle, &kind, &svc, &sfd) != 0)
        return -1;

    if (svc == SVC_FAT)
        return fat32_close_fd(sfd);

    return vfs_server_close(svc, sfd);
}

int64_t vfs_refresh(vfs_handle_t handle)
{
    vfs_node_desc_t *fd = vfs_handle_to_fd(handle, __func__);
    if (!fd)
        return -1;

    spinlock_acquire(&vfs_lock);
    fd->inode->fs->refresh(fd->inode);
    for (uint64_t i = 0;; i++) {
        vfs_dirent_t de;
        if (fd->inode->fs->getdent(fd->inode, i, &de))
            break;

        char path[VFS_MAX_PATH_LEN] = { 0 };
        strcpy(path, fd->path);
        strncat(path, "/", sizeof(path));
        strncat(path, de.name, sizeof(path));
        vfs_tnode_t *tn = vfs_path_to_node(path, CREATE, de.type);
        memcpy(&tn->inode->tm, &de.tm, sizeof(tm_t));
        tn->inode->size = de.size;
    }
    spinlock_release(&vfs_lock);

    return 0;
}

/* Get next directory entry */
int64_t vfs_getdent(vfs_handle_t handle, vfs_dirent_t * dirent)
{
    int64_t status;
    vfs_node_desc_t *fd = vfs_handle_to_fd(handle, __func__);
    if (!fd)
        return -1;

    spinlock_acquire(&vfs_lock);

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
    vfs_tnode_t *entry = vec_at(&(fd->inode->child), fd->seek_pos);
    dirent->type = entry->inode->type;
    memcpy(dirent->name, entry->name, sizeof(entry->name));
    memcpy(&dirent->tm, &entry->inode->tm, sizeof(tm_t));

    /* We're done here, advance the offset */
    status = 1;
    fd->seek_pos++;

  done:
    spinlock_release(&vfs_lock);
    return status;
}
