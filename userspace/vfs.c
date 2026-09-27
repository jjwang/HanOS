/**-----------------------------------------------------------------------------

 @file    vfs.c
 @brief   Userspace VFS/name server over the boot initrd

 @details
 @verbatim

   The kernel forwards FS syscalls here through the service router. The server
   parses the ustar initrd it is handed in bootinfo and serves the paths it
   contains: access checks, open, read, directory listing and stat. Bulk data
   (a path in, or a read buffer, stat or dirent out) travels in a memory object
   the kernel moved to the server in xfer[1]; the server maps it, fills or
   reads it, and unmaps it.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <libc/bootinfo.h>
#include <libc/protocol.h>
#include <libc/string.h>
#include <libc/sysfunc.h>

/* Where the server maps an incoming memory object (path or data). */
#define VFS_BUF_VADDR       0x20000000
#define VFS_PATH_MAX        128
#define VFS_NAME_MAX        100
#define VFS_MAX_FDS         16
#define VFS_MAX_ENTS        128

/* ustar header field offsets. */
#define USTAR_SIZE_OFF      124
#define USTAR_TYPE_OFF      156
#define USTAR_MAGIC_OFF     257
#define USTAR_BLOCK         512

typedef struct {
    char name[VFS_NAME_MAX + 1];
    uint64_t off;               /* data offset inside the initrd */
    uint64_t size;
    bool is_dir;
} vfs_ent_t;

static vfs_ent_t ents[VFS_MAX_ENTS];
static int ent_count;

static struct {
    bool in_use;
    bool is_dir;
    int ent;                    /* index in ents[] for files */
    uint64_t off;               /* read offset for files */
    uint64_t dir_idx;           /* readdir cursor for directories */
    char dir[VFS_PATH_MAX];     /* normalized directory path */
} fds[VFS_MAX_FDS];

static bootinfo_t bi;

static uint64_t oct2bin(const uint8_t *p, int n)
{
    uint64_t v = 0;

    for (int i = 0; i < n; i++) {
        if (p[i] < '0' || p[i] > '7')
            break;
        v = v * 8 + (uint64_t) (p[i] - '0');
    }
    return v;
}

static bool has_slash(const char *s)
{
    while (*s != '\0') {
        if (*s == '/')
            return true;
        s++;
    }
    return false;
}

/* Copy a path without its leading and trailing slashes ('' for the root). */
static void norm_dir(const char *path, char *out, uint64_t outsz)
{
    uint64_t n;

    while (*path == '/')
        path++;

    n = strlen(path);
    while (n > 0 && path[n - 1] == '/')
        n--;

    if (n >= outsz)
        n = outsz - 1;
    memcpy(out, path, n);
    out[n] = '\0';
}

/* Record one archive entry, stripping a trailing '/' from directories. */
static void add_ent(const uint8_t *hdr, uint64_t data_off, uint64_t size)
{
    if (ent_count >= VFS_MAX_ENTS)
        return;

    uint8_t type = hdr[USTAR_TYPE_OFF];
    bool is_dir = (type == '5');
    bool is_file = (type == '0' || type == '\0');

    if (!is_dir && !is_file)
        return;

    vfs_ent_t *e = &ents[ent_count];
    int i = 0;

    while (i < VFS_NAME_MAX && hdr[i] != '\0') {
        e->name[i] = (char) hdr[i];
        i++;
    }
    e->name[i] = '\0';

    if (is_dir && i > 0 && e->name[i - 1] == '/')
        e->name[i - 1] = '\0';

    e->off = data_off;
    e->size = size;
    e->is_dir = is_dir;
    ent_count++;
}

static void parse_initrd(void)
{
    const uint8_t *base = (const uint8_t *) bi.initrd_vaddr;
    const uint8_t *p = base;

    ent_count = 0;

    if (base == NULL || bi.initrd_size == 0)
        return;

    while ((uint64_t) (p - base) + USTAR_BLOCK <= bi.initrd_size
           && memcmp(p + USTAR_MAGIC_OFF, "ustar", 5) == 0) {
        uint64_t size = oct2bin(p + USTAR_SIZE_OFF, 11);
        uint64_t data_off = (uint64_t) (p + USTAR_BLOCK - base);

        add_ent(p, data_off, size);
        p += ((size + USTAR_BLOCK - 1) / USTAR_BLOCK + 1) * USTAR_BLOCK;
    }
}

/* Resolve an absolute path to an ents[] index, or ent_count for the root,
 * or -1 when it does not exist. */
static int resolve(const char *path)
{
    while (*path == '/')
        path++;

    if (*path == '\0')
        return ent_count;

    for (int i = 0; i < ent_count; i++) {
        if (strcmp(ents[i].name, path) == 0)
            return i;
    }
    return -1;
}

