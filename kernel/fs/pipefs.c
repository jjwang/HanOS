/**-----------------------------------------------------------------------------

 @file    pipefs.c
 @brief   Implementation of PipeFS functions
 @details
 @verbatim

  This file contains the implementation of the PipeFS, a simple filesystem for
  handling pipe files within the HanOS Kernel. It provides functions for
  mounting, creating, removing, opening, reading, and writing pipe files. The
  PipeFS is designed to facilitate inter-process communication by using pipes
  as a means to transfer data between processes.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <libc/string.h>
#include <libc/errno.h>
#include <libc/stdio.h>

#include <fs/pipefs.h>
#include <fs/filebase.h>
#include <base/kmalloc.h>
#include <base/klog.h>
#include <base/klib.h>
#include <sys/panic.h>
#include <mm/mm.h>

#define PIPE_BUFFER_SIZE    64000

/* Filesystem information */
vfs_fsinfo_t pipefs = {
    .name = "pipefs",
    .istemp = true,
    .filelist = { 0 },
    .open = pipefs_open,
    .mount = pipefs_mount,
    .mknode = pipefs_mknode,
    .rmnode = pipefs_rmnode,
    .sync = NULL,
    .refresh = NULL,
    .read = pipefs_read,
    .getdent = NULL,
    .write = pipefs_write,
    .ioctl = NULL
};

extern lock_t vfs_lock;

/* Identifying information for a node */
typedef struct {
    uint8_t buff[PIPE_BUFFER_SIZE];
    int64_t size;
} pipefs_ident_t;

static pipefs_ident_t *create_ident()
{
    pipefs_ident_t *id =
        (pipefs_ident_t *) kmalloc(sizeof(pipefs_ident_t));
    memset(id, 0, sizeof(pipefs_ident_t));
    return id;
}

static int64_t pipe_buff_write(pipefs_ident_t * id, const uint8_t * input,
                               uint64_t len)
{
    if (id->size + 4 + len > PIPE_BUFFER_SIZE) {
        return -1;              /* Buffer overflow */
    }

    /* Write the length as a 4-byte int */
    id->buff[id->size + 3] = (len >> 24) & 0xFF;
    id->buff[id->size + 2] = (len >> 16) & 0xFF;
    id->buff[id->size + 1] = (len >> 8) & 0xFF;
    id->buff[id->size] = len & 0xFF;

    /* Write the data */
    memcpy(id->buff + id->size + 4, input, len);

    /* Update the size */
    id->size += 4 + len;
    return len;
}

static bool pipe_is_eof(pipefs_ident_t * id)
{
    if (id->size == 0) {
        return false;
    }

    /* Read the length of the next data block */
    uint64_t data_len = (id->buff[3] << 24) |
        (id->buff[2] << 16) | (id->buff[1] << 8) | id->buff[0];
    uint8_t magic_word[4] = { (VFS_EOF_MAGIC_WORD >> 24) & 0xFF,
        (VFS_EOF_MAGIC_WORD >> 16) & 0xFF,
        (VFS_EOF_MAGIC_WORD >> 8) & 0xFF,
        VFS_EOF_MAGIC_WORD & 0xFF
    };

    if (data_len == 4
        && id->buff[4] == magic_word[0] && id->buff[5] == magic_word[1]
        && id->buff[6] == magic_word[2] && id->buff[7] == magic_word[3]) {
        return true;
    }

    return false;
}

static int64_t pipe_buff_read(pipefs_ident_t * id, uint8_t * output,
                              size_t max_len)
{
    if (id->size == 0) {
        return -1;              /* Nothing to read */
    }

    /* Read the length of the next data block */
    uint64_t data_len = (id->buff[3] << 24) |
        (id->buff[2] << 16) | (id->buff[1] << 8) | id->buff[0];

    if (pipe_is_eof(id)) {
        return 0;
    }

    if (max_len >= data_len) {
        /* Read the entire data block */
        memcpy(output, id->buff + 4, data_len);

        /* Move the remaining data to the beginning of the buffer */
        memcpy(id->buff, id->buff + 4 + data_len, id->size - 4 - data_len);

        /* Update the size */
        id->size -= 4 + data_len;

        return data_len;
    } else {
        /* Read only max_len bytes */
        memcpy(output, id->buff + 4, max_len);

        /* Move the remaining data to the beginning of the buffer */
        memcpy(id->buff + 4, id->buff + 4 + max_len,
               id->size - 4 - max_len);

        /* Update the new length of the truncated data block */
        uint64_t remaining_len = data_len - max_len;
        id->buff[3] = (remaining_len >> 24) & 0xFF;
        id->buff[2] = (remaining_len >> 16) & 0xFF;
        id->buff[1] = (remaining_len >> 8) & 0xFF;
        id->buff[0] = remaining_len & 0xFF;

        /* Update the size */
        id->size -= max_len;

        return max_len;
    }
}

