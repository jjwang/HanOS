/**-----------------------------------------------------------------------------

 @file    fat32_srv.c
 @brief   Spawn and probe the userspace FAT32 server

 @details
 @verbatim

   The FAT32 server is a read-only client of the block server; the kernel hands
   it the block server's endpoint and its own service endpoint. This file
   spawns it and provides a boot-time read to verify the path.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <libc/string.h>
#include <libc/bootinfo.h>
#include <libc/protocol.h>

#include <kconfig.h>
#include <base/kmalloc.h>
#include <base/klog.h>
#include <ipc/fat32_srv.h>
#include <ipc/block_srv.h>
#include <ipc/ipc.h>
#include <mm/memobj.h>
#include <mm/mm.h>
#include <service/service.h>
#include <proc/sched.h>

static endpoint_t *fat32_ep = NULL;
static pid_t fat32_spawner = PID_MAX;
static bool fat32_active = false;

static void fat32_spawn_attach(process_t * tc)
{
    if (sched_get_pid() != fat32_spawner || fat32_ep == NULL)
        return;

    endpoint_t *blk = block_server_endpoint();
    if (blk == NULL)
        return;

    handle_t h = handle_alloc(&tc->handles, endpoint_object(fat32_ep),
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

    klogi("fat32: attached service endpoint to pid %ld\n", (long) tc->pid);
}

bool fat32_server_start(void)
{
    fat32_ep = endpoint_create();
    if (fat32_ep == NULL)
        return false;

    const char *argv[] = { "fat32", NULL };

    fat32_spawner = sched_get_pid();
    sched_set_spawn_hook(fat32_spawn_attach);
    process_t *tc = sched_execve(DEFAULT_FAT32_SVR, argv, NULL, "/");
    sched_set_spawn_hook(NULL);

    if (tc == NULL)
        return false;

    service_register(SVC_FAT, fat32_ep, tc->pid);
    fat32_active = true;
    klogi("fat32: server started and registered as SVC_FAT\n");
    return true;
}

bool fat32_server_active(void)
{
    return fat32_active;
}

static memobj_t *fat_memobj(uint64_t len, handle_t * out)
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

static void fat_copy_out(memobj_t * mo, void *dst, uint64_t len, uint64_t off)
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

int64_t fat32_stat_path(const char *path, uint64_t *size, bool *is_dir)
{
    if (!fat32_active)
        return -1;

    handle_t h;
    memobj_t *mo = fat_memobj(VFS_IO_BUF_SIZE, &h);
    if (mo == NULL)
        return -1;

    strncpy((char *) PHYS_TO_VIRT(memobj_page(mo, 0)), path, 255);

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = FAT_STAT;
    req.xfer[0] = h;
    req.xfer_count = 1;

    int64_t rc = -1;
    if (service_forward(SVC_FAT, &req, &rep) && (int64_t) rep.words[0] == 0) {
        if (size != NULL)
            *size = rep.words[1];
        if (is_dir != NULL)
            *is_dir = rep.words[2] != 0;
        rc = 0;
    }

    memobj_unref(mo);
    return rc;
}

int64_t fat32_read_path(const char *path, uint64_t off, uint64_t len,
                        void *buf)
{
    if (!fat32_active)
        return -1;

    handle_t h;
    memobj_t *mo = fat_memobj(VFS_IO_BUF_SIZE, &h);
    if (mo == NULL)
        return -1;

    strncpy((char *) PHYS_TO_VIRT(memobj_page(mo, 0)), path, 255);

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = FAT_READ;
    req.words[0] = off;
    req.words[1] = len;
    req.xfer[0] = h;
    req.xfer_count = 1;

    int64_t n = -1;
    if (service_forward(SVC_FAT, &req, &rep) && (int64_t) rep.words[0] == 0) {
        n = (int64_t) rep.words[1];
        if (n > (int64_t) len)
            n = (int64_t) len;
        if (n > 0)
            fat_copy_out(mo, buf, (uint64_t) n, VFS_IO_DATA_OFF);
    }

    memobj_unref(mo);
    return n;
}

/* Read one directory entry (0-based) through the server. Returns 0 on success,
 * -2 at the end of the directory, -1 on error. */
int64_t fat32_readdir_path(const char *path, uint64_t index, char *name,
                           uint64_t namesz, uint64_t *size, bool *is_dir)
{
    if (!fat32_active)
        return -1;

    handle_t h;
    memobj_t *mo = fat_memobj(VFS_IO_BUF_SIZE, &h);
    if (mo == NULL)
        return -1;

    strncpy((char *) PHYS_TO_VIRT(memobj_page(mo, 0)), path, 255);

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = FAT_READDIR;
    req.words[0] = index;
    req.xfer[0] = h;
    req.xfer_count = 1;

    int64_t rc = -1;
    if (service_forward(SVC_FAT, &req, &rep)) {
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

/* Read a file through the server by moving a buffer memory object to it. */
static void fat32_probe_read(const char *path)
{
    process_t *t = sched_get_current_process();
    if (t == NULL)
        return;

    memobj_t *mo = memobj_create(VFS_IO_BUF_SIZE);
    if (mo == NULL)
        return;

    handle_t h = handle_alloc(&t->handles, memobj_object(mo),
                              HANDLE_RIGHT_READ | HANDLE_RIGHT_WRITE
                              | HANDLE_RIGHT_MAP | HANDLE_RIGHT_TRANSFER);
    if (h == HANDLE_INVALID) {
        memobj_unref(mo);
        return;
    }

    strncpy((char *) PHYS_TO_VIRT(memobj_page(mo, 0)), path, 255);

    ipc_msg_t req;
    ipc_msg_t rep;

    memset(&req, 0, sizeof(req));
    req.tag = FAT_READ;
    req.words[0] = 0;
    req.words[1] = 4096;
    req.xfer[0] = h;
    req.xfer_count = 1;

    if (service_forward(SVC_FAT, &req, &rep)
        && (int64_t) rep.words[0] == 0) {
        uint8_t *data = (uint8_t *)
            PHYS_TO_VIRT(memobj_page(mo, VFS_IO_DATA_OFF / PAGE_SIZE))
            + (VFS_IO_DATA_OFF % PAGE_SIZE);
        char preview[24];

        uint64_t n = rep.words[1];
        uint64_t k = (n < sizeof(preview) - 1) ? n : sizeof(preview) - 1;
        memcpy(preview, data, k);
        preview[k] = '\0';
        klogi("fat32: read %s %lu/%lu bytes: %s\n", path, n, rep.words[2],
              preview);
    } else {
        klogw("fat32: read %s failed (status %ld)\n", path,
              (int64_t) rep.words[0]);
    }

    memobj_unref(mo);
}

void fat32_server_probe(void)
{
    fat32_probe_read("/HELLO.TXT");
    fat32_probe_read("/SUB/NESTED.TXT");
    fat32_probe_read("/MISSING.TXT");
}
