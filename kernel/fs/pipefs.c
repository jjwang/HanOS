#include <libc/string.h>
#include <libc/errno.h>
#include <libc/stdio.h>

#include <fs/pipefs.h>
#include <fs/filebase.h>
#include <base/kmalloc.h>
#include <base/klog.h>
#include <base/klib.h>
#include <sys/panic.h>
#include <sys/mm.h>

#define PIPE_BUFFER_SIZE    4096

/* Filesystem information */
vfs_fsinfo_t pipefs = {
    .name = "pipefs",
    .istemp = true,
    .filelist = {0},
    .open = pipefs_open,
    .mount = pipefs_mount,
    .mknode = pipefs_mknode,
    .sync = NULL,
    .refresh = NULL,
    .read = pipefs_read,
    .getdent = NULL,
    .write = pipefs_write,
    .ioctl = NULL
};

lock_t pipe_lock;

/* Identifying information for a node */
typedef struct {
    char    buff[PIPE_BUFFER_SIZE];
    int64_t size;
} pipefs_ident_t;

static pipefs_ident_t *create_ident()
{
    pipefs_ident_t *id = (pipefs_ident_t*)kmalloc(sizeof(pipefs_ident_t));
    memset(id, 0, sizeof(pipefs_ident_t));
    return id;
}

void pipefs_init(void)
{
    /* Do nothing */
}

vfs_tnode_t *pipefs_open(vfs_inode_t *this, const char *path)
{
    (void)this;

    vfs_tnode_t *tnode = vfs_path_to_node(path, CREATE, VFS_NODE_CHAR_DEVICE);

    klogi("PIPEFS: open %s and return 0x%x\n", path, tnode);

    return tnode;
}

int64_t pipefs_read(vfs_inode_t* this, size_t offset, size_t len, void *buff)
{
    pipefs_ident_t *id = this->ident;
    size_t rlen = 0;

    if (id->size == 0 || len == 0) return 0;

    lock_lock(&pipe_lock);

    /* We do not use offset here */
    (void)offset;

    /* Below codes depend on the conduction checking at beginning */
    rlen = id->size;
    if (rlen > len) rlen = len;
    memcpy(buff, id->buff, rlen);
    
    if (id->size - rlen > 0) {
        char val = ((char*)id->buff)[id->size - 1];
        memcpy(id->buff, &(id->buff[rlen]), id->size - rlen);
        if (((char*)id->buff)[id->size - rlen - 1] != val) {
            kpanic("pipefs: corruption in memcpy() while reading %d bytes from " \
                   "%d bytes buffer\n", rlen, id->size);
        }
    }
    id->size -= rlen;

    /* Update start and size of input buffer */
    lock_release(&pipe_lock);

    return rlen;
}

int64_t pipefs_write(vfs_inode_t* this, size_t offset, size_t len,
                    const void* buff)
{
    pipefs_ident_t *id = this->ident;
    size_t wlen = 0;

    lock_lock(&pipe_lock);

    /* We do not use offset here */
    (void)offset;

    /* Output to the buffer */
    wlen = PIPE_BUFFER_SIZE - id->size;
    if (wlen > len) wlen = len;
    memcpy(&(id->buff[id->size]), buff, wlen);

    id->size += wlen;

    lock_release(&pipe_lock);

    return wlen;
}

int64_t pipefs_mknode(vfs_tnode_t* this)
{
    this->inode->ident = create_ident();
    return 0;
}

vfs_inode_t* pipefs_mount(vfs_inode_t* at)
{
    (void)at;

    klogi("PIPEFS: mount to 0x%x and load all files from system assets\n", at);
    vfs_inode_t* ret = vfs_alloc_inode(VFS_NODE_MOUNTPOINT, 0777, 0, &pipefs,
                                       NULL);
    ret->ident = create_ident();

    return ret;
}
