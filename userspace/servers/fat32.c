/**-----------------------------------------------------------------------------

 @file    fat32.c
 @brief   Userspace FAT32 server

 @details
 @verbatim

   A FAT32 client of the block server. It mounts partition 0, parses the BPB,
   walks the directory tree (8.3 names and long names) and reads or writes a
   file's cluster chain. A request carries the path at offset 0 of a shared
   buffer; the server writes file data at FAT_DATA_OFF.

   Write support covers create, write, truncate and unlink. New names use the
   8.3 form; long names remain readable. The FAT copies are kept in sync.

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
#include <time.h>

#define SEC             512
#define FAT_BLK_ADDR    0x20000000      /* block I/O memory-object mapping */
#define FAT_REQ_ADDR    0x21000000      /* request/result buffer mapping */
#define FAT_DATA_OFF    4096
#define FAT_DATA_MAX    4096
#define FAT_PART_INDEX  0
#define FAT_EOC         0x0FFFFFFFu
#define FAT_FD_MAX      32
#define LFN_CHARS_PER_ENTRY 13
#define LFN_MAX_ENTRIES     20

typedef struct[[gnu::packed]] {
    uint8_t jmp[3];
    uint8_t oem[8];
    uint16_t bytes_per_sector;
    uint8_t sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t table_count;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t media;
    uint16_t table_size_16;
    uint16_t spt;
    uint16_t heads;
    uint32_t hidden;
    uint32_t total_sectors_32;
    uint8_t ext[54];
} fat_bs_t;

typedef struct[[gnu::packed]] {
    uint8_t name[11];
    uint8_t attr;
    uint8_t nt;
    uint8_t ct_tenths;
    uint16_t ct;
    uint16_t cd;
    uint16_t ad;
    uint16_t cluster_hi;
    uint16_t mt;
    uint16_t md;
    uint16_t cluster_lo;
    uint32_t size;
} fat_dirent_t;

/* A long-file-name entry: 13 UTF-16 characters in three pieces. */
typedef struct[[gnu::packed]] {
    uint8_t seq;                /* 1..N; bit 0x40 set on the first physical */
    uint16_t name1[5];
    uint8_t attr;               /* 0x0F */
    uint8_t type;
    uint8_t checksum;
    uint16_t name2[6];
    uint16_t first_cluster;
    uint16_t name3[2];
} fat_lfn_t;

/** @brief Location of a directory entry on the volume */
typedef struct {
    uint32_t lba;               /* sector holding the entry */
    uint16_t off;               /* byte offset inside the sector */
} dent_loc_t;

static bootinfo_t bi;
static uint16_t bps;
static uint8_t spc;
static uint32_t part_lba;
static uint32_t fat_lba;
static uint32_t table_size;
static uint8_t table_count;
static uint32_t data_lba;
static uint32_t root_cluster;
static uint32_t total_sectors;
static uint32_t max_cluster;
static uint32_t alloc_hint;
static bool mounted;
static int32_t mount_err = -100;

/* Single-threaded server: one cluster-sized scratch buffer. */
static uint8_t cluster_buf[SEC * 128];

static uint32_t rd32(const uint8_t *p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8)
        | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static void wr32(uint8_t *p, uint32_t v)
{
    p[0] = (uint8_t) v;
    p[1] = (uint8_t) (v >> 8);
    p[2] = (uint8_t) (v >> 16);
    p[3] = (uint8_t) (v >> 24);
}

/* Read `count` sectors relative to the FAT volume through the block server. */
static int32_t blk_read(uint32_t lba, uint8_t count, uint8_t *dst)
{
    return blk_client_read((int64_t) bi.block_ep, part_lba + lba, count, dst,
                           FAT_BLK_ADDR);
}

/* Write `count` sectors relative to the FAT volume through the block server. */
static int32_t blk_write(uint32_t lba, uint8_t count, const uint8_t *src)
{
    return blk_client_write((int64_t) bi.block_ep, part_lba + lba, count, src,
                            FAT_BLK_ADDR);
}

static int32_t blk_get_part(uint32_t idx, uint32_t *start, uint32_t *count)
{
    int64_t reply = sys_ep_create();
    if (reply < 0)
        return -1;

    int64_t reply_send = sys_handle_dup(reply);
    if (reply_send < 0) {
        sys_handle_close(reply);
        return -1;
    }

    sys_ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.tag = BLOCK_GET_PART;
    m.words[0] = idx;
    m.xfer[0] = reply_send;
    m.xfer_count = 1;

    if (sys_ipc_send((int64_t) bi.block_ep, &m) != 0) {
        sys_handle_close(reply_send);
        sys_handle_close(reply);
        return -1;
    }
    sys_handle_close(reply_send);

    sys_ipc_msg_t rep;
    int32_t rc = -1;
    if (sys_ipc_recv_timeout(reply, &rep, 2000) == 0
        && (int64_t) rep.words[0] == 0) {
        *start = (uint32_t) rep.words[1];
        *count = (uint32_t) rep.words[2];
        rc = 0;
    }

    sys_handle_close(reply);
    return rc;
}

