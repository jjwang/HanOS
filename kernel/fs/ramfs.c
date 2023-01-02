#include <fs/ramfs.h>
#include <fs/filebase.h>
#include <lib/kmalloc.h>
#include <lib/memutils.h>
#include <lib/klog.h>
#include <lib/klib.h>
#include <lib/string.h>
#include <sys/panic.h>
#include <sys/hpet.h>
#include <sys/cmos.h>
#include <sys/mm.h>

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

static uint32_t oct2bin(unsigned char *str, size_t size)
{
    int n = 0;
    unsigned char *c = str;
    while (size-- > 0) {
        n *= 8;
        n += *c - '0';
        c++;
    }
    return n;
}

static uint8_t ustar_type_to_vfs_type(uint8_t type)
{
    switch (type) {
    case '0':
        return VFS_NODE_FILE;
    case '3':
        return VFS_NODE_CHAR_DEVICE;
    case '4':
        return VFS_NODE_BLOCK_DEVICE;
    case '5':
        return VFS_NODE_FOLDER;
    default:
        return VFS_NODE_INVALID;
    }
}

void ramfs_init(void* address, uint64_t size)
{
    klogi("RAMFS: init from 0x%x with len %d\n", address, size);

    vmm_map(NULL, (uint64_t)address, VIRT_TO_PHYS(address), NUM_PAGES(size),
        VMM_FLAGS_DEFAULT, true);

    unsigned char* ptr = (unsigned char*)address;

    while (memcmp(ptr + 257, "ustar", 5)) {
        int filesize = oct2bin(ptr + 0x7c, 11);
        ustar_file_t* file = (ustar_file_t*)ptr;
        if (ustar_type_to_vfs_type(file->type) == VFS_NODE_FOLDER) {
            char dname[VFS_MAX_PATH_LEN] = "/";
            strcat(dname, file->name);
            size_t dlen = strlen(dname);
            if (dname[dlen - 1] == '/' && dlen > 1) dname[dlen - 1] = '\0';
            vfs_path_to_node(dname, CREATE, VFS_NODE_FOLDER);

            klogi("RAMFS: folder \"%s\"\n", file->name);
            /* TODO: modify folder's datetime related attribute */
        } else if(ustar_type_to_vfs_type(file->type) == VFS_NODE_FILE) {
            char dname[VFS_MAX_PATH_LEN] = "/";
            strcat(dname, file->name);
            size_t dlen = strlen(dname);
            int64_t name_index = 0;

            for (name_index = dlen - 1; name_index >= 0; name_index--) {
                if (dname[name_index] == '/') {
                    name_index++;
                    break;
                }
            }

            time_t file_time = strtol((char*)file->last_modified, OCT);
            ramfs_ident_item_t *item =
                (ramfs_ident_item_t*)kmalloc(sizeof(ramfs_ident_item_t));
            if (item == NULL) continue;

            memset(item, 0, sizeof(ramfs_ident_item_t));
            localtime(&file_time, &(item->tm));

            item->parent = vfs_path_to_node("/", NO_CREATE, 0)->inode;

            strcpy(item->entry.name, &(dname[name_index]));
            item->entry.data = (void*)kmalloc(filesize);
            memcpy(item->entry.data, (void*)(ptr + 512), filesize);
            item->entry.size = filesize;        
            strcpy(item->name, &(dname[name_index]));

            vec_push_back(&ramfs.filelist, (void*)item);

            vfs_tnode_t *tnode = vfs_path_to_node(dname, CREATE, VFS_NODE_FILE);
            tnode->inode->size = filesize;

            klogi("RAMFS: file \"%s\", size %d bytes, last modified %s\n",
                  file->name, filesize, file->last_modified);
        }
        ptr += (DIV_ROUNDUP(filesize, 512) + 1) * 512;
    }

    vmm_unmap(NULL, (uint64_t)address, NUM_PAGES(size), true);
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

    return ret;
}
