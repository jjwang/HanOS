/**-----------------------------------------------------------------------------

 @file    vfs.c
 @brief   Userspace VFS/name server over the boot initrd

 @details
 @verbatim

   The kernel forwards FS syscalls here through the service router. The server
   parses the ustar initrd it is handed in bootinfo and serves the paths it
   contains: access checks, open, read, directory listing, stat and unlink. It
   also keeps a small set of files created at runtime in RAM.

   Bulk data (a path in, or a read buffer, stat or dirent out) travels in a
   memory object the kernel moved to the server in xfer[1]; the server maps it,
   fills or reads it, and unmaps it.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <bootinfo.h>
#include <protocol.h>
#include <string.h>
#include <sysfunc.h>

/* Where the server maps an incoming memory object (path or data). */
#define VFS_BUF_VADDR       0x20000000
#define VFS_PATH_MAX        128
#define VFS_NAME_MAX        100
#define VFS_MAX_FDS         16
#define VFS_MAX_ENTS        16384
#define VFS_MAX_DYN         16
#define VFS_DYN_CAP         8192

/* ustar header field offsets. */
#define USTAR_SIZE_OFF      124
#define USTAR_TYPE_OFF      156
#define USTAR_MAGIC_OFF     257
#define USTAR_BLOCK         512

/* whence values, matching kernel/fs/vfs.h. */
#define VFS_SEEK_CUR        1
#define VFS_SEEK_END        2
#define VFS_SEEK_SET        3

/* resolve() returns this for the root directory. */
#define ROOT_INDEX          (-2)

/**
 * @brief A file or directory entry from the initrd
 */
typedef struct {
    char name[VFS_NAME_MAX + 1];
    uint64_t off;               /* data offset inside the initrd */
    uint64_t size;
    bool is_dir;
    bool deleted;               /* removed at runtime (initrd is read-only) */
} vfs_ent_t;

/**
 * @brief A file created at runtime with server-side storage
 */
typedef struct {
    bool used;
    char name[VFS_PATH_MAX];
    char *data;
    uint64_t size;
    uint64_t cap;
} vfs_dyn_t;

static vfs_ent_t ents[VFS_MAX_ENTS];
static int ent_count;

static vfs_dyn_t dyns[VFS_MAX_DYN];