static uint32_t cluster_lba(uint32_t cluster)
{
    return data_lba + (cluster - 2) * (uint32_t) spc;
}

static uint32_t fat_raw(uint32_t cluster)
{
    uint32_t off = cluster * 4;
    uint8_t sec[SEC];

    if (blk_read(fat_lba + off / SEC, 1, sec) != 0)
        return 0;
    return rd32(sec + (off % SEC)) & 0x0FFFFFFF;
}

static uint32_t fat_next(uint32_t cluster)
{
    uint32_t v = fat_raw(cluster);

    return (v >= 0x0FFFFFF8 || v == 0) ? 0 : v;
}

/* Write one FAT entry to every FAT copy. */
static int32_t fat_set(uint32_t cluster, uint32_t value)
{
    uint32_t off = cluster * 4;
    uint32_t secno = off / SEC;
    uint32_t idx = (off % SEC) / 4;
    uint8_t sec[SEC];

    for (uint8_t t = 0; t < table_count; t++) {
        uint32_t lba = fat_lba + (uint32_t) t * table_size + secno;
        uint32_t old;

        if (blk_read(lba, 1, sec) != 0)
            return -1;
        old = rd32(sec + idx * 4);
        wr32(sec + idx * 4, (old & 0xF0000000u) | (value & 0x0FFFFFFFu));
        if (blk_write(lba, 1, sec) != 0)
            return -1;
    }
    return 0;
}

static int32_t cluster_zero(uint32_t cluster)
{
    memset(cluster_buf, 0, (uint64_t) spc * SEC);
    return blk_write(cluster_lba(cluster), spc, cluster_buf);
}

/* Find a free cluster, mark it EOC and clear its data. */
static uint32_t cluster_alloc(void)
{
    uint32_t start = (alloc_hint >= 2 && alloc_hint <= max_cluster)
        ? alloc_hint : 2;
    uint32_t first_sec = (start * 4) / SEC;
    uint32_t last_sec = (max_cluster * 4) / SEC;
    uint8_t sec[SEC];
    bool wrapped = false;

again:
    for (uint32_t s = first_sec; s <= last_sec; s++) {
        if (blk_read(fat_lba + s, 1, sec) != 0)
            return 0;

        for (uint32_t i = 0; i < SEC / 4; i++) {
            uint32_t c = s * (SEC / 4) + i;

            if (c < 2 || c > max_cluster || c < start)
                continue;
            if ((rd32(sec + i * 4) & 0x0FFFFFFFu) != 0)
                continue;
            if (fat_set(c, FAT_EOC) != 0)
                return 0;
            if (cluster_zero(c) != 0)
                return 0;
            alloc_hint = c + 1;
            return c;
        }
    }

    if (!wrapped && start > 2) {
        wrapped = true;
        start = 2;
        first_sec = 0;
        goto again;
    }
    return 0;
}

static void free_chain(uint32_t cluster)
{
    while (cluster >= 2 && cluster <= max_cluster) {
        uint32_t next = fat_next(cluster);

        fat_set(cluster, 0);
        cluster = next;
    }
}

static uint32_t chain_at(uint32_t cluster, uint32_t idx)
{
    while (cluster >= 2 && idx > 0) {
        cluster = fat_next(cluster);
        idx--;
    }
    return (cluster >= 2) ? cluster : 0;
}

/* Return the cluster at `idx`, allocating the chain as needed. */
static uint32_t chain_ensure(uint32_t *first, uint32_t idx)
{
    uint32_t c;

    if (*first < 2) {
        c = cluster_alloc();
        if (c == 0)
            return 0;
        *first = c;
    }

    c = *first;
    while (idx > 0) {
        uint32_t next = fat_next(c);

        if (next < 2) {
            next = cluster_alloc();
            if (next == 0)
                return 0;
            if (fat_set(c, next) != 0)
                return 0;
        }
        c = next;
        idx--;
    }
    return c;
}

static int32_t dirent_write(const dent_loc_t *loc, const fat_dirent_t *e)
{
    uint8_t sec[SEC];

    if (blk_read(loc->lba, 1, sec) != 0)
        return -1;
    memcpy(sec + loc->off, e, sizeof(*e));
    return blk_write(loc->lba, 1, sec);
}

static void dirent_set_cluster(fat_dirent_t *e, uint32_t cluster)
{
    e->cluster_lo = (uint16_t) (cluster & 0xFFFF);
    e->cluster_hi = (uint16_t) (cluster >> 16);
}

static uint32_t dirent_cluster(const fat_dirent_t *e)
{
    return (uint32_t) e->cluster_lo | ((uint32_t) e->cluster_hi << 16);
}

