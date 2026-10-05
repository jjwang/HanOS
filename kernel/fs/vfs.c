/**-----------------------------------------------------------------------------

 @file    vfs.c
 @brief   Forward file operations to the userspace servers

 @details
 @verbatim

   The kernel keeps no filesystem. It resolves a fd to the server-side
   descriptor through the process server, then forwards reads, writes, seeks,
   stats, directory scans and unlinks to the VFS, FAT and pipe servers. While
   the VFS server is still coming up, the ELF loader reads the boot initrd
   image directly.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>
#include <errno.h>
#include <protocol.h>
#include <fs/vfs.h>
#include <fs/initrd.h>
#include <lib/kmalloc.h>
#include <lib/klog.h>
#include <lib/spinlock.h>
#include <mm/memobj.h>
#include <mm/ipc_buf.h>
#include <mm/mm.h>
#include <ipc/object.h>
#include <srv/fat32_srv.h>
#include <srv/ext2_srv.h>
#include <srv/process_srv.h>
#include <srv/tty_srv.h>
#include <router/router.h>
#include <proc/sched.h>
#include <arch/x64/smp.h>

/* Maximum one VFS request reads/writes. A large file reaches the caller in a
 * few requests instead of one per page. */
#define VFS_SERVER_IO_MAX   (4 * 1024 * 1024)

/* The pipe buffer is 4 KiB, so a larger transfer only wastes a mapping. */
#define VFS_PIPE_IO_MAX     4096

/* Internal sentinel: the pipe server would block; the caller retries. */
#define VFS_IO_AGAIN        (-2)

/* Per-CPU scratch descriptor. The fd table lives in the process server, so a
 * fd is resolved there and copied here just for the current operation. */
static vfs_node_desc_t transient_fd[CPU_MAX];

