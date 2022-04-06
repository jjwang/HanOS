#include "fs/ramfs.h"
#include "fs/filebase.h"
#include "lib/kmalloc.h"
#include "lib/memutils.h"

/* filesystem information */
vfs_fsinfo_t ramfs = {
    .name = "ramfs",
    .istemp = true,
    .mount = ramfs_mount,
    .mknode = ramfs_mknode,
    .sync = ramfs_sync,
    .refresh = ramfs_refresh,
    .read = ramfs_read,
    .write = ramfs_write,
    .setlink = ramfs_setlink
};

/* identifying information for a node */
typedef struct {
    size_t alloc_size;
    void* data;
} ramfs_ident_t;

static ramfs_ident_t* create_ident()
{
    ramfs_ident_t* id = (ramfs_ident_t*)kmalloc(sizeof(ramfs_ident_t));
    *id = (ramfs_ident_t) { .alloc_size = 0, .data = NULL };
    return id;
}

int64_t ramfs_read(vfs_inode_t* this, size_t offset, size_t len, void* buff)
{
    ramfs_ident_t* id = (ramfs_ident_t*)this->ident;
    memcpy(buff, ((uint8_t*)id->data) + offset, len);
    return 0;
}

int64_t ramfs_write(vfs_inode_t* this, size_t offset, size_t len, const void* buff)
{
    ramfs_ident_t* id = (ramfs_ident_t*)this->ident;
    memcpy(((uint8_t*)id->data) + offset, buff, len);
    return 0;
}

/* synchronizes file size (and other metadata) */
int64_t ramfs_sync(vfs_inode_t* this)
{
    ramfs_ident_t* id = (ramfs_ident_t*)this->ident;
    if (this->size > id->alloc_size) {
        id->alloc_size = this->size;
        id->data = kmrealloc(id->data, id->alloc_size);
    }
    return 0;
}

int64_t ramfs_setlink(vfs_tnode_t* this, vfs_inode_t* inode)
{
    (void)inode;

    if (this->inode->refcount == 0) {
        ramfs_ident_t* id = (ramfs_ident_t*)this->inode->ident;
        if (id->data)
            kmfree(id->data);
        kmfree(id);
    }
    return 0;
}

int64_t ramfs_refresh(vfs_inode_t* this)
{
    (void)this;
    return 0;
}

int64_t ramfs_mknode(vfs_tnode_t* this)
{
    this->inode->ident = create_ident();
    return 0;
}

vfs_inode_t* ramfs_mount(vfs_inode_t* at)
{
    (void)at;
    vfs_inode_t* ret = vfs_alloc_inode(VFS_NODE_MOUNTPOINT, 0777, 0, &ramfs, NULL);
    ret->ident = create_ident();
    return ret;
}