static int32_t strcasecmp_ascii(const char *a, const char *b)
{
    for (;;) {
        uint8_t ca = (uint8_t) *a++;
        uint8_t cb = (uint8_t) *b++;

        if (ca >= 'a' && ca <= 'z')
            ca -= 32;
        if (cb >= 'a' && cb <= 'z')
            cb -= 32;
        if (ca != cb)
            return (int32_t) ca - (int32_t) cb;
        if (ca == 0)
            return 0;
    }
}

static void fmt_83(const uint8_t name[11], char *out)
{
    int32_t o = 0;

    for (int32_t i = 0; i < 8 && name[i] != ' '; i++)
        out[o++] = (char) name[i];
    if (name[8] != ' ') {
        out[o++] = '.';
        for (int32_t i = 8; i < 11 && name[i] != ' '; i++)
            out[o++] = (char) name[i];
    }
    out[o] = '\0';
}

static bool valid_83_char(uint8_t c)
{
    if (c < 0x20)
        return false;
    if (strchr("\"*+,/:;<=>?[\\]|", c) != NULL)
        return false;
    return true;
}

/* Convert a name to the 11-byte 8.3 form. Long names are rejected. */
static int32_t parse_83(const char *name, uint8_t out[11])
{
    const char *dot = NULL;
    uint64_t n = strlen(name);
    uint64_t base_len;
    uint64_t ext_len = 0;

    if (n == 0 || n > 12 || name[0] == '.')
        return -36;             /* -ENAMETOOLONG */

    for (uint64_t i = 0; i < n; i++) {
        if (name[i] == '.') {
            if (dot != NULL)
                return -36;
            dot = name + i;
        }
    }

    base_len = (dot != NULL) ? (uint64_t) (dot - name) : n;
    if (dot != NULL)
        ext_len = n - base_len - 1;

    if (base_len == 0 || base_len > 8 || ext_len > 3)
        return -36;

    memset(out, ' ', 11);
    for (uint64_t i = 0; i < base_len; i++) {
        uint8_t c = (uint8_t) name[i];

        if (!valid_83_char(c))
            return -22;
        if (c >= 'a' && c <= 'z')
            c -= 32;
        out[i] = c;
    }
    for (uint64_t i = 0; i < ext_len; i++) {
        uint8_t c = (uint8_t) dot[1 + i];

        if (!valid_83_char(c))
            return -22;
        if (c >= 'a' && c <= 'z')
            c -= 32;
        out[8 + i] = c;
    }
    return 0;
}

/* Scan a directory. With `want` non-NULL, match the effective name (the long
 * name if present, else the 8.3 name); otherwise return the `index`-th live
 * entry. Fills out/out_name and returns 0, or -1. */
static int32_t dir_scan(uint32_t dir_cluster, const char *want, uint64_t index,
                        fat_dirent_t *out, char *out_name,
                        dent_loc_t *out_loc, dent_loc_t *lfn_loc,
                        uint32_t *lfn_count)
{
    uint16_t lfn[LFN_MAX_ENTRIES * LFN_CHARS_PER_ENTRY];
    int32_t lfn_len = 0;
    uint32_t cluster = dir_cluster;
    uint64_t seen = 0;
    dent_loc_t pending_loc = { 0, 0 };
    uint32_t pending_count = 0;

    while (cluster >= 2) {
        uint32_t base_lba = cluster_lba(cluster);

        if (blk_read(base_lba, spc, cluster_buf) != 0)
            return -1;

        int32_t entries = (int32_t) spc * SEC / 32;
        for (int32_t i = 0; i < entries; i++) {
            fat_dirent_t *e = (fat_dirent_t *) (cluster_buf + i * 32);
            dent_loc_t loc = {
                .lba = base_lba + (uint32_t) ((i * 32) / SEC),
                .off = (uint16_t) ((i * 32) % SEC),
            };

            if (e->name[0] == 0x00)
                return -1;      /* end of directory */
            if (e->name[0] == 0xE5) {
                lfn_len = 0;    /* deleted: drop a pending long name */
                pending_count = 0;
                continue;
            }
            if (e->attr == 0x0F) {
                fat_lfn_t *l = (fat_lfn_t *) e;
                int32_t base = ((l->seq & 0x1F) - 1) * LFN_CHARS_PER_ENTRY;
                uint16_t chars[LFN_CHARS_PER_ENTRY];

                if (pending_count == 0)
                    pending_loc = loc;
                pending_count++;

                if (base < 0
                    || base + LFN_CHARS_PER_ENTRY >
                    (int32_t) (sizeof(lfn) / sizeof(lfn[0])))
                    continue;

                memcpy(chars + 0, l->name1, 10);
                memcpy(chars + 5, l->name2, 12);
                memcpy(chars + 11, l->name3, 4);
                for (int32_t c = 0; c < LFN_CHARS_PER_ENTRY; c++)
                    lfn[base + c] = chars[c];
                if (base + LFN_CHARS_PER_ENTRY > lfn_len)
                    lfn_len = base + LFN_CHARS_PER_ENTRY;
                continue;
            }
            if (e->attr & 0x08) {
                lfn_len = 0;    /* volume label */
                pending_count = 0;
                continue;
            }
            if (e->name[0] == '.') {
                lfn_len = 0;    /* . and .. */
                pending_count = 0;
                continue;
            }

            char nm[256];

            if (lfn_len > 0) {
                int32_t o = 0;

                for (int32_t k = 0; k < lfn_len && o < 255; k++) {
                    uint16_t u = lfn[k];

                    if (u == 0)
                        break;
                    nm[o++] = (u < 0x80) ? (char) u : '?';
                }
                nm[o] = '\0';
                if (o == 0)
                    fmt_83(e->name, nm);
            } else {
                fmt_83(e->name, nm);
            }

            if (want != NULL) {
                if (strcasecmp_ascii(nm, want) == 0) {
                    memcpy(out, e, sizeof(*out));
                    if (out_name != NULL)
                        memcpy(out_name, nm, strlen(nm) + 1);
                    if (out_loc != NULL)
                        *out_loc = loc;
                    if (lfn_loc != NULL)
                        *lfn_loc = pending_loc;
                    if (lfn_count != NULL)
                        *lfn_count = pending_count;
                    return 0;
                }
            } else {
                if (seen == index) {
                    memcpy(out, e, sizeof(*out));
                    if (out_name != NULL)
                        memcpy(out_name, nm, strlen(nm) + 1);
                    if (out_loc != NULL)
                        *out_loc = loc;
                    return 0;
                }
                seen++;
            }
            lfn_len = 0;
            pending_count = 0;
        }
        cluster = fat_next(cluster);
    }

    return -1;
}

