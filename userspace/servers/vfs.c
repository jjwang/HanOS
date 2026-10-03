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
#include <stdlib.h>

#include <bootinfo.h>
#include <protocol.h>
#include <string.h>
#include <sysfunc.h>

/* Where the server maps an incoming memory object (path or data). */
#define VFS_BUF_VADDR       0x20000000
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
 * @brief A file, directory or symlink created at runtime
 */
typedef struct {
    bool used;
    bool is_dir;
    bool is_symlink;
    char *name;
    char *target;               /* symlink target */
    char *data;
    uint64_t size;
    uint64_t cap;
} vfs_dyn_t;

static vfs_ent_t ents[VFS_MAX_ENTS];
static int32_t ent_count;

static vfs_dyn_t dyns[VFS_MAX_DYN];

static struct {
    bool in_use;
    bool is_dir;
    int32_t ent;                    /* resolved index (static or dynamic) */
    uint64_t off;               /* read offset for files */
    uint64_t dir_idx;           /* readdir cursor for directories */
    char *dir;                  /* normalized directory path */
    uint32_t refs;              /* number of forks sharing this description */
} fds[VFS_MAX_FDS];

static bootinfo_t bi;

static uint64_t oct2bin(const uint8_t *p, int32_t n)
{
    uint64_t v = 0;

    for (int32_t i = 0; i < n; i++) {
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

/* Return a copy of a path without its leading and trailing slashes ('' for the
 * root). The caller frees it. */
static char *norm_dir(const char *path)
{
    uint64_t n;
    char *out;

    while (*path == '/')
        path++;

    n = strlen(path);
    while (n > 0 && path[n - 1] == '/')
        n--;

    out = malloc(n + 1);
    if (out == NULL)
        return NULL;
    memcpy(out, path, n);
    out[n] = '\0';
    return out;
}

#define VFS_MOUNT_FAT "/fat"

/* Join cwd and path into a normalized absolute path. The caller frees it. */
static char *join_path(const char *cwd, const char *path)
{
    uint64_t cap = strlen(cwd) + 2 * strlen(path) + 2;
    char *out = malloc(cap);

    if (out == NULL)
        return NULL;

    if (path[0] == '/' || strlen(cwd) == 0)
        strcpy(out, "/");
    else
        strcpy(out, cwd);

    uint64_t i = 0;

    while (path[i] != '\0') {
        uint64_t j;
        uint64_t clen;
        char *comp;

        while (path[i] == '/')
            i++;
        if (path[i] == '\0')
            break;

        j = i;
        while (path[j] != '\0' && path[j] != '/')
            j++;

        clen = j - i;
        comp = malloc(clen + 1);
        if (comp == NULL) {
            free(out);
            return NULL;
        }
        memcpy(comp, &path[i], clen);
        comp[clen] = '\0';
        i = j;

        if (strcmp(comp, ".") == 0) {
            free(comp);
            continue;
        }

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
            free(comp);
            continue;
        }

        uint64_t olen = strlen(out);

        if (olen > 0 && out[olen - 1] != '/')
            strcat(out, "/");
        strcat(out, comp);
        free(comp);
    }

    return out;
}

/* Return the mount-relative path when abs is /fat or below it, else NULL. The
 * caller frees it. */
static char *fat_rel(const char *abs)
{
    uint64_t ml = strlen(VFS_MOUNT_FAT);
    const char *rel;

    if (strcmp(abs, VFS_MOUNT_FAT) == 0)
        rel = "/";
    else if (strncmp(abs, VFS_MOUNT_FAT, ml) == 0 && abs[ml] == '/')
        rel = abs + ml;
    else
        return NULL;

    return strdup(rel);
}

/* Split "cwd\0path\0" read from the request buffer. Allocates both strings;
 * the caller frees them. Returns false on out of memory. */
static bool split_cwd_path(const uint8_t *buf, uint64_t buflen,
                           char **cwd_out, char **path_out)
{
    uint64_t i = 0, j;
    char *cwd;

    while (i < buflen && buf[i] != '\0')
        i++;
    cwd = malloc(i + 1);
    if (cwd == NULL)
        return false;
    memcpy(cwd, buf, i);
    cwd[i] = '\0';
    *cwd_out = cwd;

    if (i >= buflen) {
        *path_out = strdup("");
        return *path_out != NULL;
    }
    i++;
    j = i;
    while (j < buflen && buf[j] != '\0')
        j++;
    *path_out = malloc(j - i + 1);
    if (*path_out == NULL) {
        free(cwd);
        return false;
    }
    memcpy(*path_out, buf + i, j - i);
    (*path_out)[j - i] = '\0';
    return true;
}

/* Write a redirect path at the start of the request buffer, bounded by the
 * path area. */
static void buf_write_path(uint8_t *buf, const char *path)
{
    uint64_t n = strlen(path);

    if (n >= VFS_IO_DATA_OFF)
        n = VFS_IO_DATA_OFF - 1;
    memcpy(buf, path, n);
    buf[n] = '\0';
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
    int32_t i = 0;

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

static bool idx_is_dyn(int32_t i)
{
    return i >= ent_count;
}

static bool idx_is_dir(int32_t i)
{
    if (i == ROOT_INDEX)
        return true;
    if (idx_is_dyn(i))
        return dyns[i - ent_count].is_dir;
    return ents[i].is_dir;
}

static bool idx_is_symlink(int32_t i)
{
    return idx_is_dyn(i) && dyns[i - ent_count].is_symlink;
}

static uint64_t idx_size(int32_t i)
{
    if (idx_is_dyn(i)) {
        vfs_dyn_t *d = &dyns[i - ent_count];

        if (d->is_symlink)
            return (d->target != NULL) ? strlen(d->target) : 0;
        return d->size;
    }
    return ents[i].size;
}

static const char *idx_name(int32_t i)
{
    if (idx_is_dyn(i))
        return dyns[i - ent_count].name;
    return ents[i].name;
}

/* Resolve an absolute path to an index, ROOT_INDEX for the root, or -1. */
static int32_t resolve(const char *path)
{
    while (*path == '/')
        path++;

    if (*path == '\0')
        return ROOT_INDEX;

    for (int32_t i = 0; i < ent_count; i++) {
        if (ents[i].deleted)
            continue;
        if (strcmp(ents[i].name, path) == 0)
            return i;
    }

    for (int32_t i = 0; i < VFS_MAX_DYN; i++) {
        if (dyns[i].used && strcmp(dyns[i].name, path) == 0)
            return ent_count + i;
    }
    return -1;
}

/* Allocate a runtime entry named path (already normalized). Returns the index
 * or -1. */
static int32_t dyn_alloc(const char *path)
{
    for (int32_t i = 0; i < VFS_MAX_DYN; i++) {
        if (dyns[i].used)
            continue;

        memset(&dyns[i], 0, sizeof(dyns[i]));
        dyns[i].used = true;
        dyns[i].name = strdup(path);
        if (dyns[i].name == NULL) {
            dyns[i].used = false;
            return -1;
        }
        return ent_count + i;
    }
    return -1;
}

/* Create a runtime file (path already normalized). Returns the index or -1. */
static int32_t dyn_create(const char *path)
{
    int32_t idx = dyn_alloc(path);

    if (idx < 0)
        return -1;

    vfs_dyn_t *d = &dyns[idx - ent_count];

    d->data = (char *) sys_malloc(VFS_DYN_CAP);
    if (d->data == NULL) {
        free(d->name);
        d->used = false;
        return -1;
    }
    d->cap = VFS_DYN_CAP;
    return idx;
}

static int32_t dyn_create_dir(const char *path)
{
    int32_t idx = dyn_alloc(path);

    if (idx >= 0)
        dyns[idx - ent_count].is_dir = true;
    return idx;
}

static int32_t dyn_create_symlink(const char *path, const char *target)
{
    int32_t idx = dyn_alloc(path);

    if (idx < 0)
        return -1;

    vfs_dyn_t *d = &dyns[idx - ent_count];

    d->is_symlink = true;
    d->target = strdup(target);
    if (d->target == NULL) {
        free(d->name);
        d->used = false;
        return -1;
    }
    return idx;
}

/* Fill a stat for a resolved index. */
static void fill_stat(int32_t e, void *out)
{
    stat_t *st = (stat_t *) out;

    memset(st, 0, sizeof(*st));
    st->st_nlink = 1;

    if (idx_is_dir(e)) {
        st->st_mode = S_IFDIR | 0755;
        st->st_ino = (e == ROOT_INDEX) ? 1 : (uint64_t) e + 1;
    } else if (idx_is_symlink(e)) {
        st->st_mode = S_IFLNK | 0777;
        st->st_ino = (uint64_t) e + 1;
        st->st_size = (int64_t) idx_size(e);
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

static int32_t fd_alloc(void)
{
    for (int32_t i = 0; i < VFS_MAX_FDS; i++) {
        if (!fds[i].in_use) {
            memset(&fds[i], 0, sizeof(fds[i]));
            fds[i].in_use = true;
            fds[i].refs = 1;
            return i + 1;
        }
    }
    return -1;
}

/* Read the next NUL-terminated string from buf at *pos. Advances *pos. The
 * caller frees the result. */
static char *buf_read_str(const uint8_t *buf, uint64_t buflen, uint64_t * pos)
{
    uint64_t start = *pos;
    uint64_t i = start;

    while (i < buflen && buf[i] != '\0')
        i++;

    char *out = malloc(i - start + 1);

    if (out == NULL)
        return NULL;
    memcpy(out, buf + start, i - start);
    out[i - start] = '\0';
    *pos = (i < buflen) ? i + 1 : i;
    return out;
}

static void handle(sys_ipc_msg_t * m, sys_ipc_msg_t * rep)
{
    if (m->tag == VFS_PING) {
        rep->words[0] = VFS_PONG;
        return;
    }

    if (m->tag == VFS_FACCESSAT) {
        char *cwd = NULL, *path = NULL, *abs = NULL, *rel = NULL;
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;
        uint8_t *buf = (memh != 0) ? map_buf(memh) : NULL;

        if (buf == NULL) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -5;    /* -EIO */
            return;
        }

        if (split_cwd_path(buf, VFS_IO_DATA_OFF, &cwd, &path)) {
            abs = join_path(cwd, path);
            rel = (abs != NULL) ? fat_rel(abs) : NULL;

            if (rel != NULL) {
                buf_write_path(buf, rel);
                rep->words[0] = VFS_REDIRECT_FAT;
            } else if (abs != NULL) {
                rep->words[0] =
                    (resolve(abs) != -1) ? 0 : (uint64_t) (int64_t) -2;
            } else {
                rep->words[0] = (uint64_t) (int64_t) -12;   /* -ENOMEM */
            }
        } else {
            rep->words[0] = (uint64_t) (int64_t) -12;       /* -ENOMEM */
        }

        free(cwd);
        free(path);
        free(abs);
        free(rel);
        sys_mem_unmap(memh, VFS_BUF_VADDR);
        sys_handle_close(memh);
        return;
    }

    if (m->tag == VFS_FSTATAT) {
        char *cwd = NULL, *path = NULL, *abs = NULL, *rel = NULL;
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;
        uint8_t *buf = (memh != 0) ? map_buf(memh) : NULL;

        if (buf == NULL) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -5;    /* -EIO */
            return;
        }

        if (split_cwd_path(buf, VFS_IO_DATA_OFF, &cwd, &path)) {
            abs = join_path(cwd, path);
            rel = (abs != NULL) ? fat_rel(abs) : NULL;

            if (rel != NULL) {
                buf_write_path(buf, rel);
                rep->words[0] = VFS_REDIRECT_FAT;
            } else if (abs == NULL) {
                rep->words[0] = (uint64_t) (int64_t) -12;   /* -ENOMEM */
            } else {
                int32_t e = resolve(abs);

                if (e == -1) {
                    rep->words[0] = (uint64_t) (int64_t) -2;    /* -ENOENT */
                } else {
                    fill_stat(e, buf + VFS_IO_DATA_OFF);
                    rep->words[0] = 0;
                }
            }
        } else {
            rep->words[0] = (uint64_t) (int64_t) -12;       /* -ENOMEM */
        }

        free(cwd);
        free(path);
        free(abs);
        free(rel);
        sys_mem_unmap(memh, VFS_BUF_VADDR);
        sys_handle_close(memh);
        return;
    }

    if (m->tag == VFS_FSTAT) {
        int32_t fd = (int32_t) m->words[0];
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
        char *cwd = NULL, *path = NULL, *abs = NULL, *rel = NULL;
        uint64_t flags = m->words[0];
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;
        uint8_t *buf = (memh != 0) ? map_buf(memh) : NULL;
        int32_t e;

        if (buf == NULL) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -5;    /* -EIO */
            return;
        }

        if (!split_cwd_path(buf, VFS_IO_DATA_OFF, &cwd, &path)) {
            rep->words[0] = (uint64_t) (int64_t) -12;   /* -ENOMEM */
            goto openat_out;
        }

        abs = join_path(cwd, path);
        if (abs == NULL) {
            rep->words[0] = (uint64_t) (int64_t) -12;
            goto openat_out;
        }

        rel = fat_rel(abs);

        if (rel != NULL) {
            buf_write_path(buf, rel);
            rep->words[0] = VFS_REDIRECT_FAT;
            goto openat_out;
        }

        e = resolve(abs);

        if (e == -1 && (flags & O_CREAT)) {
            char *norm = norm_dir(abs);

            if (norm != NULL && norm[0] != '\0')
                e = dyn_create(norm);
            free(norm);
        }

        if (e == -1) {
            rep->words[0] = (uint64_t) (int64_t) -2;    /* -ENOENT */
            goto openat_out;
        }

        int32_t fd = fd_alloc();
        if (fd < 0) {
            rep->words[0] = (uint64_t) (int64_t) -24;   /* -EMFILE */
            goto openat_out;
        }

        if (idx_is_dir(e)) {
            fds[fd - 1].is_dir = true;
            fds[fd - 1].dir = norm_dir(abs);
        } else {
            fds[fd - 1].ent = e;
        }

        rep->words[0] = 0;
        rep->words[1] = (uint64_t) fd;
        rep->words[2] = idx_is_dir(e) ? 0 : idx_size(e);

      openat_out:
        free(cwd);
        free(path);
        free(abs);
        free(rel);
        sys_mem_unmap(memh, VFS_BUF_VADDR);
        sys_handle_close(memh);
        return;
    }

    if (m->tag == VFS_READ) {
        int32_t fd = (int32_t) m->words[0];
        uint64_t len = m->words[1];
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;

        if (fd < 1 || fd > VFS_MAX_FDS || !fds[fd - 1].in_use
            || fds[fd - 1].is_dir) {
            rep->words[0] = (uint64_t) (int64_t) -9;    /* -EBADF */
        } else {
            int32_t e = fds[fd - 1].ent;
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

                memcpy(buf, src, n);
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
        int32_t fd = (int32_t) m->words[0];
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

                memcpy(d->data + off, src, n);
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
        int32_t fd = (int32_t) m->words[0];
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
        int32_t found = -1;

        for (int32_t i = 0; i < ent_count && found < 0; i++) {
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
        for (int32_t i = 0; i < VFS_MAX_DYN && found < 0; i++) {
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
            int32_t k = 0;
            while (base[k] != '\0' && k < (int32_t) sizeof(de->d_name) - 1) {
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
        int32_t fd = (int32_t) m->words[0];

        if (fd >= 1 && fd <= VFS_MAX_FDS && fds[fd - 1].in_use) {
            if (fds[fd - 1].refs > 0)
                fds[fd - 1].refs--;
            if (fds[fd - 1].refs == 0) {
                if (fds[fd - 1].dir != NULL)
                    free(fds[fd - 1].dir);
                fds[fd - 1].in_use = false;
            }
            rep->words[0] = 0;
        } else {
            rep->words[0] = (uint64_t) (int64_t) -9;
        }
        return;
    }

    if (m->tag == VFS_FD_FORK) {
        int32_t fd = (int32_t) m->words[0];

        if (fd >= 1 && fd <= VFS_MAX_FDS && fds[fd - 1].in_use) {
            fds[fd - 1].refs++;
            rep->words[0] = 0;
        } else {
            rep->words[0] = (uint64_t) (int64_t) -9;
        }
        return;
    }

    if (m->tag == VFS_SEEK) {
        int32_t fd = (int32_t) m->words[0];
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
        char *cwd = NULL, *path = NULL, *abs = NULL, *rel = NULL;
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;
        uint8_t *buf = (memh != 0) ? map_buf(memh) : NULL;

        if (buf == NULL) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -5;    /* -EIO */
            return;
        }

        if (split_cwd_path(buf, VFS_IO_DATA_OFF, &cwd, &path)) {
            abs = join_path(cwd, path);
            rel = (abs != NULL) ? fat_rel(abs) : NULL;

            if (rel != NULL) {
                buf_write_path(buf, rel);
                rep->words[0] = VFS_REDIRECT_FAT;
            } else if (abs != NULL) {
                int32_t e = resolve(abs);

                if (e == -1 || idx_is_dir(e)) {
                    rep->words[0] = (uint64_t) (int64_t) -2;    /* -ENOENT */
                } else if (idx_is_dyn(e)) {
                    dyns[e - ent_count].used = false;
                    rep->words[0] = 0;
                } else {
                    ents[e].deleted = true;
                    rep->words[0] = 0;
                }
            } else {
                rep->words[0] = (uint64_t) (int64_t) -12;   /* -ENOMEM */
            }
        } else {
            rep->words[0] = (uint64_t) (int64_t) -12;       /* -ENOMEM */
        }

        free(cwd);
        free(path);
        free(abs);
        free(rel);
        sys_mem_unmap(memh, VFS_BUF_VADDR);
        sys_handle_close(memh);
        return;
    }

    if (m->tag == VFS_MKDIRAT) {
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;
        uint8_t *buf = (memh != 0) ? map_buf(memh) : NULL;
        uint64_t pos = 0;
        char *cwd = NULL, *path = NULL, *abs = NULL, *rel = NULL, *norm = NULL;

        if (buf == NULL) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -5;    /* -EIO */
            return;
        }

        cwd = buf_read_str(buf, VFS_IO_DATA_OFF, &pos);
        path = buf_read_str(buf, VFS_IO_DATA_OFF, &pos);

        if (cwd != NULL && path != NULL) {
            abs = join_path(cwd, path);
            rel = (abs != NULL) ? fat_rel(abs) : NULL;

            if (rel != NULL)
                rep->words[0] = VFS_REDIRECT_FAT;
            else if (abs == NULL)
                rep->words[0] = (uint64_t) (int64_t) -12;
            else if (resolve(abs) != -1)
                rep->words[0] = (uint64_t) (int64_t) -17;   /* -EEXIST */
            else {
                norm = norm_dir(abs);
                if (norm == NULL || norm[0] == '\0')
                    rep->words[0] = (uint64_t) (int64_t) -22;
                else if (dyn_create_dir(norm) < 0)
                    rep->words[0] = (uint64_t) (int64_t) -28;       /* -ENOSPC */
                else
                    rep->words[0] = 0;
            }
        } else {
            rep->words[0] = (uint64_t) (int64_t) -12;
        }

        free(cwd);
        free(path);
        free(abs);
        free(rel);
        free(norm);
        sys_mem_unmap(memh, VFS_BUF_VADDR);
        sys_handle_close(memh);
        return;
    }

    if (m->tag == VFS_SYMLINKAT) {
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;
        uint8_t *buf = (memh != 0) ? map_buf(memh) : NULL;
        uint64_t pos = 0;
        char *cwd = NULL, *path = NULL, *target = NULL;
        char *abs = NULL, *rel = NULL, *norm = NULL;

        if (buf == NULL) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -5;
            return;
        }

        cwd = buf_read_str(buf, VFS_IO_DATA_OFF, &pos);
        path = buf_read_str(buf, VFS_IO_DATA_OFF, &pos);
        target = buf_read_str(buf, VFS_IO_DATA_OFF, &pos);

        if (cwd != NULL && path != NULL && target != NULL) {
            abs = join_path(cwd, path);
            rel = (abs != NULL) ? fat_rel(abs) : NULL;

            if (rel != NULL)
                rep->words[0] = VFS_REDIRECT_FAT;
            else if (abs == NULL)
                rep->words[0] = (uint64_t) (int64_t) -12;
            else if (resolve(abs) != -1)
                rep->words[0] = (uint64_t) (int64_t) -17;
            else {
                norm = norm_dir(abs);
                if (norm == NULL || norm[0] == '\0')
                    rep->words[0] = (uint64_t) (int64_t) -22;
                else if (dyn_create_symlink(norm, target) < 0)
                    rep->words[0] = (uint64_t) (int64_t) -28;
                else
                    rep->words[0] = 0;
            }
        } else {
            rep->words[0] = (uint64_t) (int64_t) -12;
        }

        free(cwd);
        free(path);
        free(target);
        free(abs);
        free(rel);
        free(norm);
        sys_mem_unmap(memh, VFS_BUF_VADDR);
        sys_handle_close(memh);
        return;
    }

    if (m->tag == VFS_RENAMEAT) {
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;
        uint8_t *buf = (memh != 0) ? map_buf(memh) : NULL;
        uint64_t pos = 0;
        char *cwd = NULL, *oldp = NULL, *newp = NULL;
        char *oldabs = NULL, *newabs = NULL, *norm = NULL;

        if (buf == NULL) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -5;
            return;
        }

        cwd = buf_read_str(buf, VFS_IO_DATA_OFF, &pos);
        oldp = buf_read_str(buf, VFS_IO_DATA_OFF, &pos);
        newp = buf_read_str(buf, VFS_IO_DATA_OFF, &pos);

        if (cwd != NULL && oldp != NULL && newp != NULL) {
            oldabs = join_path(cwd, oldp);
            newabs = join_path(cwd, newp);

            int32_t e = (oldabs != NULL) ? resolve(oldabs) : -1;

            if ((oldabs != NULL && fat_rel(oldabs) != NULL)
                || (newabs != NULL && fat_rel(newabs) != NULL))
                rep->words[0] = VFS_REDIRECT_FAT;
            else if (oldabs == NULL || newabs == NULL)
                rep->words[0] = (uint64_t) (int64_t) -12;
            else if (e == -1)
                rep->words[0] = (uint64_t) (int64_t) -2;    /* -ENOENT */
            else if (!idx_is_dyn(e))
                rep->words[0] = (uint64_t) (int64_t) -30;   /* -EROFS */
            else if (resolve(newabs) != -1)
                rep->words[0] = (uint64_t) (int64_t) -17;   /* -EEXIST */
            else {
                norm = norm_dir(newabs);
                if (norm == NULL || norm[0] == '\0') {
                    rep->words[0] = (uint64_t) (int64_t) -22;
                } else {
                    vfs_dyn_t *d = &dyns[e - ent_count];
                    uint64_t oldlen = strlen(d->name);

                    /* Update children of a renamed directory. */
                    for (int32_t i = 0; i < VFS_MAX_DYN; i++) {
                        if (!dyns[i].used || &dyns[i] == d)
                            continue;
                        if (strncmp(dyns[i].name, d->name, oldlen) == 0
                            && dyns[i].name[oldlen] == '/') {
                            char *suffix = strdup(dyns[i].name + oldlen);
                            char *repl = NULL;

                            if (suffix != NULL) {
                                uint64_t rl = strlen(norm) + strlen(suffix) + 1;

                                repl = malloc(rl);
                                if (repl != NULL) {
                                    strcpy(repl, norm);
                                    strcat(repl, suffix);
                                }
                                free(suffix);
                            }
                            if (repl != NULL) {
                                free(dyns[i].name);
                                dyns[i].name = repl;
                            }
                        }
                    }

                    free(d->name);
                    d->name = strdup(norm);
                    rep->words[0] = (d->name != NULL) ? 0
                        : (uint64_t) (int64_t) -12;
                }
            }
        } else {
            rep->words[0] = (uint64_t) (int64_t) -12;
        }

        free(cwd);
        free(oldp);
        free(newp);
        free(oldabs);
        free(newabs);
        free(norm);
        sys_mem_unmap(memh, VFS_BUF_VADDR);
        sys_handle_close(memh);
        return;
    }

    if (m->tag == VFS_READLINK) {
        int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;
        uint8_t *buf = (memh != 0) ? map_buf(memh) : NULL;
        uint64_t pos = 0;
        char *cwd = NULL, *path = NULL, *abs = NULL;

        if (buf == NULL) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -5;
            return;
        }

        cwd = buf_read_str(buf, VFS_IO_DATA_OFF, &pos);
        path = buf_read_str(buf, VFS_IO_DATA_OFF, &pos);

        if (cwd != NULL && path != NULL) {
            abs = join_path(cwd, path);

            int32_t e = (abs != NULL) ? resolve(abs) : -1;

            if (e == -1)
                rep->words[0] = (uint64_t) (int64_t) -2;
            else if (!idx_is_symlink(e))
                rep->words[0] = (uint64_t) (int64_t) -22;   /* -EINVAL */
            else {
                const char *t = dyns[e - ent_count].target;
                uint64_t n = strlen(t);

                if (n > VFS_IO_DATA_OFF - 1)
                    n = VFS_IO_DATA_OFF - 1;
                memcpy(buf + VFS_IO_DATA_OFF, t, n);
                buf[VFS_IO_DATA_OFF + n] = '\0';
                rep->words[0] = 0;
                rep->words[1] = n;
            }
        } else {
            rep->words[0] = (uint64_t) (int64_t) -12;
        }

        free(cwd);
        free(path);
        free(abs);
        sys_mem_unmap(memh, VFS_BUF_VADDR);
        sys_handle_close(memh);
        return;
    }

    rep->words[0] = (uint64_t) (int64_t) -38;   /* -ENOSYS */
}

int32_t main(void)
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
