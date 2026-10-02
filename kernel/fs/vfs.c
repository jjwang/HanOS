/**-----------------------------------------------------------------------------

 @file    vfs.c
 @brief   Forward file operations to the userspace servers

 @details
 @verbatim

   The kernel keeps no filesystem. It resolves a handle to the server-side
   descriptor through the process server, then forwards reads, writes, seeks,
   stats, directory scans and unlinks to the VFS, FAT and pipe servers. While
   the VFS server is still coming up, the ELF loader reads the boot initrd
   image directly.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <string.h>
#include <protocol.h>
#include <fs/vfs.h>
#include <fs/filebase.h>
#include <fs/initrd.h>
#include <lib/kmalloc.h>
#include <lib/klog.h>
#include <lib/spinlock.h>
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
    if (!fd || !fd->server)
        return 0;

    if (fd->svc == SVC_PIPE)
        return vfs_pipe_rw(fd->server_fd, PIPE_READ, len, buff);
    if (fd->svc == SVC_FAT) {
        /* The FAT server keeps its own offset. */
        return fat32_read_fd(fd->server_fd, len, buff);
    }

    /* The server moves at most VFS_SERVER_IO_MAX per request; loop so a caller
     * asking for the whole file (e.g. the ELF loader) gets it. */
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

int64_t vfs_write(vfs_handle_t handle, uint64_t len, const void *buff)
{
    vfs_node_desc_t *fd = vfs_handle_to_fd(handle, __func__);
    if (!fd || !fd->server)
        return 0;

    if (fd->svc == SVC_PIPE)
        return vfs_pipe_rw(fd->server_fd, PIPE_WRITE, len, (void *) buff);
    return vfs_server_write(fd->server_fd, len, buff);
}

int64_t vfs_seek(vfs_handle_t handle, uint64_t pos, int64_t whence)
{
    vfs_node_desc_t *fd = vfs_handle_to_fd(handle, __func__);
    if (!fd || !fd->server)
        return -1;

    if (fd->svc == SVC_PIPE)
        return -1;              /* pipes are not seekable */
    if (fd->svc == SVC_FAT)
        return fat32_seek_fd(fd->server_fd, pos, whence);
    return vfs_server_seek(fd->server_fd, pos, whence);
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

uint64_t vfs_tell(vfs_handle_t handle)
{
    vfs_node_desc_t *fd = vfs_handle_to_fd(handle, __func__);

    if (!fd) {
        kloge("VFS: cannot get fd for file %ld\n", handle);
        return 0;
    }
    return fd->server_size;
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
