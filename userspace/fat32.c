/**-----------------------------------------------------------------------------

 @file    fat32.c
 @brief   Userspace FAT32 server

 @details
 @verbatim

   A read-only FAT32 client of the block server. It parses the BPB, walks the
   directory tree (8.3 names) and reads a file's cluster chain. A FAT_READ
   request carries the path at offset 0 of a shared buffer; the server writes
   the file data at FAT_DATA_OFF and returns the byte count.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <libc/bootinfo.h>
#include <libc/protocol.h>
#include <libc/string.h>
#include <libc/sysfunc.h>

#define SEC             512
#define FAT_BLK_ADDR    0x20000000      /* block I/O memory-object mapping */
#define FAT_REQ_ADDR    0x21000000      /* request/result buffer mapping */
#define FAT_DATA_OFF    4096
#define FAT_DATA_MAX    4096

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

static bootinfo_t bi;
static uint16_t bps;
static uint8_t spc;
static uint32_t fat_lba;
static uint32_t data_lba;
static uint32_t root_cluster;
static bool mounted;
static int mount_err = -100;

/* Single-threaded server: one cluster-sized scratch buffer. */
static uint8_t cluster_buf[SEC * 128];

static int blk_read(uint32_t lba, uint8_t count, uint8_t *dst)
{
    int64_t memh = sys_mem_alloc((uint64_t) count * SEC);
    if (memh < 0)
        return -1;

    int64_t reply = sys_ep_create();
    if (reply < 0) {
        sys_handle_close(memh);
        return -2;
    }

    /* The send moves one handle of each object to the block server; keep the
     * other so we can map the data and receive the reply here. */
    int64_t reply_send = sys_handle_dup(reply);
    int64_t memh_send = sys_handle_dup(memh);
    if (reply_send < 0 || memh_send < 0) {
        if (reply_send >= 0)
            sys_handle_close(reply_send);
        if (memh_send >= 0)
            sys_handle_close(memh_send);
        sys_handle_close(reply);
        sys_handle_close(memh);
        return -3;
    }

    sys_ipc_msg_t m;
    memset(&m, 0, sizeof(m));
    m.tag = BLOCK_READ;
    m.words[0] = lba;
    m.words[1] = count;
    m.xfer[0] = reply_send;
    m.xfer[1] = memh_send;
    m.xfer_count = 2;

    if (sys_ipc_send((int64_t) bi.block_ep, &m) != 0) {
        sys_handle_close(reply_send);
        sys_handle_close(memh_send);
        sys_handle_close(reply);
        sys_handle_close(memh);
        return -4;
    }

    sys_ipc_msg_t rep;
    int rc = -5;
    if (sys_ipc_recv_timeout(reply, &rep, 2000) == 0) {
        if ((int64_t) rep.words[0] != BLOCK_OK) {
            rc = -6;
        } else if (sys_mem_map(memh, FAT_BLK_ADDR, 1) != 0) {
            rc = -7;
        } else {
            memcpy(dst, (void *) (uint64_t) FAT_BLK_ADDR,
                   (uint64_t) count * SEC);
            sys_mem_unmap(memh, FAT_BLK_ADDR);
            rc = 0;
        }
    }

    sys_handle_close(reply);
    sys_handle_close(memh);
    return rc;
}

static uint32_t cluster_lba(uint32_t cluster)
{
    return data_lba + (cluster - 2) * (uint32_t) spc;
}

static uint32_t fat_next(uint32_t cluster)
{
    uint32_t off = cluster * 4;
    uint8_t sec[SEC];

    if (blk_read(fat_lba + off / SEC, 1, sec) != 0)
        return 0;

    uint32_t v = *(uint32_t *) (sec + (off % SEC)) & 0x0FFFFFFF;

    return (v >= 0x0FFFFFF8) ? 0 : v;
}

/* Normalise a path component into an 8.3 directory name (space padded). */
static void to_83(const char *comp, int len, uint8_t out[11])
{
    memset(out, ' ', 11);

    int i = 0, o = 0;
    for (; i < len && comp[i] != '.' && o < 8; i++) {
        char c = comp[i];
        out[o++] = (c >= 'a' && c <= 'z') ? (uint8_t) (c - 32) : (uint8_t) c;
    }
    while (i < len && comp[i] != '.')
        i++;
    if (i < len && comp[i] == '.') {
        i++;
        o = 8;
        for (; i < len && o < 11; i++) {
            char c = comp[i];
            out[o++] = (c >= 'a' && c <= 'z') ? (uint8_t) (c - 32) : (uint8_t) c;
        }
    }
}

/* Look up one 8.3 name in a directory (following its cluster chain). */
static int dir_lookup(uint32_t dir_cluster, const uint8_t name83[11],
                      fat_dirent_t *out)
{
    uint8_t *buf = cluster_buf;
    uint32_t cluster = dir_cluster;
    int found = -1;

    while (cluster >= 2) {
        if (blk_read(cluster_lba(cluster), spc, buf) != 0)
            break;

        int entries = (int) spc * SEC / 32;
        for (int i = 0; i < entries; i++) {
            fat_dirent_t *e = (fat_dirent_t *) (buf + i * 32);

            if (e->name[0] == 0x00) {
                cluster = 0;
                break;
            }
            if (e->name[0] == 0xE5 || e->attr == 0x0F)
                continue;
            if (memcmp(e->name, name83, 11) == 0) {
                memcpy(out, e, sizeof(*out));
                found = 0;
                cluster = 0;
                break;
            }
        }
        if (cluster == 0)
            break;
        cluster = fat_next(cluster);
    }

    return found;
}