/* Return the node descriptor for a fd */
vfs_node_desc_t *vfs_fd_to_desc(vfs_fd_t fd, const char *func)
{
    process_t *t = sched_get_current_process();
    if (t == NULL)
        return NULL;

    int32_t kind = 0, svc = 0;
    int64_t sfd = 0;
    uint64_t size = 0, seek = 0;

    if (process_fd_get((int32_t) fd, &kind, &svc, &sfd, &size, &seek) != 0) {
        klogw
            ("VFS: %s() cannot locate %ld (0x%016lx) in file list of process %ld\n",
             func, (int64_t) fd, (int64_t) fd, (int64_t) t->pid);
        return NULL;
    }

    vfs_node_desc_t *desc = &transient_fd[smp_get_current_cpu_id()];

    memset(desc, 0, sizeof(*desc));
    desc->server = true;
    desc->svc = svc;
    desc->server_fd = sfd;
    desc->server_size = size;
    desc->seek_pos = seek;
    return desc;
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

/* Allocate a memory object of len bytes with a transferable fd in the
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

static int64_t vfs_server_close(int32_t svc, int64_t sfd)
{
    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    if (svc == SVC_PIPE)
        req.tag = PIPE_CLOSE;
    else if (svc == SVC_NET)
        req.tag = NET_CLOSE;
    else
        req.tag = VFS_CLOSE;
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
    if (len > VFS_PIPE_IO_MAX)
        len = VFS_PIPE_IO_MAX;

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

    /* A read or write blocks until the other end makes progress, so wait for
     * the deferred reply without a timeout. */
    if (!router_forward_timeout(SVC_PIPE, &req, &rep, -1)) {
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
 * progress and answers it once the other end moves data or closes. When its
 * deferred table is full it answers EAGAIN, so retry until a slot frees. */
static int64_t vfs_pipe_rw(int64_t sfd, uint64_t tag, uint64_t len, void *buff)
{
    for (;;) {
        int64_t r = vfs_pipe_xfer(sfd, tag, len, buff);

        if (r != VFS_IO_AGAIN)
            return r;
        sched_sleep(1);
    }
}

/* Pack "cwd\0path\0" at the start of the request buffer. */
static void memobj_pack_cwd_path(memobj_t * mo, const char *cwd,
                                 const char *path)
{
    uint64_t clen = strlen(cwd) + 1;
    uint64_t plen = strlen(path) + 1;

    if (clen > VFS_IO_DATA_OFF)
        clen = VFS_IO_DATA_OFF;
    if (clen + plen > VFS_IO_DATA_OFF)
        plen = VFS_IO_DATA_OFF - clen;

    memobj_copy_in(mo, cwd, clen, 0);
    if (plen > 1)
        memobj_copy_in(mo, path, plen, clen);
}

/* Read the redirect path into a buffer sized to the string. The caller frees
 * it with kmfree(). */
static char *memobj_read_path_alloc(memobj_t * mo)
{
    char *tmp = kmalloc(VFS_IO_DATA_OFF);
    char *out;
    uint64_t n;

    if (tmp == NULL)
        return NULL;
    memobj_copy_out(mo, tmp, VFS_IO_DATA_OFF - 1, 0);
    tmp[VFS_IO_DATA_OFF - 1] = '\0';

    n = strlen(tmp);
    out = kmalloc(n + 1);
    if (out != NULL)
        memcpy(out, tmp, n + 1);
    kmfree(tmp);
    return out;
}

/* Stat a path. The VFS server resolves cwd+path; when the path is under the FAT
 * mount it redirects and the kernel asks the FAT server. */
int64_t vfs_stat_path(const char *cwd, const char *path, vfs_stat_t * out)
{
    handle_t mh;
    memobj_t *mo = server_memobj(VFS_IO_BUF_SIZE, &mh);

    if (mo == NULL)
        return -1;

    memobj_pack_cwd_path(mo, cwd, path);

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = VFS_FSTATAT;
    req.xfer[0] = mh;
    req.xfer_count = 1;

    if (!router_forward(SVC_FS, &req, &rep)) {
        memobj_unref(mo);
        return -1;
    }

    int64_t rc = (int64_t) rep.words[0];

    if (rc == VFS_REDIRECT_FAT) {
        char *rel = memobj_read_path_alloc(mo);
        uint64_t size = 0;
        bool is_dir = false;

        memobj_unref(mo);
        if (rel == NULL)
            return -1;
        int64_t r = fat32_stat_path(rel, &size, &is_dir);

        kmfree(rel);
        if (r < 0)
            return -1;
        memset(out, 0, sizeof(*out));
        out->st_mode = is_dir ? (S_IFDIR | 0755) : (S_IFREG | 0644);
        out->st_nlink = 1;
        out->st_size = size;
        return 0;
    }

    if (rc == VFS_REDIRECT_EXT) {
        char *rel = memobj_read_path_alloc(mo);
        uint64_t size = 0;
        bool is_dir = false;
        uint32_t mode = 0;

        memobj_unref(mo);
        if (rel == NULL)
            return -1;
        int64_t r = ext2_stat_path(rel, &size, &is_dir, &mode);

        kmfree(rel);
        if (r < 0)
            return -1;
        memset(out, 0, sizeof(*out));
        out->st_mode = (uint16_t) mode;
        out->st_nlink = 1;
        out->st_size = size;
        return 0;
    }

    if (rc < 0) {
        memobj_unref(mo);
        return rc;
    }

    memobj_copy_out(mo, out, sizeof(vfs_stat_t), VFS_IO_DATA_OFF);
    memobj_unref(mo);
    return 0;
}

int64_t vfs_access_path(const char *cwd, const char *path, uint64_t mode)
{
    handle_t mh;
    memobj_t *mo = server_memobj(VFS_IO_BUF_SIZE, &mh);

    if (mo == NULL)
        return -1;

    memobj_pack_cwd_path(mo, cwd, path);

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = VFS_FACCESSAT;
    req.words[0] = mode;
    req.xfer[0] = mh;
    req.xfer_count = 1;

    if (!router_forward(SVC_FS, &req, &rep)) {
        memobj_unref(mo);
        return -1;
    }

    int64_t rc = (int64_t) rep.words[0];

    if (rc == VFS_REDIRECT_FAT) {
        char *rel = memobj_read_path_alloc(mo);
        uint64_t size = 0;
        bool is_dir = false;
        int64_t r;

        memobj_unref(mo);
        if (rel == NULL)
            return -1;
        r = (fat32_stat_path(rel, &size, &is_dir) == 0) ? 0 : -1;
        kmfree(rel);
        return r;
    }

    if (rc == VFS_REDIRECT_EXT) {
        char *rel = memobj_read_path_alloc(mo);
        uint64_t size = 0;
        bool is_dir = false;
        uint32_t mode = 0;
        int64_t r;

        memobj_unref(mo);
        if (rel == NULL)
            return -1;
        r = (ext2_stat_path(rel, &size, &is_dir, &mode) == 0) ? 0 : -1;
        kmfree(rel);
        return r;
    }

    memobj_unref(mo);
    return rc;
}

int64_t vfs_unlink_path(const char *cwd, const char *path)
{
    handle_t mh;
    memobj_t *mo = server_memobj(VFS_IO_BUF_SIZE, &mh);

    if (mo == NULL)
        return -1;

    memobj_pack_cwd_path(mo, cwd, path);

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = VFS_UNLINK;
    req.xfer[0] = mh;
    req.xfer_count = 1;

    if (!router_forward(SVC_FS, &req, &rep)) {
        memobj_unref(mo);
        return -1;
    }

    int64_t rc = (int64_t) rep.words[0];

    if (rc == VFS_REDIRECT_FAT)
        rc = -30;               /* -EROFS: the FAT mount is read-only */
    if (rc == VFS_REDIRECT_EXT)
        rc = -30;               /* -EROFS: the ext2 mount is read-only */
    memobj_unref(mo);
    return rc;
}

/* Pack "cwd\0a\0b\0" (b may be NULL) for the path calls. */
static void memobj_pack_paths(memobj_t * mo, const char *cwd, const char *a,
                              const char *b)
{
    const char *strs[3] = { cwd, a, b };
    uint64_t off = 0;

    for (int32_t i = 0; i < 3; i++) {
        uint64_t n;

        if (strs[i] == NULL)
            continue;
        n = strlen(strs[i]) + 1;
        if (off + n > VFS_IO_DATA_OFF)
            break;
        memobj_copy_in(mo, strs[i], n, off);
        off += n;
    }
}

/* Forward a path-shaped request (mkdir, symlink, rename). */
static int64_t vfs_path_cmd(uint64_t tag, const char *cwd, const char *a,
                            const char *b)
{
    handle_t mh;
    memobj_t *mo = server_memobj(VFS_IO_BUF_SIZE, &mh);

    if (mo == NULL)
        return -1;

    memobj_pack_paths(mo, cwd, a, b);

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = tag;
    req.xfer[0] = mh;
    req.xfer_count = 1;

    if (!router_forward(SVC_FS, &req, &rep)) {
        memobj_unref(mo);
        return -1;
    }

    int64_t rc = (int64_t) rep.words[0];

    if (rc == VFS_REDIRECT_FAT)
        rc = -30;               /* -EROFS */
    memobj_unref(mo);
    return rc;
}

int64_t vfs_mkdir_path(const char *cwd, const char *path)
{
    return vfs_path_cmd(VFS_MKDIRAT, cwd, path, NULL);
}

int64_t vfs_symlink_path(const char *cwd, const char *target, const char *path)
{
    return vfs_path_cmd(VFS_SYMLINKAT, cwd, path, target);
}

int64_t vfs_rename_path(const char *cwd, const char *oldp, const char *newp)
{
    return vfs_path_cmd(VFS_RENAMEAT, cwd, oldp, newp);
}

int64_t vfs_readlink_path(const char *cwd, const char *path, char *out,
                          uint64_t outsz)
{
    handle_t mh;
    memobj_t *mo = server_memobj(VFS_IO_BUF_SIZE, &mh);

    if (mo == NULL)
        return -1;

    memobj_pack_cwd_path(mo, cwd, path);

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = VFS_READLINK;
    req.words[0] = outsz;
    req.xfer[0] = mh;
    req.xfer_count = 1;

    if (!router_forward(SVC_FS, &req, &rep)) {
        memobj_unref(mo);
        return -1;
    }

    int64_t rc = (int64_t) rep.words[0];
    uint64_t n = 0;

    if (rc == 0) {
        n = rep.words[1];
        if (n > outsz)
            n = outsz;
        if (out != NULL)
            memobj_copy_out(mo, out, n, VFS_IO_DATA_OFF);
    }
    memobj_unref(mo);
    return (rc == 0) ? (int64_t) n : rc;
}

/* Open a path; fills *svc with the service that owns the returned fd. */
vfs_fd_t vfs_open_path(const char *cwd, const char *path, int32_t flags,
                       int32_t *svc)
{
    handle_t mh;
    memobj_t *mo = server_memobj(VFS_IO_BUF_SIZE, &mh);

    if (mo == NULL)
        return VFS_INVALID_FD;

    memobj_pack_cwd_path(mo, cwd, path);

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = VFS_OPENAT;
    req.words[0] = (uint64_t) flags;
    req.xfer[0] = mh;
    req.xfer_count = 1;

    if (!router_forward(SVC_FS, &req, &rep)) {
        memobj_unref(mo);
        return VFS_INVALID_FD;
    }

    int64_t rc = (int64_t) rep.words[0];

    if (rc == VFS_REDIRECT_FAT) {
        char *rel = memobj_read_path_alloc(mo);
        uint64_t size = 0;
        int64_t ffd;

        memobj_unref(mo);
        if (rel == NULL)
            return VFS_INVALID_FD;

        ffd = fat32_open_path(rel, &size);

        if (ffd < 0) {
            kmfree(rel);
            return VFS_INVALID_FD;
        }
        if (svc != NULL)
            *svc = SVC_FAT;
        vfs_fd_t fd = vfs_open_server_svc(ffd, rel, VFS_MODE_READ, size,
                                          SVC_FAT);
        kmfree(rel);
        return fd;
    }

    if (rc == VFS_REDIRECT_EXT) {
        char *rel = memobj_read_path_alloc(mo);
        uint64_t size = 0;
        bool is_dir = false;
        uint32_t mode = 0;
        int64_t efd;

        memobj_unref(mo);
        if (rel == NULL)
            return VFS_INVALID_FD;

        efd = ext2_open_path(rel, &size, &is_dir, &mode);

        if (efd < 0) {
            kmfree(rel);
            return VFS_INVALID_FD;
        }
        if (svc != NULL)
            *svc = SVC_EXT;
        vfs_fd_t fd = vfs_open_server_svc(efd, rel, VFS_MODE_READ, size,
                                          SVC_EXT);
        kmfree(rel);
        return fd;
    }

    if (rc < 0) {
        memobj_unref(mo);
        return VFS_INVALID_FD;
    }

    int64_t sfd = (int64_t) rep.words[1];
    uint64_t size = rep.words[2];

    memobj_unref(mo);
    if (svc != NULL)
        *svc = SVC_FS;
    return vfs_open_server(sfd, path, VFS_MODE_READWRITE, size);
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
int64_t vfs_server_readdir(vfs_fd_t fd, void *out)
{
    vfs_node_desc_t *desc = vfs_fd_to_desc(fd, __func__);

    if (desc == NULL || !desc->server)
        return -1;

    handle_t mh;
    memobj_t *mo = server_memobj(VFS_IO_BUF_SIZE, &mh);

    if (mo == NULL)
        return -1;

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = VFS_READDIR;
    req.words[0] = (uint64_t) desc->server_fd;
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

/* Add a reference to a server fd's open file description (used by fork). */
void vfs_server_ref_fd(int32_t svc, int64_t sfd)
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

int64_t vfs_read(vfs_fd_t fd, uint64_t len, void *buff)
{
    vfs_node_desc_t *desc = vfs_fd_to_desc(fd, __func__);
    if (!desc || !desc->server)
        return 0;

    if (desc->svc == SVC_PIPE)
        return vfs_pipe_rw(desc->server_fd, PIPE_READ, len, buff);
    if (desc->svc == SVC_FAT) {
        /* The FAT server keeps its own offset. */
        return fat32_read_fd(desc->server_fd, len, buff);
    }
    if (desc->svc == SVC_EXT) {
        /* The ext2 server keeps its own offset. */
        return ext2_read_fd(desc->server_fd, len, buff);
    }
    if (desc->svc == SVC_TTY)
        return tty_server_read(buff, len);

    /* The server moves at most VFS_SERVER_IO_MAX per request; loop so a caller
     * asking for the whole file (e.g. the ELF loader) gets it. */
    uint64_t done = 0;

    while (done < len) {
        int64_t n = vfs_server_read(desc->server_fd, len - done,
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

    uint64_t n = strlen(parent);

    while (n > 1 && parent[n - 1] == '/')
        parent[--n] = '\0';

    if (n <= 1) {
        parent[0] = '\0';
        return -1;              /* the root has no parent */
    }

    /* k lands just after the last '/'. */
    uint64_t k = n;

    while (k > 0 && parent[k - 1] != '/')
        k--;

    if (currdir != NULL)
        strcpy(currdir, parent + k);

    if (k == 1) {
        parent[0] = '/';
        parent[1] = '\0';
    } else {
        parent[k - 1] = '\0';
    }

    return 0;
}

int64_t vfs_write(vfs_fd_t fd, uint64_t len, const void *buff)
{
    vfs_node_desc_t *desc = vfs_fd_to_desc(fd, __func__);
    if (!desc || !desc->server)
        return 0;

    if (desc->svc == SVC_PIPE)
        return vfs_pipe_rw(desc->server_fd, PIPE_WRITE, len, (void *) buff);
    if (desc->svc == SVC_TTY)
        return tty_server_write(buff, len);
    return vfs_server_write(desc->server_fd, len, buff);
}

int64_t vfs_pipe_poll(int64_t sfd, int32_t * readable, int32_t * writable)
{
    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = PIPE_POLL;
    req.words[0] = (uint64_t) sfd;

    if (!router_forward(SVC_PIPE, &req, &rep) || (int64_t) rep.words[0] < 0)
        return -1;

    if (readable != NULL)
        *readable = (int32_t) rep.words[1];
    if (writable != NULL)
        *writable = (int32_t) rep.words[2];
    return 0;
}

int64_t vfs_pipe_poll_register(int64_t sfd, void *key, int32_t events)
{
    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = PIPE_POLL_WAIT;
    req.words[0] = (uint64_t) sfd;
    req.words[1] = (uint64_t) key;
    req.words[2] = (uint32_t) events;

    if (!router_forward(SVC_PIPE, &req, &rep))
        return -1;
    return (int64_t) rep.words[0];
}

int64_t vfs_seek(vfs_fd_t fd, uint64_t pos, int64_t whence)
{    vfs_node_desc_t *desc = vfs_fd_to_desc(fd, __func__);
    if (!desc || !desc->server)
        return -1;

    if (desc->svc == SVC_PIPE)
        return -1;              /* pipes are not seekable */
    if (desc->svc == SVC_FAT)
        return fat32_seek_fd(desc->server_fd, pos, whence);
    if (desc->svc == SVC_EXT)
        return ext2_seek_fd(desc->server_fd, pos, whence);
    return vfs_server_seek(desc->server_fd, pos, whence);
}

vfs_fd_t vfs_open_server_svc(int64_t server_fd, const char *path,
                             vfs_openmode_t mode, uint64_t size, int32_t svc)
{
    (void) path;

    /* The process server owns the fd table; it allocates the fd. */
    int64_t fd = process_fd_open(svc, server_fd, size, (int64_t) mode);

    return (fd < 0) ? VFS_INVALID_FD : (vfs_fd_t) fd;
}

vfs_fd_t vfs_open_server(int64_t server_fd, const char *path,
                         vfs_openmode_t mode, uint64_t size)
{
    return vfs_open_server_svc(server_fd, path, mode, size, SVC_FS);
}

static int64_t vfs_load_via_server(const char *path, uint8_t **out_buf,
                                   uint64_t *out_len)
{
    /* Pack an empty cwd before the path, so the VFS server resolves it as a
     * path and normalizes "." and "..". */
    handle_t ph;
    uint64_t plen = strlen(path) + 1;
    char *pbuf = kmalloc(plen + 1);

    if (pbuf == NULL)
        return -1;

    pbuf[0] = '\0';
    memcpy(pbuf + 1, path, plen);

    if (ipc_buf_from_kernel(pbuf, plen + 1, &ph) != 0) {
        kmfree(pbuf);
        return -1;
    }
    kmfree(pbuf);

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

uint64_t vfs_tell(vfs_fd_t fd)
{
    vfs_node_desc_t *desc = vfs_fd_to_desc(fd, __func__);

    if (!desc) {
        kloge("VFS: cannot get fd for file %ld\n", fd);
        return 0;
    }
    return desc->server_size;
}

int64_t vfs_close(vfs_fd_t fd)
{
    int32_t kind = 0, svc = 0;
    int64_t sfd = 0;

    /* The process server removes the fd and returns where it lived so the
     * owning server can drop its open file description. */
    if (process_fd_close((int32_t) fd, &kind, &svc, &sfd) != 0)
        return -1;

    if (svc == SVC_FAT)
        return fat32_close_fd(sfd);
    if (svc == SVC_EXT)
        return ext2_close_fd(sfd);

    return vfs_server_close(svc, sfd);
}
