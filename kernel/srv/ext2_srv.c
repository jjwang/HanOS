/**-----------------------------------------------------------------------------

 @file    ext2_srv.c
 @brief   Spawn the userspace ext2 server

 @details
 @verbatim

  The ext2 server is a read-only client of the block server; the kernel hands it
  the block server's endpoint and its own service endpoint. This file spawns it,
  registers it with the router as SVC_EXT, and forwards the filesystem calls.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>
#include <bootinfo.h>
#include <protocol.h>

#include <kconfig.h>
#include <lib/kmalloc.h>
#include <lib/klog.h>
#include <srv/ext2_srv.h>
#include <srv/svc_monitor.h>
#include <srv/block_srv.h>
#include <ipc/ipc.h>
#include <mm/memobj.h>
#include <mm/mm.h>
#include <router/router.h>
#include <proc/sched.h>
#include <fs/vfs.h>

static endpoint_t *ext2_ep = NULL;
static pid_t ext2_spawner = PID_MAX;
static bool ext2_active = false;

static void ext2_spawn_attach(process_t * tc)
{
    if (sched_get_pid() != ext2_spawner || ext2_ep == NULL)
        return;

    endpoint_t *blk = block_server_endpoint();
    if (blk == NULL)
        return;

    handle_t h = handle_alloc(&tc->handles, endpoint_object(ext2_ep),
                              HANDLE_RIGHT_RECV);
    handle_t hb = handle_alloc(&tc->handles, endpoint_object(blk),
                               HANDLE_RIGHT_SEND);
    if (h == HANDLE_INVALID || hb == HANDLE_INVALID)
        return;

    bootinfo_t *bi = kmalloc(sizeof(bootinfo_t));
    if (bi == NULL)
        return;

    memset(bi, 0, sizeof(bootinfo_t));
    bi->magic = BOOTINFO_MAGIC;
    bi->service_ep = h;
    bi->block_ep = hb;
    tc->bootinfo = bi;

    klogi("ext2: attached service endpoint to pid %ld\n", (int64_t) tc->pid);
}

bool ext2_server_start(void)
{
    ext2_ep = endpoint_create();
    if (ext2_ep == NULL)
        return false;

    const char *argv[] = { "ext2", NULL };

    ext2_spawner = sched_get_pid();
    sched_set_spawn_hook(ext2_spawn_attach);
    process_t *tc = sched_execve(DEFAULT_EXT2_SVR, argv, NULL, "/");
    sched_set_spawn_hook(NULL);

    if (tc == NULL)
        return false;

    router_register(SVC_EXT, ext2_ep, tc->pid);
    svc_monitor_set(SVC_EXT, ext2_server_start, "ext2");
    ext2_active = true;
    klogi("ext2: server started and registered as SVC_EXT\n");
    return true;
}

bool ext2_server_active(void)
{
    return ext2_active;
}

static memobj_t *ext2_memobj(uint64_t len, handle_t * out)
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
    return mo;
}

static void ext2_copy_out(memobj_t * mo, void *dst, uint64_t len, uint64_t off)
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

static int64_t ext2_path_call(uint64_t tag, const char *path, uint64_t *size,
                              bool *is_dir, uint32_t *mode)
{
    if (!ext2_active)
        return -1;

    handle_t h;
    memobj_t *mo = ext2_memobj(VFS_IO_BUF_SIZE, &h);
    if (mo == NULL)
        return -1;

    strncpy((char *) PHYS_TO_VIRT(memobj_page(mo, 0)), path, 255);

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = tag;
    req.xfer[0] = h;
    req.xfer_count = 1;

    int64_t rc = -1;
    if (router_forward(SVC_EXT, &req, &rep) && (int64_t) rep.words[0] == 0) {
        if (tag == EXT2_OPEN) {
            rc = (int64_t) rep.words[1];
            if (size != NULL)
                *size = rep.words[2];
            if (is_dir != NULL)
                *is_dir = rep.words[3] != 0;
        } else {
            if (size != NULL)
                *size = rep.words[1];
            if (is_dir != NULL)
                *is_dir = rep.words[2] != 0;
            if (mode != NULL)
                *mode = (uint32_t) rep.words[3];
            rc = 0;
        }
    }

    memobj_unref(mo);
    return rc;
}

int64_t ext2_stat_path(const char *path, uint64_t *size, bool *is_dir,
                       uint32_t *mode)
{
    return ext2_path_call(EXT2_STAT, path, size, is_dir, mode);
}

int64_t ext2_open_path(const char *path, uint64_t *size, bool *is_dir,
                       uint32_t *mode)
{
    return ext2_path_call(EXT2_OPEN, path, size, is_dir, mode);
}

int64_t ext2_read_fd(int64_t fd, uint64_t len, void *buf)
{
    if (!ext2_active)
        return -1;

    handle_t h;
    memobj_t *mo = ext2_memobj(VFS_IO_BUF_SIZE, &h);
    if (mo == NULL)
        return -1;

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = EXT2_READ;
    req.words[0] = (uint64_t) fd;
    req.words[1] = len;
    req.xfer[0] = h;
    req.xfer_count = 1;

    int64_t n = -1;
    if (router_forward(SVC_EXT, &req, &rep) && (int64_t) rep.words[0] == 0) {
        n = (int64_t) rep.words[1];
        if (n > (int64_t) len)
            n = (int64_t) len;
        if (n > 0)
            ext2_copy_out(mo, buf, (uint64_t) n, VFS_IO_DATA_OFF);
    }

    memobj_unref(mo);
    return n;
}

int64_t ext2_seek_fd(int64_t fd, uint64_t off, int64_t whence)
{
    if (!ext2_active)
        return -1;

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = EXT2_SEEK;
    req.words[0] = (uint64_t) fd;
    req.words[1] = off;
    req.words[2] = (uint64_t) whence;

    if (!router_forward(SVC_EXT, &req, &rep) || (int64_t) rep.words[0] < 0)
        return -1;

    return (int64_t) rep.words[1];
}

int64_t ext2_close_fd(int64_t fd)
{
    if (!ext2_active)
        return -1;

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = EXT2_CLOSE;
    req.words[0] = (uint64_t) fd;

    if (!router_forward(SVC_EXT, &req, &rep) || (int64_t) rep.words[0] < 0)
        return -1;

    return 0;
}

int64_t ext2_fstat_fd(int64_t fd, uint64_t *size, bool *is_dir, uint32_t *mode)
{
    if (!ext2_active)
        return -1;

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = EXT2_FSTAT;
    req.words[0] = (uint64_t) fd;

    if (!router_forward(SVC_EXT, &req, &rep) || (int64_t) rep.words[0] < 0)
        return -1;

    if (size != NULL)
        *size = rep.words[1];
    if (is_dir != NULL)
        *is_dir = rep.words[2] != 0;
    if (mode != NULL)
        *mode = (uint32_t) rep.words[3];
    return 0;
}

int64_t ext2_readdir_fd(int64_t fd, uint64_t index, char *name,
                        uint64_t namesz, uint64_t *size, bool *is_dir)
{
    if (!ext2_active)
        return -1;

    handle_t h;
    memobj_t *mo = ext2_memobj(VFS_IO_BUF_SIZE, &h);
    if (mo == NULL)
        return -1;

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = EXT2_READDIR;
    req.words[0] = (uint64_t) fd;
    req.words[1] = index;
    req.xfer[0] = h;
    req.xfer_count = 1;

    int64_t rc = -1;
    if (router_forward(SVC_EXT, &req, &rep)) {
        if ((int64_t) rep.words[0] == 0) {
            const char *src = (const char *)
                (PHYS_TO_VIRT(memobj_page(mo, VFS_IO_DATA_OFF / PAGE_SIZE))
                 + (VFS_IO_DATA_OFF % PAGE_SIZE));
            uint64_t i = 0;

            while (i + 1 < namesz && src[i] != '\0') {
                name[i] = src[i];
                i++;
            }
            name[i] = '\0';
            if (size != NULL)
                *size = rep.words[1];
            if (is_dir != NULL)
                *is_dir = rep.words[2] != 0;
            rc = 0;
        } else if ((int64_t) rep.words[0] == -1) {
            rc = -2;
        }
    }

    memobj_unref(mo);
    return rc;
}

/* Read a short file through the server at boot to verify the mount. */
static void ext2_probe_read(const char *path)
{
    uint64_t size = 0;
    bool is_dir = false;
    uint32_t mode = 0;
    int64_t fd = ext2_open_path(path, &size, &is_dir, &mode);

    if (fd < 0) {
        klogw("ext2: open %s failed\n", path);
        return;
    }

    uint8_t preview[24];
    int64_t n = ext2_read_fd(fd, sizeof(preview) - 1, preview);

    if (n > 0) {
        preview[n] = '\0';
        klogi("ext2: read %s %ld/%lu mode %o: %s\n", path, (int64_t) n,
              (unsigned long) size, (unsigned) mode, preview);
    } else {
        klogw("ext2: read %s failed\n", path);
    }

    ext2_close_fd(fd);
}

void ext2_server_probe(void)
{
    ext2_probe_read("/assets/readme.txt");
}
