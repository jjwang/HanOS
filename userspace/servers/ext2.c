/**-----------------------------------------------------------------------------

 @file    ext2.c
 @brief   Userspace read-only ext2 server

 @details
 @verbatim

   A read-only ext2 client of the block server. It mounts partition index 1
   through BLOCK_GET_PART, reads the superblock, the block group descriptors and
   the inode table, and serves OPEN / READ / READDIR / STAT / CLOSE / SEEK. Names
   keep their case; each inode carries a mode and an owner. It parses the direct
   and the single, double and triple indirect block maps.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <bootinfo.h>
#include <protocol.h>
#include <string.h>
#include <stdlib.h>
#include <sysfunc.h>

#define SEC                 512
#define EXT2_BLK_ADDR       0x20000000  /* block I/O memory-object mapping */
#define EXT2_REQ_ADDR       0x21000000  /* request/result buffer mapping */
#define EXT2_DATA_OFF       EXT2_IO_DATA_OFF
#define EXT2_DATA_MAX       (EXT2_IO_BUF_SIZE - EXT2_IO_DATA_OFF)
#define EXT2_PART_INDEX     1
#define EXT2_ROOT_INO       2
#define EXT2_FD_MAX         32
#define EXT2_MAX_GROUPS     128

static bootinfo_t bi;

static uint32_t blk_size;
static uint32_t inodes_per_group;
static uint32_t inode_count;
static uint32_t inode_size;
static uint32_t first_data_block;
static uint32_t part_lba;
static bool mounted;

static uint8_t gdt[EXT2_MAX_GROUPS * 32];
static uint8_t blk[4096];       /* one filesystem block */
static uint8_t ind[4096];       /* one indirect block */