static struct {
    bool in_use;
    bool is_dir;
    int ent;                    /* resolved index (static or dynamic) */
    uint64_t off;               /* read offset for files */
    uint64_t dir_idx;           /* readdir cursor for directories */
    char dir[VFS_PATH_MAX];     /* normalized directory path */
    uint32_t refs;              /* number of forks sharing this description */
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

#define VFS_MOUNT_FAT "/fat"

/* Join cwd and path into a normalized absolute path. cwd is already absolute. */
static void join_path(const char *cwd, const char *path, char *out,
                      uint64_t outsz)
{
    if (path[0] == '/') {
        out[0] = '/';
        out[1] = '\0';
    } else {
        uint64_t n = strlen(cwd);

        if (n == 0 || n >= outsz)
            n = 0;
        memcpy(out, cwd, n);
        out[n] = '\0';
        if (n == 0) {
            out[0] = '/';
            out[1] = '\0';
        }
    }

    uint64_t i = 0;

    while (path[i] != '\0') {
        char comp[VFS_PATH_MAX];

        while (path[i] == '/')
            i++;
        if (path[i] == '\0')
            break;

        uint64_t j = i;
        while (path[j] != '\0' && path[j] != '/')
            j++;

        uint64_t clen = j - i;
        if (clen >= sizeof(comp))
            clen = sizeof(comp) - 1;
        memcpy(comp, &path[i], clen);
        comp[clen] = '\0';
        i = j;

        if (strcmp(comp, ".") == 0)
            continue;

        if (strcmp(comp, "..") == 0) {
            uint64_t n = strlen(out);
            while (n > 1 && out[n - 1] == '/')
                n--;
            if (n > 1) {
                uint64_t k = n - 1;
                while (k > 0 && out[k] != '/')
                    k--;
                out[(k == 0) ? 1 : k] = '\0';
            }
            continue;
        }

        uint64_t olen = strlen(out);
        if (olen > 0 && out[olen - 1] != '/')
            strncat(out, "/", outsz - olen - 1);
        strncat(out, comp, outsz - strlen(out) - 1);
    }
}

/* True when abs is /fat or below it; rel receives the mount-relative path. */
static bool fat_rel(const char *abs, char *rel, uint64_t relsz)
{
    uint64_t ml = strlen(VFS_MOUNT_FAT);

    if (strcmp(abs, VFS_MOUNT_FAT) == 0) {
        strcpy(rel, "/");
        return true;
    }
    if (strncmp(abs, VFS_MOUNT_FAT, ml) == 0 && abs[ml] == '/') {
        uint64_t n = strlen(abs + ml);

        if (n >= relsz)
            n = relsz - 1;
        memcpy(rel, abs + ml, n);
        rel[n] = '\0';
        return true;
    }
    return false;
}

/* Split "cwd\0path\0" read from the request buffer. */
static void split_cwd_path(const uint8_t *buf, char *cwd, char *path)
{
    int i = 0;

    while (i < VFS_PATH_MAX - 1 && buf[i] != '\0') {
        cwd[i] = (char) buf[i];
        i++;
    }
    cwd[i] = '\0';
    if (buf[i] != '\0') {
        path[0] = '\0';
        return;
    }
    i++;

    int j = 0;

    while (i < VFS_PATH_MAX - 1 && buf[i] != '\0')
        path[j++] = (char) buf[i++];
    path[j] = '\0';
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
    e->deleted = false;
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

static bool idx_is_dyn(int i)
{
    return i >= ent_count;
}

static bool idx_is_dir(int i)
{
    if (i == ROOT_INDEX)
        return true;
    if (idx_is_dyn(i))
        return false;
    return ents[i].is_dir;
}

static uint64_t idx_size(int i)
{
    if (idx_is_dyn(i))
        return dyns[i - ent_count].size;
    return ents[i].size;
}

static const char *idx_name(int i)
{
    if (idx_is_dyn(i))
        return dyns[i - ent_count].name;
    return ents[i].name;
}

/* Resolve an absolute path to an index, ROOT_INDEX for the root, or -1. */
static int resolve(const char *path)
{
    while (*path == '/')
        path++;

    if (*path == '\0')
        return ROOT_INDEX;

    for (int i = 0; i < ent_count; i++) {
        if (ents[i].deleted)
            continue;
        if (strcmp(ents[i].name, path) == 0)
            return i;
    }

    for (int i = 0; i < VFS_MAX_DYN; i++) {
        if (dyns[i].used && strcmp(dyns[i].name, path) == 0)
            return ent_count + i;
    }
    return -1;
}

/* Create a runtime file (path already normalized). Returns the index or -1. */
static int dyn_create(const char *path)
{
    for (int i = 0; i < VFS_MAX_DYN; i++) {
        if (dyns[i].used)
            continue;

        char *data = (char *) sys_malloc(VFS_DYN_CAP);
        if (data == NULL)
            return -1;

        memset(&dyns[i], 0, sizeof(dyns[i]));
        dyns[i].used = true;
        dyns[i].data = data;
        dyns[i].cap = VFS_DYN_CAP;

        uint64_t n = strlen(path);
        if (n >= sizeof(dyns[i].name))
            n = sizeof(dyns[i].name) - 1;
        memcpy(dyns[i].name, path, n);
        dyns[i].name[n] = '\0';

        return ent_count + i;
    }
    return -1;
}

/* Fill a stat for a resolved index. */
static void fill_stat(int e, void *out)
{
    stat_t *st = (stat_t *) out;

    memset(st, 0, sizeof(*st));
    st->st_nlink = 1;

    if (idx_is_dir(e)) {
        st->st_mode = S_IFDIR | 0755;
        st->st_ino = (e == ROOT_INDEX) ? 1 : (uint64_t) e + 1;
    } else {
        st->st_mode = S_IFREG | 0644;
        st->st_ino = (uint64_t) e + 1;
        st->st_size = (int64_t) idx_size(e);
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
            fds[i].refs = 1;
            return i + 1;
        }
    }
    return -1;
}

static void handle(sys_ipc_msg_t * m, sys_ipc_msg_t * rep)
{
    if (m->tag == VFS_PING) {
        rep->words[0] = VFS_PONG;
        return;
    }

    if (m->tag == VFS_FACCESSAT) {
        char cwd[VFS_PATH_MAX], path[VFS_PATH_MAX], abs[VFS_PATH_MAX];
        char rel[VFS_PATH_MAX];
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;
        uint8_t *buf = (memh != 0) ? map_buf(memh) : NULL;

        if (buf == NULL) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -5;    /* -EIO */
            return;
        }

        split_cwd_path(buf, cwd, path);
        join_path(cwd, path, abs, sizeof(abs));

        if (fat_rel(abs, rel, sizeof(rel))) {
            strcpy((char *) buf, rel);
            rep->words[0] = VFS_REDIRECT_FAT;
        } else {
            rep->words[0] = (resolve(abs) >= 0) ? 0 : (uint64_t) (int64_t) -2;
        }

        sys_mem_unmap(memh, VFS_BUF_VADDR);
        sys_handle_close(memh);
        return;
    }