/* Walk an absolute path; returns 0 and fills cluster/size/is_dir. */
static int find_path(const char *path, uint32_t *out_cluster,
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
        int len = (int) (p - start);
        while (*p == '/')
            p++;

        uint8_t name83[11];
        fat_dirent_t e;

        to_83(start, len, name83);
        if (dir_lookup(cluster, name83, &e) != 0)
            return -1;

        cluster = (uint32_t) e.cluster_lo | ((uint32_t) e.cluster_hi << 16);
        if (*p == '\0') {
            *out_cluster = cluster;
            *out_size = e.size;
            *out_is_dir = (e.attr & 0x10) != 0;
            return 0;
        }
        if (!(e.attr & 0x10))
            return -1;
    }
    return -1;
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

/* Look up the index-th real entry of a directory (0-based). */
static int dir_index(uint32_t dir_cluster, uint64_t index, fat_dirent_t *out)
{
    uint32_t cluster = dir_cluster;
    uint64_t seen = 0;

    while (cluster >= 2) {
        if (blk_read(cluster_lba(cluster), spc, cluster_buf) != 0)
            break;

        int entries = (int) spc * SEC / 32;
        for (int i = 0; i < entries; i++) {
            fat_dirent_t *e = (fat_dirent_t *) (cluster_buf + i * 32);

            if (e->name[0] == 0x00)
                return -1;
            if (e->name[0] == 0xE5 || e->attr == 0x0F || e->name[0] == '.')
                continue;
            if (e->attr & 0x08)
                continue;       /* volume label */
            if (seen == index) {
                memcpy(out, e, sizeof(*out));
                return 0;
            }
            seen++;
        }
        cluster = fat_next(cluster);
    }
    return -1;
}

static void fmt_83(const uint8_t name[11], char *out)
{
    int o = 0;

    for (int i = 0; i < 8 && name[i] != ' '; i++)
        out[o++] = (char) name[i];
    if (name[8] != ' ') {
        out[o++] = '.';
        for (int i = 8; i < 11 && name[i] != ' '; i++)
            out[o++] = (char) name[i];
    }
    out[o] = '\0';
}

static int mount(void)
{
    uint8_t sec[SEC];
    int rc = blk_read(0, 1, sec);
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

    uint32_t table_size = *(uint32_t *) (sec + 36);
    root_cluster = *(uint32_t *) (sec + 44);

    fat_lba = bs->reserved_sector_count;
    data_lba = fat_lba + (uint32_t) bs->table_count * table_size;

    mounted = true;
    mount_err = 0;
    return 0;
}

static void handle(sys_ipc_msg_t * m, sys_ipc_msg_t * rep)
{
    if (m->tag != FAT_READ && m->tag != FAT_STAT && m->tag != FAT_READDIR) {
        rep->words[0] = (uint64_t) (int64_t) -38;
        return;
    }

    int64_t memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;
    uint8_t *buf = NULL;

    if (!mounted || memh == 0) {
        if (memh != 0)
            sys_handle_close(memh);
        rep->words[0] = (uint64_t) (int64_t) mount_err;
        return;
    }

    if (sys_mem_map(memh, FAT_REQ_ADDR, 3) == 0)
        buf = (uint8_t *) (uint64_t) FAT_REQ_ADDR;
    if (buf == NULL) {
        sys_handle_close(memh);
        rep->words[0] = (uint64_t) (int64_t) -5;
        return;
    }

    char path[256];
    int i = 0;
    while (i < 255 && buf[i] != '\0') {
        path[i] = (char) buf[i];
        i++;
    }
    path[i] = '\0';

    uint32_t cluster = 0, size = 0;
    bool is_dir = false;

    if (find_path(path, &cluster, &size, &is_dir) != 0) {
        sys_mem_unmap(memh, FAT_REQ_ADDR);
        sys_handle_close(memh);
        rep->words[0] = (uint64_t) (int64_t) -2;    /* -ENOENT */
        return;
    }

    if (m->tag == FAT_STAT) {
        rep->words[0] = 0;
        rep->words[1] = size;
        rep->words[2] = is_dir ? 1 : 0;
    } else if (m->tag == FAT_READDIR) {
        fat_dirent_t e;

        if (!is_dir) {
            rep->words[0] = (uint64_t) (int64_t) -2;
        } else if (dir_index(cluster, m->words[0], &e) != 0) {
            rep->words[0] = (uint64_t) (int64_t) -1;    /* end of directory */
        } else {
            fmt_83(e.name, (char *) (buf + FAT_DATA_OFF));
            rep->words[0] = 0;
            rep->words[1] = e.size;
            rep->words[2] = (e.attr & 0x10) ? 1 : 0;
        }
    } else if (is_dir) {
        rep->words[0] = (uint64_t) (int64_t) -2;
    } else {
        uint64_t off = m->words[0];
        uint64_t len = m->words[1];

        if (len > FAT_DATA_MAX)
            len = FAT_DATA_MAX;
        uint64_t n = read_from(cluster, off, len, size, buf + FAT_DATA_OFF);

        rep->words[0] = 0;
        rep->words[1] = n;
        rep->words[2] = size;
    }

    sys_mem_unmap(memh, FAT_REQ_ADDR);
    sys_handle_close(memh);
}

int main(void)
{
    while (sys_bootinfo(&bi) < 0 || bi.magic != BOOTINFO_MAGIC) {
        /* The kernel sets the bootinfo before the process is runnable. */
    }

    mount();

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
