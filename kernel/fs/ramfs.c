#include <fs/ramfs.h>
#include <fs/filebase.h>
#include <fs/ramfs_file.h>
#include <lib/kmalloc.h>
#include <lib/memutils.h>
#include <lib/klog.h>
#include <lib/string.h>
#include <core/panic.h>
#include <core/hpet.h>
#include <core/cmos.h>

/* Filesystem information */
vfs_fsinfo_t ramfs = {
    .name = "ramfs",
    .istemp = true,
    .filelist = {0},
    .open = ramfs_open,
    .mount = ramfs_mount,
    .mknode = ramfs_mknode,
    .sync = ramfs_sync,
    .refresh = ramfs_refresh,
    .read = ramfs_read,
    .getdent = ramfs_getdent,
    .write = ramfs_write,
};

/* Identifying information for a node */
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

vfs_tnode_t* ramfs_open(vfs_inode_t* this, const char* path)
{
    klogi("RAMFS: Open 0x%x with path %s\n", this, path);

    int idx = 0;
    size_t pathlen = strlen(path);
    for (idx = pathlen - 1; idx >= 0; idx--) {
        if (path[idx] == '/') break;
    }
    if (idx < 0) idx = 0;

    ramfs_ident_t* id = (ramfs_ident_t*)this->ident;

    if (id == NULL) { 
        id = (ramfs_ident_t*)kmalloc(sizeof(ramfs_ident_t));
        memset(id, 0, sizeof(ramfs_ident_t));
    }
    if (id->data != NULL) goto exit;
 
    for (size_t i = 0; i < vec_length(&ramfs.filelist); i++) {
        ramfs_ident_item_t* item = vec_at(&ramfs.filelist, i); 
        if (strcmp(item->name, &(path[idx + 1])) == 0) {
            if (id->data != NULL) {
                id->data = (void*)kmrealloc(id->data, item->entry.size);
            } else {
                id->data = (void*)kmalloc(item->entry.size);
            }
            id->alloc_size = item->entry.size;
            memcpy(id->data, item->entry.data, item->entry.size);
            klogi("RAMFS:\tGet #%d file %s, len %d\n", i, item->name,
                  item->entry.size);
            break;
        }   
    }

exit:
    return vfs_path_to_node(path, NO_CREATE, 0);
}

int64_t ramfs_read(vfs_inode_t* this, size_t offset, size_t len, void* buff)
{
    klogi("RAMFS: read from 0x%x with length %d offset %d\n",
          this, len, offset);

    size_t retlen = len;
    ramfs_ident_t* id = (ramfs_ident_t*)this->ident;
    if (offset + retlen > id->alloc_size) retlen = id->alloc_size - offset;
    if (offset > id->alloc_size) retlen = 0;
    if (retlen > 0) memcpy(buff, ((uint8_t*)id->data) + offset, len);

    return retlen;
}

int64_t ramfs_write(vfs_inode_t* this, size_t offset, size_t len, const void* buff)
{
    klogi("RAMFS: write to 0x%x with length %d offset %d\n",
          this, len, offset);

    ramfs_ident_t* id = (ramfs_ident_t*)this->ident;
    memcpy(((uint8_t*)id->data) + offset, buff, len);
    return 0;
}

/* Synchronizes file size (and other metadata) */
int64_t ramfs_sync(vfs_inode_t* this)
{
    ramfs_ident_t* id = (ramfs_ident_t*)this->ident;
    klogi("RAMFS: sync 0x%x from %d to %d\n", this, id->alloc_size, this->size);

    if (this->size > id->alloc_size) {
        id->alloc_size = this->size;
        id->data = kmrealloc(id->data, id->alloc_size);
    }
    return 0;
}

int64_t ramfs_setlink(vfs_tnode_t* this, vfs_inode_t* inode)
{
    klogi("RAMFS: setlink to 0x%x\n", this);

    (void)inode;

    if (this->inode->refcount == 0) {
        ramfs_ident_t* id = (ramfs_ident_t*)this->inode->ident;
        if (id->data)
            kmfree(id->data);
        kmfree(id);
        this->inode->ident = NULL;
    }
    return 0;
}

int64_t ramfs_refresh(vfs_inode_t* this)
{
    klogi("RAMFS: refresh 0x%x\n", this);
    return 0;
}

int64_t ramfs_getdent(vfs_inode_t* this, size_t pos, vfs_dirent_t* dirent)
{
    klogi("RAMFS: getdent 0x%x\n", this);

    size_t num = 0;
    for (size_t i = 0; i < vec_length(&ramfs.filelist); i++) {
        ramfs_ident_item_t* item = vec_at(&ramfs.filelist, i);
        if ((uint64_t)item->parent != (uint64_t)this) continue;
        if (num == pos) {
            strcpy(dirent->name, item->name);
            memcpy(&dirent->tm, &item->tm, sizeof(tm_t));
            dirent->type = VFS_NODE_FILE;
            dirent->size = item->entry.size;

            klogi("RAMFS:\tGet #%d file %s, len %d\n", i, item->name,
                  item->entry.size);
            return 0;
        }   
        num++;
    }

    return -1; 
}

int64_t ramfs_mknode(vfs_tnode_t* this)
{
    klogi("RAMFS: mknode 0x%x\n", this);

    this->inode->ident = create_ident();
    return 0;
}

vfs_inode_t* ramfs_mount(vfs_inode_t* at)
{
    (void)at;
    klogi("RAMFS: mount to 0x%x and load all files from system assets\n", at);
    vfs_inode_t* ret = vfs_alloc_inode(
        VFS_NODE_MOUNTPOINT, 0777, 0, &ramfs, NULL);
    ret->ident = create_ident();

    uint64_t now_sec = hpet_get_nanos() / 1000000000;
    
    time_t boot_time = cmos_boot_time();
    time_t now_time = now_sec + boot_time;

    for (size_t i = 0; i < RAMFS_FILE_NUM; i++) {
        ramfs_ident_item_t* item =
            (ramfs_ident_item_t*)kmalloc(sizeof(ramfs_ident_item_t));
        if (item == NULL) continue;

        memset(item, 0, sizeof(ramfs_ident_item_t));
        localtime(&now_time, &(item->tm));
        item->parent = ret;

        switch (i) {
        case 0:
            strcpy(item->entry.name, "HELLOWLD.TXT");
            item->entry.data = (void*)&helloworld_text_file_start;
            item->entry.size = (uint64_t)&helloworld_text_file_end
                - (uint64_t)&helloworld_text_file_start;
            break;
        case 1: 
            strcpy(item->entry.name, "HELLOWLD2.TXT");
            item->entry.data = (void*)&helloworld_text_file_start;
            item->entry.size = (uint64_t)&helloworld_text_file_end
                - (uint64_t)&helloworld_text_file_start;
            break;
        }
        strcpy(item->name, item->entry.name);

        vec_push_back(&ramfs.filelist, (void*)item);

        /* Call vfs_path_to_node() to alloc nodes */
    }

    return ret;
}