static int32_t dir_lookup(uint32_t dir_cluster, const char *want,
                          fat_dirent_t *out, dent_loc_t *loc)
{
    return dir_scan(dir_cluster, want, 0, out, NULL, loc, NULL, NULL);
}

static int32_t dir_index(uint32_t dir_cluster, uint64_t index, fat_dirent_t *out,
                         char *out_name)
{
    return dir_scan(dir_cluster, NULL, index, out, out_name, NULL, NULL, NULL);
}

/* Walk an absolute path; returns 0 and fills cluster/size/is_dir. */
static int32_t find_path(const char *path, uint32_t *out_cluster,
                         uint32_t *out_size, bool *out_is_dir)
{
    uint32_t cluster = root_cluster;
    const char *p = path;

    while (*p == '/')
        p++;

    if (*p == '\0') {
        *out_cluster = root_cluster;
        *out_size = 0;
        *out_is_dir = true;
        return 0;
    }

    while (*p != '\0') {
        const char *start = p;
        while (*p != '\0' && *p != '/')
            p++;
        int32_t len = (int32_t) (p - start);
        while (*p == '/')
            p++;

        fat_dirent_t e;
        char comp[256];
        int32_t cl = (len < 255) ? len : 255;

        memcpy(comp, start, (uint64_t) cl);
        comp[cl] = '\0';
        if (dir_lookup(cluster, comp, &e, NULL) != 0)
            return -1;

        cluster = dirent_cluster(&e);
        if (*p == '\0') {
            *out_cluster = cluster;
            *out_size = e.size;
            *out_is_dir = (e.attr & 0x10) != 0;
            return 0;
        }
        if (!(e.attr & 0x10) || cluster < 2)
            return -1;
    }
    return -1;
}

/* Split an absolute path into its parent directory and final name. */
static int32_t split_parent(const char *path, char *parent, char *base)
{
    const char *p = path;
    const char *end;
    const char *last = NULL;

    while (*p == '/')
        p++;
    end = p + strlen(p);
    while (end > p && end[-1] == '/')
        end--;
    if (end == p)
        return -1;

    for (const char *q = p; q < end; q++)
        if (*q == '/')
            last = q;

    if (last == NULL) {
        strcpy(parent, "/");
        uint64_t n = (uint64_t) (end - p);

        if (n >= 256)
            return -1;
        memcpy(base, p, n);
        base[n] = '\0';
    } else {
        uint64_t n = (uint64_t) (last - path);

        if (n >= 256)
            return -1;
        memcpy(parent, path, n);
        parent[n] = '\0';
        if (parent[0] == '\0')
            strcpy(parent, "/");

        n = (uint64_t) (end - last - 1);
        if (n >= 256)
            return -1;
        memcpy(base, last + 1, n);
        base[n] = '\0';
    }
    return 0;
}

/* Find a free directory slot, growing the directory when needed. */
static int32_t dir_find_free(uint32_t dir_cluster, dent_loc_t *loc)
{
    uint32_t cluster = dir_cluster;
    uint32_t last = dir_cluster;

    while (cluster >= 2) {
        uint32_t base_lba = cluster_lba(cluster);

        if (blk_read(base_lba, spc, cluster_buf) != 0)
            return -1;

        int32_t entries = (int32_t) spc * SEC / 32;
        for (int32_t i = 0; i < entries; i++) {
            fat_dirent_t *e = (fat_dirent_t *) (cluster_buf + i * 32);

            if (e->name[0] == 0x00 || e->name[0] == 0xE5) {
                loc->lba = base_lba + (uint32_t) ((i * 32) / SEC);
                loc->off = (uint16_t) ((i * 32) % SEC);
                return 0;
            }
        }
        last = cluster;
        cluster = fat_next(cluster);
    }

    uint32_t c = cluster_alloc();

    if (c == 0 || fat_set(last, c) != 0)
        return -1;
    loc->lba = cluster_lba(c);
    loc->off = 0;
    return 0;
}