static uint16_t rd16(const uint8_t * p)
{
    return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

static uint32_t rd32(const uint8_t * p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8)
        | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

/* Read `count` sectors starting at an absolute LBA through the block server. */
static int32_t blk_read(uint32_t lba, uint8_t count, uint8_t *dst)
{
    int64_t memh = sys_mem_alloc((uint64_t) count * SEC);
    if (memh < 0)
        return -1;

    int64_t reply = sys_ep_create();
    if (reply < 0) {
        sys_handle_close(memh);
        return -2;
    }

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
    int32_t rc = -5;
    if (sys_ipc_recv_timeout(reply, &rep, 2000) == 0) {
        if ((int64_t) rep.words[0] != BLOCK_OK) {
            rc = -6;
        } else if (sys_mem_map(memh, EXT2_BLK_ADDR, 1) != 0) {
            rc = -7;
        } else {
            memcpy(dst, (void *) (uint64_t) EXT2_BLK_ADDR,
                   (uint64_t) count * SEC);
            sys_mem_unmap(memh, EXT2_BLK_ADDR);
            rc = 0;
        }
    }

    sys_handle_close(reply);
    sys_handle_close(memh);
    return rc;
}

/* Ask the block server for a partition's LBA range. */
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

static void mount(void)
{
    uint32_t start = 0, count = 0;

    if (blk_get_part(EXT2_PART_INDEX, &start, &count) != 0)
        return;
    part_lba = start;

    /* The superblock sits at byte offset 1024 within the volume. */
    uint8_t sb[1024];

    if (blk_read(part_lba + 1024 / SEC, 2, sb) != 0)
        return;
    if (rd16(sb + 56) != 0xEF53)
        return;

    inode_count = rd32(sb + 0);
    inodes_per_group = rd32(sb + 40);
    inode_size = rd16(sb + 88);
    if (inode_size == 0)
        inode_size = 128;
    blk_size = 1024u << rd32(sb + 24);
    first_data_block = rd32(sb + 20);
    if (blk_size < 1024 || blk_size > 4096)
        return;

    uint32_t sectors_per_block = blk_size / SEC;

    /* Block group descriptor table: block 2 for a 1 KiB block size, else 1. */
    uint32_t gdt_block = (blk_size == 1024) ? 2 : 1;
    uint32_t groups = (rd32(sb + 4) - first_data_block
                       + rd32(sb + 32) - 1) / rd32(sb + 32);

    if (groups > EXT2_MAX_GROUPS)
        groups = EXT2_MAX_GROUPS;

    uint32_t gdt_bytes = groups * 32;
    uint32_t done = 0;

    for (uint32_t b = 0; done < gdt_bytes; b++) {
        uint32_t chunk = blk_size;

        if (blk_read(part_lba + (gdt_block + b) * sectors_per_block,
                     (uint8_t) sectors_per_block, blk) != 0)
            return;
        if (chunk > gdt_bytes - done)
            chunk = gdt_bytes - done;
        memcpy(gdt + done, blk, chunk);
        done += chunk;
    }

    mounted = true;
}

static int32_t read_block(uint32_t block, uint8_t *dst)
{
    return blk_read(part_lba + (uint64_t) block * (blk_size / SEC),
                    (uint8_t) (blk_size / SEC), dst);
}

static int32_t inode_read(uint32_t n, uint8_t *out)
{
    if (n == 0 || n > inode_count)
        return -1;

    uint32_t group = (n - 1) / inodes_per_group;
    uint32_t index = (n - 1) % inodes_per_group;
    uint32_t table = rd32(gdt + (uint64_t) group * 32 + 8);
    uint64_t off = (uint64_t) table * blk_size + (uint64_t) index * inode_size;
    uint32_t block = (uint32_t) (off / blk_size);
    uint32_t boff = (uint32_t) (off % blk_size);

    if (read_block(block, blk) != 0)
        return -1;
    if (boff + inode_size <= blk_size) {
        memcpy(out, blk + boff, inode_size);
    } else {
        uint32_t first = blk_size - boff;

        memcpy(out, blk + boff, first);
        if (read_block(block + 1, blk) != 0)
            return -1;
        memcpy(out + first, blk, inode_size - first);
    }
    return 0;
}

static uint32_t block_map(const uint8_t * inode, uint32_t lbn)
{
    uint32_t per = blk_size / 4;

    if (lbn < 12)
        return rd32(inode + 40 + lbn * 4);

    lbn -= 12;
    if (lbn < per) {
        uint32_t a = rd32(inode + 40 + 12 * 4);

        if (a == 0 || read_block(a, ind) != 0)
            return 0;
        return rd32(ind + lbn * 4);
    }

    lbn -= per;
    if (lbn < per * per) {
        uint32_t a = rd32(inode + 40 + 13 * 4);

        if (a == 0 || read_block(a, ind) != 0)
            return 0;
        uint32_t b = rd32(ind + (lbn / per) * 4);

        if (b == 0 || read_block(b, ind) != 0)
            return 0;
        return rd32(ind + (lbn % per) * 4);
    }

    lbn -= per * per;
    uint32_t a = rd32(inode + 40 + 14 * 4);

    if (a == 0 || read_block(a, ind) != 0)
        return 0;
    uint32_t b = rd32(ind + (lbn / (per * per)) * 4);

    if (b == 0 || read_block(b, ind) != 0)
        return 0;
    uint32_t c = rd32(ind + ((lbn / per) % per) * 4);

    if (c == 0 || read_block(c, ind) != 0)
        return 0;
    return rd32(ind + (lbn % per) * 4);
}

static uint64_t file_read(const uint8_t * inode, uint64_t off, uint64_t len,
                          uint8_t *dst)
{
    uint32_t size = rd32(inode + 4);

    if (off >= size)
        return 0;
    if (off + len > size)
        len = size - off;

    uint64_t done = 0;

    while (done < len) {
        uint32_t lbn = (uint32_t) ((off + done) / blk_size);
        uint32_t boff = (uint32_t) ((off + done) % blk_size);
        uint32_t chunk = blk_size - boff;
        uint32_t pblk;

        if (chunk > len - done)
            chunk = (uint32_t) (len - done);

        pblk = block_map(inode, lbn);
        if (pblk == 0) {
            memset(dst + done, 0, chunk);
            done += chunk;
            continue;
        }
        if (read_block(pblk, blk) != 0)
            return done;
        memcpy(dst + done, blk + boff, chunk);
        done += chunk;
    }

    return done;
}

/* Walk a directory. With `want` set, look the name up and return its inode; with
 * `want` NULL, return the `index`-th live entry and its name. */
static int32_t dir_walk(const uint8_t * inode, const char *want, uint64_t index,
                        uint32_t * out_ino, uint8_t * out_type, char *out_name,
                        uint64_t namesz)
{
    uint32_t size = rd32(inode + 4);
    uint64_t seen = 0;
    uint32_t want_len = (want != NULL) ? (uint32_t) strlen(want) : 0;

    for (uint64_t off = 0; off < size; off += blk_size) {
        uint32_t pblk = block_map(inode, (uint32_t) (off / blk_size));
        uint32_t e = 0;

        if (pblk == 0)
            continue;
        if (read_block(pblk, blk) != 0)
            return -1;

        while (e + 8 <= blk_size) {
            uint32_t dino = rd32(blk + e);
            uint16_t rl = rd16(blk + e + 4);
            uint8_t nl = blk[e + 6];

            if (rl < 8)
                break;
            if (dino != 0 && nl > 0) {
                const char *nm = (const char *) (blk + e + 8);

                if (want != NULL) {
                    if (nl == want_len && memcmp(want, nm, nl) == 0) {
                        *out_ino = dino;
                        if (out_type != NULL)
                            *out_type = blk[e + 7];
                        return 0;
                    }
                } else if (seen == index) {
                    uint32_t n = (nl < namesz - 1) ? nl : (uint32_t) (namesz - 1);

                    memcpy(out_name, nm, n);
                    out_name[n] = '\0';
                    *out_ino = dino;
                    if (out_type != NULL)
                        *out_type = blk[e + 7];
                    return 0;
                } else {
                    seen++;
                }
            }
            e += rl;
        }
    }

    return -1;                  /* not found / end of directory */
}

static uint32_t path_resolve(const char *path)
{
    uint32_t ino = EXT2_ROOT_INO;
    uint8_t inode[256];

    if (path == NULL)
        return 0;
    if (path[0] == '/')
        path++;

    while (*path != '\0') {
        char comp[256];
        uint32_t ci = 0;

        while (*path != '\0' && *path != '/' && ci < 255)
            comp[ci++] = *path++;
        comp[ci] = '\0';
        while (*path == '/')
            path++;
        if (ci == 0)
            continue;

        if (inode_read(ino, inode) != 0)
            return 0;
        if ((rd16(inode) & 0xF000) != 0x4000)
            return 0;
        uint32_t next = 0;

        if (dir_walk(inode, comp, 0, &next, NULL, NULL, 0) != 0)
            return 0;
        ino = next;
    }

    return ino;
}

typedef struct {
    bool used;
    uint32_t ino;
    uint32_t size;
    bool is_dir;
    uint32_t mode;
    uint64_t meta;              /* uid | gid<<16 | nlink<<32 */
    uint32_t mtime;
    uint64_t off;
    uint64_t dir_idx;           /* readdir cursor */
} ext2_fd_t;

static ext2_fd_t fdtab[EXT2_FD_MAX];

static int32_t fd_alloc(void)
{
    for (int32_t i = 0; i < EXT2_FD_MAX; i++)
        if (!fdtab[i].used)
            return i;
    return -1;
}

static bool fd_ok(int32_t fd)
{
    return fd >= 0 && fd < EXT2_FD_MAX && fdtab[fd].used;
}

static char *read_path(const uint8_t * buf)
{
    uint64_t i = 0;
    char *path;

    while (i < EXT2_DATA_OFF && buf[i] != '\0')
        i++;
    path = malloc(i + 1);
    if (path == NULL)
        return NULL;
    memcpy(path, buf, i);
    path[i] = '\0';
    return path;
}

static void handle(sys_ipc_msg_t * m, sys_ipc_msg_t * rep)
{
    int32_t fd = (int32_t) m->words[0];
    int64_t memh = 0;
    uint8_t *buf = NULL;

    if (m->tag == EXT2_OPEN || m->tag == EXT2_STAT || m->tag == EXT2_READ
        || m->tag == EXT2_READDIR) {
        memh = (m->xfer_count >= 2) ? (int64_t) m->xfer[1] : 0;
        if (memh == 0 || sys_mem_map(memh, EXT2_REQ_ADDR, 3) != 0) {
            if (memh != 0)
                sys_handle_close(memh);
            rep->words[0] = (uint64_t) (int64_t) -5;
            return;
        }
        buf = (uint8_t *) (uint64_t) EXT2_REQ_ADDR;
    }

    switch (m->tag) {
    case EXT2_OPEN:
    case EXT2_STAT:{
            char *path = read_path(buf);

            if (path == NULL) {
                rep->words[0] = (uint64_t) (int64_t) -12;
                break;
            }
            uint32_t ino = path_resolve(path);

            free(path);
            if (ino == 0) {
                rep->words[0] = (uint64_t) (int64_t) -2;
                break;
            }

            uint8_t inode[256];

            if (inode_read(ino, inode) != 0) {
                rep->words[0] = (uint64_t) (int64_t) -5;
                break;
            }
            uint32_t mode = rd16(inode);
            uint32_t size = rd32(inode + 4);
            bool is_dir = (mode & 0xF000) == 0x4000;
            uint32_t uid = rd16(inode + 2);
            uint32_t gid = rd16(inode + 24);
            uint32_t nlink = rd16(inode + 26);
            uint32_t mtime = rd32(inode + 16);
            uint64_t meta = (uid & 0xffffu)
                | ((uint64_t) (gid & 0xffffu) << 16)
                | ((uint64_t) (nlink & 0xffffu) << 32);

            if (m->tag == EXT2_STAT) {
                rep->words[0] = 0;
                rep->words[1] = size;
                rep->words[2] = is_dir ? 1 : 0;
                rep->words[3] = mode;
                rep->words[4] = meta;
                rep->words[5] = mtime;
                break;
            }

            int32_t nfd = fd_alloc();

            if (nfd < 0) {
                rep->words[0] = (uint64_t) (int64_t) -24;
                break;
            }
            fdtab[nfd].used = true;
            fdtab[nfd].ino = ino;
            fdtab[nfd].size = size;
            fdtab[nfd].is_dir = is_dir;
            fdtab[nfd].mode = mode;
            fdtab[nfd].meta = meta;
            fdtab[nfd].mtime = mtime;
            fdtab[nfd].off = 0;
            fdtab[nfd].dir_idx = 0;
            rep->words[0] = 0;
            rep->words[1] = (uint64_t) nfd;
            rep->words[2] = size;
            rep->words[3] = is_dir ? 1 : 0;
            break;
        }

    case EXT2_CLOSE:
        if (!fd_ok(fd)) {
            rep->words[0] = (uint64_t) (int64_t) -9;
            break;
        }
        fdtab[fd].used = false;
        rep->words[0] = 0;
        break;

    case EXT2_FSTAT:
        if (!fd_ok(fd)) {
            rep->words[0] = (uint64_t) (int64_t) -9;
            break;
        }
        rep->words[0] = 0;
        rep->words[1] = fdtab[fd].size;
        rep->words[2] = fdtab[fd].is_dir ? 1 : 0;
        rep->words[3] = fdtab[fd].mode;
        rep->words[4] = fdtab[fd].meta;
        rep->words[5] = fdtab[fd].mtime;
        break;

    case EXT2_READ:{
            uint64_t len = m->words[1];
            uint8_t inode[256];

            if (!fd_ok(fd) || fdtab[fd].is_dir) {
                rep->words[0] = (uint64_t) (int64_t) -9;
                break;
            }
            if (len > EXT2_DATA_MAX)
                len = EXT2_DATA_MAX;
            if (inode_read(fdtab[fd].ino, inode) != 0) {
                rep->words[0] = (uint64_t) (int64_t) -5;
                break;
            }
            uint64_t n = file_read(inode, fdtab[fd].off, len,
                                   buf + EXT2_DATA_OFF);

            fdtab[fd].off += n;
            rep->words[0] = 0;
            rep->words[1] = n;
            rep->words[2] = fdtab[fd].size;
            break;
        }

    case EXT2_SEEK:{
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

    case EXT2_READDIR:{
            uint8_t inode[256];
            uint32_t ino;
            uint8_t type;

            if (!fd_ok(fd) || !fdtab[fd].is_dir) {
                rep->words[0] = (uint64_t) (int64_t) -2;
                break;
            }
            if (inode_read(fdtab[fd].ino, inode) != 0) {
                rep->words[0] = (uint64_t) (int64_t) -5;
                break;
            }
            if (dir_walk(inode, NULL, fdtab[fd].dir_idx, &ino, &type,
                         (char *) (buf + EXT2_DATA_OFF), 256) != 0) {
                rep->words[0] = (uint64_t) (int64_t) -1;
                break;
            }
            fdtab[fd].dir_idx++;
            rep->words[0] = 0;
            rep->words[1] = 0;          /* size of the entry: not needed */
            rep->words[2] = (type == 2) ? 1 : 0;
            break;
        }

    default:
        rep->words[0] = (uint64_t) (int64_t) -38;
        break;
    }

    if (memh != 0) {
        sys_mem_unmap(memh, EXT2_REQ_ADDR);
        sys_handle_close(memh);
    }
}

int32_t main(void)
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