/* Fill a stat for an ents[] index (ent_count means the root directory). */
static void fill_stat(int e, void *out)
{
    stat_t *st = (stat_t *) out;

    memset(st, 0, sizeof(*st));
    st->st_nlink = 1;

    if (e == ent_count || ents[e].is_dir) {
        st->st_mode = S_IFDIR | 0755;
        st->st_ino = (uint64_t) e + 1;
    } else {
        st->st_mode = S_IFREG | 0644;
        st->st_ino = (uint64_t) e + 1;
        st->st_size = (int64_t) ents[e].size;
    }
}

/* Is `name` a direct child of directory `dir`? */
static bool is_child(const char *name, const char *dir)
{
    uint64_t dl = strlen(dir);

    if (dl == 0)
        return !has_slash(name);
    if (strncmp(name, dir, dl) != 0 || name[dl] != '/')
        return false;
    return !has_slash(name + dl + 1);
}

static uint8_t *map_buf(int64_t memh)
{
    if (sys_mem_map(memh, VFS_BUF_VADDR, 3) != 0)
        return NULL;
    return (uint8_t *) VFS_BUF_VADDR;
}

static int fd_alloc(void)
{
    for (int i = 0; i < VFS_MAX_FDS; i++) {
        if (!fds[i].in_use) {
            memset(&fds[i], 0, sizeof(fds[i]));
            fds[i].in_use = true;
            return i + 1;
        }
    }
    return -1;
}

/* Read the request's path from the buffer in xfer[1]. */
static void read_path(const sys_ipc_msg_t *m, char *path)
{
    int i = 0;

    path[0] = '\0';

    if (m->xfer_count >= 2) {
        int64_t memh = (int64_t) m->xfer[1];
        uint8_t *buf = map_buf(memh);

        if (buf != NULL) {
            while (i < VFS_PATH_MAX - 1 && buf[i] != '\0') {
                path[i] = (char) buf[i];
                i++;
            }
            sys_mem_unmap(memh, VFS_BUF_VADDR);
        }
        sys_handle_close(memh);
    } else {
        const char *p = (const char *) &m->words[1];

        while (i < VFS_PATH_MAX - 1 && p[i] != '\0') {
            path[i] = p[i];
            i++;
        }
    }

    path[i] = '\0';
}