/* Copy [off, off+len) of a file (first cluster, size) into dst. */
static uint64_t read_from(uint32_t cluster, uint64_t off, uint64_t len,
                          uint64_t size, uint8_t *dst)
{
    if (off >= size)
        return 0;
    if (off + len > size)
        len = size - off;

    uint64_t skipped = 0;

    /* Skip whole clusters before the requested offset. */
    while (cluster >= 2 && off - skipped >= (uint64_t) spc * SEC) {
        skipped += (uint64_t) spc * SEC;
        cluster = fat_next(cluster);
    }

    uint64_t skip_in = off - skipped;
    uint64_t done = 0;

    while (done < len && cluster >= 2) {
        uint8_t *cb = cluster_buf;

        if (blk_read(cluster_lba(cluster), spc, cb) != 0)
            break;

        uint64_t avail = (uint64_t) spc * SEC - skip_in;
        uint64_t want = len - done;

        if (want > avail)
            want = avail;
        if (want == 0)
            break;

        memcpy(dst + done, cb + skip_in, want);
        done += want;
        skip_in = 0;
        cluster = fat_next(cluster);
    }

    return done;
}

static int32_t mount(void)
{
    uint8_t sec[SEC];
    uint32_t start = 0, count = 0;

    if (blk_get_part(FAT_PART_INDEX, &start, &count) != 0) {
        mount_err = -100;
        return -1;
    }
    part_lba = start;
    total_sectors = count;

    int32_t rc = blk_read(0, 1, sec);
    if (rc != 0) {
        mount_err = -100 + rc;
        return -1;
    }

    fat_bs_t *bs = (fat_bs_t *) sec;

    bps = bs->bytes_per_sector;
    if (bps != SEC) {
        mount_err = -120;
        return -1;
    }
    spc = bs->sectors_per_cluster;
    if (spc == 0) {
        mount_err = -130;
        return -1;
    }

    table_size = rd32(sec + 36);
    root_cluster = rd32(sec + 44);
    table_count = bs->table_count;

    if (bs->root_entry_count != 0 || bs->table_size_16 != 0
        || root_cluster < 2 || table_size == 0 || table_count == 0) {
        mount_err = -131;       /* not a FAT32 volume */
        return -1;
    }

    if (bs->total_sectors_32 != 0)
        total_sectors = bs->total_sectors_32;

    fat_lba = bs->reserved_sector_count;
    data_lba = fat_lba + (uint32_t) table_count * table_size;
    if (total_sectors <= data_lba) {
        mount_err = -132;
        return -1;
    }

    max_cluster = (total_sectors - data_lba) / (uint32_t) spc + 2;
    alloc_hint = 2;

    mounted = true;
    mount_err = 0;
    return 0;
}

/**
 * @brief One open file entry in the FAT32 server
 */
typedef struct {
    bool used;
    uint32_t cluster;
    uint32_t size;
    uint64_t off;
    bool is_dir;
    uint32_t dir_idx;
    dent_loc_t dirent;
} fat_fd_t;

static fat_fd_t fdtab[FAT_FD_MAX];

static int32_t fd_alloc(void)
{
    for (int32_t i = 0; i < FAT_FD_MAX; i++)
        if (!fdtab[i].used)
            return i;
    return -1;
}

static bool fd_ok(int32_t fd)
{
    return fd >= 0 && fd < FAT_FD_MAX && fdtab[fd].used;
}

/* Copy the request path into a fresh buffer. The caller frees it. */
static char *read_path(const uint8_t *buf)
{
    uint64_t i = 0;
    char *path;

    while (i < VFS_IO_DATA_OFF && buf[i] != '\0')
        i++;

    path = malloc(i + 1);
    if (path == NULL)
        return NULL;
    memcpy(path, buf, i);
    path[i] = '\0';
    return path;
}

static bool path_exists(const char *path)
{
    uint32_t cluster, size;
    bool is_dir;

    return find_path(path, &cluster, &size, &is_dir) == 0;
}