    if (m->tag == VFS_FSTATAT) {
        char cwd[VFS_PATH_MAX], path[VFS_PATH_MAX], abs[VFS_PATH_MAX];
        char rel[VFS_PATH_MAX];
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;
        uint8_t *buf = (memh != 0) ? map_buf(memh) : NULL;
        int e;

        if (buf == NULL) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -5;    /* -EIO */
            return;
        }

        split_cwd_path(buf, cwd, path);
        join_path(cwd, path, abs, sizeof(abs));

        if (fat_rel(abs, rel, sizeof(rel))) {
            strcpy((char *) buf, rel);
            rep->words[0] = VFS_REDIRECT_FAT;
        } else {
            e = resolve(abs);
            if (e < 0) {
                rep->words[0] = (uint64_t) (int64_t) -2;        /* -ENOENT */
            } else {
                fill_stat(e, buf + VFS_IO_DATA_OFF);
                rep->words[0] = 0;
            }
        }

        sys_mem_unmap(memh, VFS_BUF_VADDR);
        sys_handle_close(memh);
        return;
    }

    if (m->tag == VFS_FSTAT) {
        int fd = (int) m->words[0];
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;
        uint8_t *buf = (memh != 0) ? map_buf(memh) : NULL;

        if (buf == NULL || fd <= 0 || fd > VFS_MAX_FDS
            || !fds[fd - 1].in_use) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -9;    /* -EBADF */
            return;
        }

        fill_stat(fds[fd - 1].ent, buf + VFS_IO_DATA_OFF);
        rep->words[0] = 0;

        sys_mem_unmap(memh, VFS_BUF_VADDR);
        sys_handle_close(memh);
        return;
    }

    if (m->tag == VFS_OPENAT) {
        char cwd[VFS_PATH_MAX], path[VFS_PATH_MAX], abs[VFS_PATH_MAX];
        char rel[VFS_PATH_MAX];
        char norm[VFS_PATH_MAX];
        uint64_t flags = m->words[0];
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;
        uint8_t *buf = (memh != 0) ? map_buf(memh) : NULL;
        int e;

        if (buf == NULL) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -5;    /* -EIO */
            return;
        }

        split_cwd_path(buf, cwd, path);
        join_path(cwd, path, abs, sizeof(abs));

        if (fat_rel(abs, rel, sizeof(rel))) {
            strcpy((char *) buf, rel);
            rep->words[0] = VFS_REDIRECT_FAT;
            sys_mem_unmap(memh, VFS_BUF_VADDR);
            sys_handle_close(memh);
            return;
        }

        e = resolve(abs);

        if (e < 0 && (flags & O_CREAT)) {
            norm_dir(abs, norm, sizeof(norm));
            if (norm[0] != '\0')
                e = dyn_create(norm);
        }

        if (e < 0) {
            rep->words[0] = (uint64_t) (int64_t) -2;    /* -ENOENT */
            sys_mem_unmap(memh, VFS_BUF_VADDR);
            sys_handle_close(memh);
            return;
        }

        int fd = fd_alloc();
        if (fd < 0) {
            rep->words[0] = (uint64_t) (int64_t) -24;   /* -EMFILE */
            sys_mem_unmap(memh, VFS_BUF_VADDR);
            sys_handle_close(memh);
            return;
        }

        if (idx_is_dir(e)) {
            fds[fd - 1].is_dir = true;
            norm_dir(abs, fds[fd - 1].dir, sizeof(fds[fd - 1].dir));
        } else {
            fds[fd - 1].ent = e;
        }

        rep->words[0] = 0;
        rep->words[1] = (uint64_t) fd;
        rep->words[2] = idx_is_dir(e) ? 0 : idx_size(e);
        sys_mem_unmap(memh, VFS_BUF_VADDR);
        sys_handle_close(memh);
        return;
    }

    if (m->tag == VFS_READ) {
        int fd = (int) m->words[0];
        uint64_t len = m->words[1];
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;

        if (fd < 1 || fd > VFS_MAX_FDS || !fds[fd - 1].in_use
            || fds[fd - 1].is_dir) {
            rep->words[0] = (uint64_t) (int64_t) -9;    /* -EBADF */
        } else {
            int e = fds[fd - 1].ent;
            uint64_t off = fds[fd - 1].off;
            uint64_t fsize = idx_size(e);
            uint64_t n = (off < fsize) ? fsize - off : 0;

            if (n > len)
                n = len;

            if (n > 0 && memh != 0
                && sys_mem_map(memh, VFS_BUF_VADDR, 3) == 0) {
                const uint8_t *src = idx_is_dyn(e)
                    ? (const uint8_t *) dyns[e - ent_count].data + off
                    : (const uint8_t *) bi.initrd_vaddr + ents[e].off + off;
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

    if (m->tag == VFS_WRITE) {
        int fd = (int) m->words[0];
        uint64_t len = m->words[1];
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;

        if (fd < 1 || fd > VFS_MAX_FDS || !fds[fd - 1].in_use
            || fds[fd - 1].is_dir) {
            rep->words[0] = (uint64_t) (int64_t) -9;    /* -EBADF */
        } else if (!idx_is_dyn(fds[fd - 1].ent)) {
            rep->words[0] = (uint64_t) (int64_t) -30;   /* -EROFS */
        } else {
            vfs_dyn_t *d = &dyns[fds[fd - 1].ent - ent_count];
            uint64_t off = fds[fd - 1].off;
            uint64_t n = len;

            if (off >= d->cap)
                n = 0;
            else if (off + n > d->cap)
                n = d->cap - off;

            if (n > 0 && memh != 0
                && sys_mem_map(memh, VFS_BUF_VADDR, 1) == 0) {
                const uint8_t *src = (const uint8_t *) VFS_BUF_VADDR;

                for (uint64_t i = 0; i < n; i++)
                    d->data[off + i] = (char) src[i];
                sys_mem_unmap(memh, VFS_BUF_VADDR);
            } else {
                n = 0;
            }

            fds[fd - 1].off += n;
            if (fds[fd - 1].off > d->size)
                d->size = fds[fd - 1].off;
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

        const char *dir = fds[fd - 1].dir;
        uint64_t wanted = fds[fd - 1].dir_idx;
        uint64_t seen = 0;
        int found = -1;

        for (int i = 0; i < ent_count && found < 0; i++) {
            if (ents[i].deleted)
                continue;
            if (is_child(ents[i].name, dir)) {
                if (seen == wanted) {
                    found = i;
                    break;
                }
                seen++;
            }
        }
        for (int i = 0; i < VFS_MAX_DYN && found < 0; i++) {
            if (dyns[i].used && is_child(dyns[i].name, dir)) {
                if (seen == wanted) {
                    found = ent_count + i;
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
            const char *name = idx_name(found);
            const char *base = name;
            uint64_t dl = strlen(dir);

            if (dl > 0)
                base = name + dl + 1;

            memset(de, 0, sizeof(*de));
            de->d_ino = (uint64_t) found + 1;
            de->d_type = idx_is_dir(found) ? DT_DIR : DT_REG;
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

    if (m->tag == VFS_CLOSE) {
        int fd = (int) m->words[0];

        if (fd >= 1 && fd <= VFS_MAX_FDS && fds[fd - 1].in_use) {
            if (fds[fd - 1].refs > 0)
                fds[fd - 1].refs--;
            if (fds[fd - 1].refs == 0)
                fds[fd - 1].in_use = false;
            rep->words[0] = 0;
        } else {
            rep->words[0] = (uint64_t) (int64_t) -9;
        }
        return;
    }

    if (m->tag == VFS_FD_FORK) {
        int fd = (int) m->words[0];

        if (fd >= 1 && fd <= VFS_MAX_FDS && fds[fd - 1].in_use) {
            fds[fd - 1].refs++;
            rep->words[0] = 0;
        } else {
            rep->words[0] = (uint64_t) (int64_t) -9;
        }
        return;
    }

    if (m->tag == VFS_SEEK) {
        int fd = (int) m->words[0];
        int64_t pos = (int64_t) m->words[1];
        int64_t whence = (int64_t) m->words[2];

        if (fd < 1 || fd > VFS_MAX_FDS || !fds[fd - 1].in_use
            || fds[fd - 1].is_dir) {
            rep->words[0] = (uint64_t) (int64_t) -9;    /* -EBADF */
            return;
        }

        uint64_t fsize = idx_size(fds[fd - 1].ent);
        int64_t offset = -1;

        switch (whence) {
        case VFS_SEEK_SET:
            offset = pos;
            break;
        case VFS_SEEK_CUR:
            offset = (int64_t) fds[fd - 1].off + pos;
            break;
        case VFS_SEEK_END:
            offset = (int64_t) fsize - pos;
            break;
        }

        if (offset < 0 || offset > (int64_t) fsize) {
            rep->words[0] = (uint64_t) (int64_t) -22;   /* -EINVAL */
            return;
        }

        fds[fd - 1].off = (uint64_t) offset;
        rep->words[0] = 0;
        rep->words[1] = (uint64_t) offset;
        return;
    }

    if (m->tag == VFS_UNLINK) {
        char cwd[VFS_PATH_MAX], path[VFS_PATH_MAX], abs[VFS_PATH_MAX];
        char rel[VFS_PATH_MAX];
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;
        uint8_t *buf = (memh != 0) ? map_buf(memh) : NULL;
        int e;

        if (buf == NULL) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -5;    /* -EIO */
            return;
        }

        split_cwd_path(buf, cwd, path);
        join_path(cwd, path, abs, sizeof(abs));

        if (fat_rel(abs, rel, sizeof(rel))) {
            strcpy((char *) buf, rel);
            rep->words[0] = VFS_REDIRECT_FAT;
        } else {
            e = resolve(abs);
            if (e < 0 || idx_is_dir(e)) {
                rep->words[0] = (uint64_t) (int64_t) -2;        /* -ENOENT */
            } else if (idx_is_dyn(e)) {
                dyns[e - ent_count].used = false;
                rep->words[0] = 0;
            } else {
                ents[e].deleted = true;
                rep->words[0] = 0;
            }
        }

        sys_mem_unmap(memh, VFS_BUF_VADDR);
        sys_handle_close(memh);
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