void pipefs_init(void)
{
    /* Do nothing */
}

vfs_tnode_t *pipefs_open(vfs_inode_t * this, const char *path)
{
    (void) this;

    vfs_tnode_t *tnode =
        vfs_path_to_node(path, CREATE, VFS_NODE_CHAR_DEVICE);

    klogi("PIPEFS: open %s and return 0x%x\n", path, tnode);

    return tnode;
}

int64_t pipefs_read(vfs_inode_t * this, uint64_t offset, uint64_t len,
                    void *buff)
{
    pipefs_ident_t *id = this->ident;

    if (len == 0) {
        klogd
            ("PIPEFS: read %d bytes to 0x%x and return 0 bytes [status: %s]\n",
             len, buff, (pipe_is_eof(id) ? "EOF" : "normal"));
        return 0;
    } else {
        klogd
            ("PIPEFS: try to read %d bytes from 0x%x with %d bytes to 0x%x\n",
             len, id->buff, id->size, buff);
    }

    /* We do not use offset here */
    (void) offset;

    /* According to standard pipe implementation, the task should be blocked
     * here until there are data available.
     */
    int64_t rlen = 0;
    while (true) {
        rlen = pipe_buff_read(id, buff, len);
        if (rlen >= 0)
            break;

        lock_release(&vfs_lock);
        sched_sleep(0);
        lock_lock(&vfs_lock);
    }

    klogd("PIPEFS: read %d bytes from 0x%x to 0x%x and return %d"
          " bytes [%02x]\n", len, id->buff, buff, rlen,
          (rlen > 0 ? ((char *) buff)[rlen - 1] : 0));

    return rlen;
}

int64_t pipefs_write(vfs_inode_t * this, uint64_t offset, uint64_t len,
                     const void *buff)
{
    pipefs_ident_t *id = this->ident;

    klogd("PIPEFS: write %d bytes from %x to %x (PIPE) whose size is "
          "%d bytes, refcount %d, readcount %d, writecount %d "
          "[%02x %02x %02x %02x]\n", len, buff, id->buff, id->size,
          this->refcount, this->readcount, this->writecount,
          (len >= 4 ? ((char *) buff)[len - 4] : 0),
          (len >= 4 ? ((char *) buff)[len - 3] : 0),
          (len >= 4 ? ((char *) buff)[len - 2] : 0),
          (len >= 4 ? ((char *) buff)[len - 1] : 0));

    if (pipe_is_eof(id)) {
        id->size = 0;
    }

    /* We do not use offset here */
    (void) offset;

    /* According to standard pipe implementation, the task should be blocked
     * here until there are rooms available in the pipe.
     */
    int64_t wlen = 0;
    while (true) {
        wlen = pipe_buff_write(id, buff, len);
        if (wlen >= 0)
            break;

        lock_release(&vfs_lock);
        sched_sleep(0);
        lock_lock(&vfs_lock);
    }

    return wlen;
}

int64_t pipefs_mknode(vfs_tnode_t * this)
{
    this->inode->ident = create_ident();
    return 0;
}

int64_t pipefs_rmnode(vfs_tnode_t * this)
{
    pipefs_ident_t *id = (pipefs_ident_t *) this->inode->ident;

    if (id == NULL)
        goto err_exit;
    kmfree(id);

    vfs_inode_t *parent = this->parent;
    uint64_t child_num = vec_length(&parent->child);
    if (child_num > 0) {
        for (uint64_t i = 0; i < child_num; i++) {
            vfs_tnode_t *t = vec_at(&parent->child, i);
            if (t == this) {
                vec_erase(&parent->child, i);
                return 0;
            }
        }
    }
  err_exit:
    return -1;
}

vfs_inode_t *pipefs_mount(vfs_inode_t * at)
{
    (void) at;

    klogi("PIPEFS: mount to 0x%x and load all files from system assets\n",
          at);
    vfs_inode_t *ret =
        vfs_alloc_inode(VFS_NODE_MOUNTPOINT, 0777, 0, &pipefs,
                        NULL);
    ret->ident = create_ident();

    return ret;
}