static int32_t open_file(const char *path, int32_t *out_fd, uint32_t *out_size)
{
    fat_dirent_t e;
    dent_loc_t loc;

    const char *root = path;

    while (*root == '/')
        root++;
    if (*root == '\0') {
        int32_t rfd = fd_alloc();

        if (rfd < 0)
            return -24;
        fdtab[rfd].used = true;
        fdtab[rfd].cluster = root_cluster;
        fdtab[rfd].size = 0;
        fdtab[rfd].off = 0;
        fdtab[rfd].is_dir = true;
        fdtab[rfd].dir_idx = 0;
        fdtab[rfd].dirent.lba = 0;
        if (out_fd != NULL)
            *out_fd = rfd;
        if (out_size != NULL)
            *out_size = 0;
        return 0;
    }

    char parent[256], base[256];

    if (split_parent(path, parent, base) != 0)
        return -2;

    uint32_t dir_cluster;
    bool pdir;
    uint32_t psize;

    if (find_path(parent, &dir_cluster, &psize, &pdir) != 0 || !pdir)
        return -2;
    if (dir_lookup(dir_cluster, base, &e, &loc) != 0)
        return -2;

    int32_t nfd = fd_alloc();

    if (nfd < 0)
        return -24;
    fdtab[nfd].used = true;
    fdtab[nfd].cluster = dirent_cluster(&e);
    fdtab[nfd].size = e.size;
    fdtab[nfd].off = 0;
    fdtab[nfd].is_dir = (e.attr & 0x10) != 0;
    fdtab[nfd].dir_idx = 0;
    fdtab[nfd].dirent = loc;
    if (out_fd != NULL)
        *out_fd = nfd;
    if (out_size != NULL)
        *out_size = e.size;
    return 0;
}

static int32_t create_file(const char *path, int32_t *out_fd, uint32_t *out_size)
{
    fat_dirent_t e;
    dent_loc_t loc;

    if (path_exists(path))
        return open_file(path, out_fd, out_size);

    char parent[256], base[256];
    uint8_t name83[11];

    if (split_parent(path, parent, base) != 0)
        return -2;
    int32_t prc = parse_83(base, name83);

    if (prc != 0)
        return prc;

    uint32_t dir_cluster;
    bool pdir;
    uint32_t psize;

    if (find_path(parent, &dir_cluster, &psize, &pdir) != 0 || !pdir)
        return -2;

    if (dir_lookup(dir_cluster, base, &e, NULL) == 0)
        return open_file(path, out_fd, out_size);

    if (dir_find_free(dir_cluster, &loc) != 0)
        return -28;

    memset(&e, 0, sizeof(e));
    memcpy(e.name, name83, 11);
    e.attr = 0x20;
    if (dirent_write(&loc, &e) != 0)
        return -5;

    int32_t nfd = fd_alloc();

    if (nfd < 0)
        return -24;
    fdtab[nfd].used = true;
    fdtab[nfd].cluster = 0;
    fdtab[nfd].size = 0;
    fdtab[nfd].off = 0;
    fdtab[nfd].is_dir = false;
    fdtab[nfd].dir_idx = 0;
    fdtab[nfd].dirent = loc;
    if (out_fd != NULL)
        *out_fd = nfd;
    if (out_size != NULL)
        *out_size = 0;
    return 0;
}

/* Flush the first cluster and size to the file's directory entry. */
static void fd_flush_dirent(int32_t fd)
{
    uint8_t sec[SEC];
    fat_dirent_t *e;

    if (!fd_ok(fd) || fdtab[fd].dirent.lba == 0)
        return;
    if (blk_read(fdtab[fd].dirent.lba, 1, sec) != 0)
        return;
    e = (fat_dirent_t *) (sec + fdtab[fd].dirent.off);
    dirent_set_cluster(e, fdtab[fd].cluster);
    e->size = fdtab[fd].size;
    blk_write(fdtab[fd].dirent.lba, 1, sec);
}

static uint64_t write_from(int32_t fd, uint64_t off, uint64_t len,
                           const uint8_t *src)
{
    fat_fd_t *f = &fdtab[fd];
    uint64_t cs = (uint64_t) spc * SEC;
    uint64_t done = 0;

    if (off > f->size) {
        uint8_t zero[SEC];

        memset(zero, 0, sizeof(zero));
        uint64_t gap = off - f->size;

        while (gap > 0) {
            uint64_t chunk = (gap > sizeof(zero)) ? sizeof(zero) : gap;
            uint64_t n = write_from(fd, f->size, chunk, zero);

            if (n != chunk)
                return done;
            gap -= chunk;
        }
    }

    while (done < len) {
        uint64_t pos = off + done;
        uint32_t idx = (uint32_t) (pos / cs);
        uint32_t in = (uint32_t) (pos % cs);
        uint32_t c = chain_ensure(&f->cluster, idx);

        if (c < 2)
            break;
        if (blk_read(cluster_lba(c), spc, cluster_buf) != 0)
            break;

        uint64_t room = cs - in;
        uint64_t chunk = len - done;

        if (chunk > room)
            chunk = room;
        memcpy(cluster_buf + in, src + done, chunk);
        if (blk_write(cluster_lba(c), spc, cluster_buf) != 0)
            break;
        done += chunk;
        if (pos + chunk > f->size)
            f->size = (uint32_t) (pos + chunk);
    }

    fd_flush_dirent(fd);
    return done;
}