static void handle(sys_ipc_msg_t * m, sys_ipc_msg_t * rep)
{
    if (m->tag == VFS_PING) {
        rep->words[0] = VFS_PONG;
        return;
    }

    if (m->tag == VFS_FACCESSAT) {
        char path[VFS_PATH_MAX];

        read_path(m, path);
        rep->words[0] = (resolve(path) >= 0) ? 0 : (uint64_t) (int64_t) -2;
        return;
    }

    if (m->tag == VFS_FSTATAT) {
        char path[VFS_PATH_MAX];
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;
        uint8_t *buf = (memh != 0) ? map_buf(memh) : NULL;
        int e;
        int i = 0;

        if (buf == NULL) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -5;    /* -EIO */
            return;
        }

        while (i < VFS_PATH_MAX - 1 && buf[i] != '\0') {
            path[i] = (char) buf[i];
            i++;
        }
        path[i] = '\0';

        e = resolve(path);
        if (e < 0) {
            rep->words[0] = (uint64_t) (int64_t) -2;    /* -ENOENT */
        } else {
            fill_stat(e, buf + VFS_IO_DATA_OFF);
            rep->words[0] = 0;
        }

        sys_mem_unmap(memh, VFS_BUF_VADDR);
        sys_handle_close(memh);
        return;
    }

    if (m->tag == VFS_OPENAT) {
        char path[VFS_PATH_MAX];
        int e;

        read_path(m, path);
        e = resolve(path);

        if (e < 0) {
            rep->words[0] = (uint64_t) (int64_t) -2;    /* -ENOENT */
            return;
        }

        int fd = fd_alloc();
        if (fd < 0) {
            rep->words[0] = (uint64_t) (int64_t) -24;   /* -EMFILE */
            return;
        }

        if (e == ent_count || ents[e].is_dir) {
            fds[fd - 1].is_dir = true;
            norm_dir(path, fds[fd - 1].dir, sizeof(fds[fd - 1].dir));
        } else {
            fds[fd - 1].ent = e;
        }

        rep->words[0] = 0;
        rep->words[1] = (uint64_t) fd;
        rep->words[2] = (e == ent_count || ents[e].is_dir) ? 0 : ents[e].size;
        return;
    }

    if (m->tag == VFS_READ) {
        int fd = (int) m->words[0];
        uint64_t len = m->words[1];
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;

        if (fd < 1 || fd > VFS_MAX_FDS || !fds[fd - 1].in_use) {
            rep->words[0] = (uint64_t) (int64_t) -9;    /* -EBADF */
        } else {
            const vfs_ent_t *e = &ents[fds[fd - 1].ent];
            uint64_t off = fds[fd - 1].off;
            uint64_t n = (off < e->size) ? e->size - off : 0;

            if (n > len)
                n = len;

            if (n > 0 && memh != 0
                && sys_mem_map(memh, VFS_BUF_VADDR, 3) == 0) {
                const uint8_t *src =
                    (const uint8_t *) bi.initrd_vaddr + e->off + off;
                uint8_t *buf = (uint8_t *) VFS_BUF_VADDR;

                for (uint64_t i = 0; i < n; i++)
                    buf[i] = src[i];
                sys_mem_unmap(memh, VFS_BUF_VADDR);
            } else {
                n = 0;
            }

            fds[fd - 1].off += n;
            rep->words[0] = 0;
            rep->words[1] = n;
        }

        if (memh != 0)
            sys_handle_close(memh);
        return;
    }

    if (m->tag == VFS_READDIR) {
        int fd = (int) m->words[0];
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;

        if (fd < 1 || fd > VFS_MAX_FDS || !fds[fd - 1].in_use
            || !fds[fd - 1].is_dir) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -9;    /* -EBADF */
            return;
        }

        uint64_t wanted = fds[fd - 1].dir_idx;
        uint64_t seen = 0;
        int found = -1;

        for (int i = 0; i < ent_count; i++) {
            if (is_child(ents[i].name, fds[fd - 1].dir)) {
                if (seen == wanted) {
                    found = i;
                    break;
                }
                seen++;
            }
        }

        uint8_t *buf = (memh != 0) ? map_buf(memh) : NULL;
        if (buf == NULL) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -5;
            return;
        }

        if (found < 0) {
            rep->words[0] = (uint64_t) (int64_t) -1;    /* end of directory */
        } else {
            dirent_t *de = (dirent_t *) (buf + VFS_IO_DATA_OFF);
            const char *name = ents[found].name;
            const char *base = name;
            uint64_t dl = strlen(fds[fd - 1].dir);

            if (dl > 0)
                base = name + dl + 1;

            memset(de, 0, sizeof(*de));
            de->d_ino = (uint64_t) found + 1;
            de->d_type = ents[found].is_dir ? DT_DIR : DT_REG;
            int k = 0;
            while (base[k] != '\0' && k < (int) sizeof(de->d_name) - 1) {
                de->d_name[k] = base[k];
                k++;
            }
            de->d_name[k] = '\0';

            fds[fd - 1].dir_idx++;
            rep->words[0] = 0;
        }

        sys_mem_unmap(memh, VFS_BUF_VADDR);
        sys_handle_close(memh);
        return;
    }

    if (m->tag == VFS_WRITE) {
        int fd = (int) m->words[0];

        if (m->xfer_count >= 2)
            sys_handle_close((int64_t) m->xfer[1]);

        if (fd < 1 || fd > VFS_MAX_FDS || !fds[fd - 1].in_use)
            rep->words[0] = (uint64_t) (int64_t) -9;    /* -EBADF */
        else
            rep->words[0] = (uint64_t) (int64_t) -30;   /* -EROFS */
        return;
    }

    if (m->tag == VFS_CLOSE) {
        int fd = (int) m->words[0];

        if (fd >= 1 && fd <= VFS_MAX_FDS && fds[fd - 1].in_use) {
            fds[fd - 1].in_use = false;
            rep->words[0] = 0;
        } else {
            rep->words[0] = (uint64_t) (int64_t) -9;
        }
        return;
    }

    rep->words[0] = (uint64_t) (int64_t) -38;   /* -ENOSYS */
}

int main(void)
{
    while (sys_bootinfo(&bi) < 0 || bi.magic != BOOTINFO_MAGIC) {
        /* The kernel sets the bootinfo before the process is runnable. */
    }

    parse_initrd();

    for (;;) {
        sys_ipc_msg_t m;

        if (sys_ipc_recv_timeout((int64_t) bi.service_ep, &m, 1000) != 0)
            continue;

        sys_ipc_msg_t rep;
        memset(&rep, 0, sizeof(rep));
        rep.tag = m.tag;
        handle(&m, &rep);

        int64_t reply = (int64_t) m.xfer[0];
        if (reply != 0) {
            sys_ipc_send(reply, &rep);
            sys_handle_close(reply);
        }
    }

    return 0;
}