static int32_t truncate_file(int32_t fd, uint64_t newsize)
{
    fat_fd_t *f = &fdtab[fd];
    uint64_t cs = (uint64_t) spc * SEC;

    if (newsize == 0) {
        free_chain(f->cluster);
        f->cluster = 0;
        f->size = 0;
        f->off = 0;
        fd_flush_dirent(fd);
        return 0;
    }

    if (newsize > f->size)
        return -22;             /* growing through truncate is not supported */

    uint32_t keep = (uint32_t) ((newsize - 1) / cs);
    uint32_t tail = chain_at(f->cluster, keep);

    if (tail < 2)
        return -5;
    uint32_t next = fat_next(tail);

    free_chain(next);
    fat_set(tail, FAT_EOC);
    f->size = (uint32_t) newsize;
    if (f->off > newsize)
        f->off = newsize;
    fd_flush_dirent(fd);
    return 0;
}

static void mark_deleted(const dent_loc_t *loc)
{
    uint8_t sec[SEC];

    if (blk_read(loc->lba, 1, sec) != 0)
        return;
    sec[loc->off] = 0xE5;
    blk_write(loc->lba, 1, sec);
}

static int32_t unlink_file(const char *path)
{
    char parent[256], base[256];

    if (split_parent(path, parent, base) != 0)
        return -2;

    uint32_t dir_cluster;
    bool pdir;
    uint32_t psize;
    fat_dirent_t e;
    dent_loc_t loc;
    dent_loc_t lfn_loc;
    uint32_t lfn_count = 0;

    if (find_path(parent, &dir_cluster, &psize, &pdir) != 0 || !pdir)
        return -2;
    if (dir_scan(dir_cluster, base, 0, &e, NULL, &loc, &lfn_loc,
                 &lfn_count) != 0)
        return -2;
    if (e.attr & 0x10)
        return -21;

    free_chain(dirent_cluster(&e));

    if (lfn_count > 0) {
        dent_loc_t l = lfn_loc;

        for (uint32_t i = 0; i < lfn_count; i++) {
            mark_deleted(&l);
            l.off = (uint16_t) (l.off + 32);
            if (l.off >= SEC) {
                l.off = 0;
                l.lba++;
            }
        }
    }
    mark_deleted(&loc);
    return 0;
}

static void handle(sys_ipc_msg_t *m, sys_ipc_msg_t *rep)
{
    if (m->tag != FAT_READ && m->tag != FAT_STAT && m->tag != FAT_READDIR
        && m->tag != FAT_OPEN && m->tag != FAT_CLOSE && m->tag != FAT_SEEK
        && m->tag != FAT_FSTAT && m->tag != FAT_CREATE && m->tag != FAT_WRITE
        && m->tag != FAT_TRUNC && m->tag != FAT_UNLINK) {
        rep->words[0] = (uint64_t) (int64_t) -38;
        return;
    }

    if (!mounted) {
        rep->words[0] = (uint64_t) (int64_t) mount_err;
        return;
    }

    int32_t fd = (int32_t) m->words[0];
    int64_t memh = 0;
    uint8_t *buf = NULL;
    bool need_buf = (m->tag == FAT_OPEN || m->tag == FAT_STAT
                     || m->tag == FAT_READ || m->tag == FAT_READDIR
                     || m->tag == FAT_CREATE || m->tag == FAT_WRITE
                     || m->tag == FAT_UNLINK);

    if (need_buf) {
        memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;
        if (memh == 0 || sys_mem_map(memh, FAT_REQ_ADDR, 3) != 0) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -5;
            return;
        }
        buf = (uint8_t *) (uint64_t) FAT_REQ_ADDR;
    }

    switch (m->tag) {
    case FAT_OPEN:
    case FAT_STAT:{
            char *path = read_path(buf);

            if (path == NULL) {
                rep->words[0] = (uint64_t) (int64_t) -12;
                break;
            }

            uint32_t cluster = 0, size = 0;
            bool is_dir = false;

            if (m->tag == FAT_STAT) {
                if (find_path(path, &cluster, &size, &is_dir) != 0) {
                    free(path);
                    rep->words[0] = (uint64_t) (int64_t) -2;
                    break;
                }
                free(path);
                rep->words[0] = 0;
                rep->words[1] = size;
                rep->words[2] = is_dir ? 1 : 0;
                break;
            }

            int32_t nfd = 0;

            if (open_file(path, &nfd, &size) != 0) {
                free(path);
                rep->words[0] = (uint64_t) (int64_t) -2;
                break;
            }
            free(path);
            rep->words[0] = 0;
            rep->words[1] = (uint64_t) nfd;
            rep->words[2] = size;
            break;
        }

    case FAT_CREATE:{
            char *path = read_path(buf);

            if (path == NULL) {
                rep->words[0] = (uint64_t) (int64_t) -12;
                break;
            }

            int32_t nfd = 0;
            uint32_t size = 0;
            int32_t rc = create_file(path, &nfd, &size);

            free(path);
            if (rc != 0) {
                rep->words[0] = (uint64_t) (int64_t) rc;
                break;
            }
            rep->words[0] = 0;
            rep->words[1] = (uint64_t) nfd;
            rep->words[2] = size;
            break;
        }

    case FAT_CLOSE:
        if (!fd_ok(fd)) {
            rep->words[0] = (uint64_t) (int64_t) -9;
            break;
        }
        fdtab[fd].used = false;
        rep->words[0] = 0;
        break;

    case FAT_FSTAT:
        if (!fd_ok(fd)) {
            rep->words[0] = (uint64_t) (int64_t) -9;
            break;
        }
        rep->words[0] = 0;
        rep->words[1] = fdtab[fd].size;
        rep->words[2] = fdtab[fd].is_dir ? 1 : 0;
        break;

    case FAT_READ:{
            uint64_t len = m->words[1];

            if (!fd_ok(fd) || fdtab[fd].is_dir) {
                rep->words[0] = (uint64_t) (int64_t) -9;
                break;
            }
            if (len > FAT_DATA_MAX)
                len = FAT_DATA_MAX;
            uint64_t n = read_from(fdtab[fd].cluster, fdtab[fd].off, len,
                                   fdtab[fd].size, buf + FAT_DATA_OFF);

            fdtab[fd].off += n;
            rep->words[0] = 0;
            rep->words[1] = n;
            rep->words[2] = fdtab[fd].size;
            break;
        }

    case FAT_WRITE:{
            uint64_t len = m->words[1];

            if (!fd_ok(fd) || fdtab[fd].is_dir) {
                rep->words[0] = (uint64_t) (int64_t) -9;
                break;
            }
            if (len > FAT_DATA_MAX)
                len = FAT_DATA_MAX;
            uint64_t n = write_from(fd, fdtab[fd].off, len,
                                    buf + FAT_DATA_OFF);

            fdtab[fd].off += n;
            rep->words[0] = 0;
            rep->words[1] = n;
            rep->words[2] = fdtab[fd].size;
            break;
        }

    case FAT_TRUNC:
        if (!fd_ok(fd) || fdtab[fd].is_dir) {
            rep->words[0] = (uint64_t) (int64_t) -9;
            break;
        }
        rep->words[0] = (uint64_t) (int64_t) truncate_file(fd, m->words[1]);
        rep->words[1] = fdtab[fd].size;
        break;

    case FAT_UNLINK:{
            char *path = read_path(buf);

            if (path == NULL) {
                rep->words[0] = (uint64_t) (int64_t) -12;
                break;
            }
            rep->words[0] = (uint64_t) (int64_t) unlink_file(path);
            free(path);
            break;
        }

    case FAT_SEEK:{
            int64_t off = (int64_t) m->words[1];
            int64_t whence = (int64_t) m->words[2];
            int64_t pos;

            if (!fd_ok(fd)) {
                rep->words[0] = (uint64_t) (int64_t) -9;
                break;
            }
            if (whence == 0)
                pos = off;
            else if (whence == 1)
                pos = (int64_t) fdtab[fd].off + off;
            else
                pos = (int64_t) fdtab[fd].size + off;
            if (pos < 0) {
                rep->words[0] = (uint64_t) (int64_t) -22;
                break;
            }
            fdtab[fd].off = (uint64_t) pos;
            rep->words[0] = 0;
            rep->words[1] = (uint64_t) pos;
            break;
        }

    case FAT_READDIR:{
            fat_dirent_t e;
            char nm[256];


            if (!fd_ok(fd) || !fdtab[fd].is_dir) {
                rep->words[0] = (uint64_t) (int64_t) -2;
                break;
            }
            if (dir_index(fdtab[fd].cluster, fdtab[fd].dir_idx, &e, nm) != 0) {
                rep->words[0] = (uint64_t) (int64_t) -1;    /* end of directory */
                break;
            }
            fdtab[fd].dir_idx++;
            memcpy(buf + FAT_DATA_OFF, nm, strlen(nm) + 1);
            rep->words[0] = 0;
            rep->words[1] = e.size;
            rep->words[2] = (e.attr & 0x10) ? 1 : 0;
            break;
        }

    default:
        rep->words[0] = (uint64_t) (int64_t) -38;
        break;
    }

    if (memh != 0) {
        sys_mem_unmap(memh, FAT_REQ_ADDR);
        sys_handle_close(memh);
    }
}

int32_t main(void)
{
    while (sys_bootinfo(&bi) < 0 || bi.magic != BOOTINFO_MAGIC) {
        /* The kernel sets the bootinfo before the process is runnable. */
    }

    for (int32_t i = 0; i < 10 && !mounted; i++) {
        struct timespec ts = { 0, 100000000L };

        mount();
        if (!mounted)
            nanosleep(&ts, NULL);
    }

    sys_server_run((int64_t) bi.service_ep, 1000, handle);

    return 0;
}
