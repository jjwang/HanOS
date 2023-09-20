#include <libc/string.h>

#include <fs/ramfs.h>
#include <fs/filebase.h>
#include <base/kmalloc.h>
#include <base/klog.h>
#include <base/klib.h>
#include <sys/panic.h>
#include <sys/hpet.h>
#include <sys/cmos.h>
#include <sys/mm.h>

#include <kconfig.h>

static bool debug_info = false;

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
    .ioctl = NULL
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
    case '2':
        return VFS_NODE_SYMLINK;
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

    unsigned char* ptr = (unsigned char*)address;

    while (memcmp(ptr + 257, "ustar", 5)) {
        int filesize = oct2bin(ptr + 0x7c, 11);
        ustar_file_t* file = (ustar_file_t*)ptr;
        if (ustar_type_to_vfs_type(file->type) == VFS_NODE_FOLDER) {
            char dname[VFS_MAX_PATH_LEN] = "/";
            strcat(dname, file->name);
            size_t dlen = strlen(dname);
            if (dname[dlen - 1] == '/' && dlen > 1) dname[dlen - 1] = '\0';

            vfs_tnode_t *tnode = vfs_path_to_node
                (dname, CREATE, VFS_NODE_FOLDER);

            /* Set the file type and mode */
            tnode->inode->perms = oct2bin((unsigned char*)&file->mode, 7)
                & (S_IRWXU | S_IRWXG | S_IRWXO);
            tnode->st.st_mode |= tnode->inode->perms;

            time_t file_time = strtol((char*)file->last_modified, OCT);

            /* Adjust for current time zone */
            file_time += DEFAULT_TZ_SEC_SHIFT;

            tnode->st.st_atim.tv_sec = file_time;
            tnode->st.st_mtim.tv_sec = file_time;
            tnode->st.st_ctim.tv_sec = file_time;

            tnode->st.st_atim.tv_nsec = 0;
            tnode->st.st_mtim.tv_nsec = 0;
            tnode->st.st_ctim.tv_nsec = 0;

            if (debug_info) {
                klogi("RAMFS: folder \"%s\"\n", file->name);
            }
            /* TODO: modify folder's datetime related attribute */
        } else if(ustar_type_to_vfs_type(file->type) == VFS_NODE_FILE
                 || ustar_type_to_vfs_type(file->type) == VFS_NODE_SYMLINK) {
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

            /* Adjust for current time zone */
            file_time += DEFAULT_TZ_SEC_SHIFT;

            ramfs_ident_item_t *item =
                (ramfs_ident_item_t*)kmalloc(sizeof(ramfs_ident_item_t));
            if (item == NULL) continue;

            memset(item, 0, sizeof(ramfs_ident_item_t));
            localtime(&file_time, &(item->tm));

            item->type = ustar_type_to_vfs_type(file->type);

            strcpy(item->entry.name, &(dname[name_index]));
            strcpy(item->name, &(dname[name_index]));

            item->entry.size = filesize;

            vfs_tnode_t *tnode = NULL;

            if (item->type == VFS_NODE_SYMLINK) {
                item->entry.size = sizeof(file->linked_file_name);
                if (item->entry.size > 0) {
                    item->entry.data = (void*)kmalloc(item->entry.size);
                    memcpy(item->entry.data, file->linked_file_name,
                           item->entry.size);
                } else {
                    item->entry.data = NULL;
                }

                tnode = vfs_path_to_node(dname, CREATE, VFS_NODE_SYMLINK);
                if (tnode->inode->size <= sizeof(tnode->inode->link))
                {
                    tnode->inode->size = item->entry.size;
                    memcpy(tnode->inode->link, file->linked_file_name,
                           tnode->inode->size);
                }
            } else {
                if (filesize > 0) {
                    item->entry.data = (void*)kmalloc(filesize);
                    memcpy(item->entry.data, (void*)(ptr + 512), filesize);
                } else {
                    item->entry.data = NULL;
                }
            
                tnode = vfs_path_to_node(dname, CREATE, VFS_NODE_FILE);
                tnode->inode->size = item->entry.size;
            }

            /* Set the file type and mode */
            tnode->inode->perms = oct2bin((unsigned char*)&file->mode, 7)
                & (S_IRWXU | S_IRWXG | S_IRWXO);
            tnode->st.st_mode |= tnode->inode->perms;

            tnode->st.st_atim.tv_sec = file_time;
            tnode->st.st_mtim.tv_sec = file_time;
            tnode->st.st_ctim.tv_sec = file_time;

            tnode->st.st_atim.tv_nsec = 0;
            tnode->st.st_mtim.tv_nsec = 0;
            tnode->st.st_ctim.tv_nsec = 0;

            /* Set the file size visible in userspace */
            if (ustar_type_to_vfs_type(file->type) == VFS_NODE_FILE) {
                tnode->st.st_size = tnode->inode->size;
            } else {
                tnode->st.st_size = 0;
            }

            vec_push_back(&ramfs.filelist, (void*)item);

            /* Here we need to set the right parent node */
            strcpy(item->path, dname);
            if (name_index > 0) dname[name_index] = '\0';
            vfs_tnode_t *parent_tnode = vfs_path_to_node(dname, NO_CREATE, 0);
            if (parent_tnode != NULL) {
                item->parent = parent_tnode->inode;
                tnode->parent = parent_tnode->inode;
            } else {
                kloge("RAMFS: %s cannot find parent node\n", file->name);
            }

            if (debug_info) {
                klogi("RAMFS: file \"%s\", size %d bytes, last modified %s\n",
                      file->name, filesize, file->last_modified);
            }
        }
        ptr += (DIV_ROUNDUP(filesize, 512) + 1) * 512;
    }
}

/* The path parameter needs to be full path */
vfs_tnode_t* ramfs_open(vfs_inode_t* this, const char* path)
{
    int nn = 0;
    size_t pathlen = strlen(path);
    for (nn = pathlen - 1; nn >= 0; nn--) {
        if (path[nn] == '/') break;
    }

    ramfs_ident_t* id = (ramfs_ident_t*)this->ident;

    if (id == NULL) { 
        id = (ramfs_ident_t*)kmalloc(sizeof(ramfs_ident_t));
    }
    memset(id, 0, sizeof(ramfs_ident_t));

    /* TODO: Need to speed up this part and check where to free id->data */
    bool islink = false;
    ramfs_ident_item_t *item_link = NULL;

    for (size_t i = 0; i < vec_length(&ramfs.filelist); i++) {
        ramfs_ident_item_t* item = vec_at(&ramfs.filelist, i); 
        if (strcmp(item->path, path) == 0) {
            if (item->type == VFS_NODE_SYMLINK) {
                klogd("RAMFS: symlink %s, target %s\n",
                      item->path, item->entry.data);
                islink = true;
                item_link = item;
                break;
            }

            if (id->data != NULL) {
                id->data = (void*)kmrealloc(id->data, item->entry.size);
            } else {
                id->data = (void*)kmalloc(item->entry.size);
            }
            id->alloc_size = item->entry.size;

            uint8_t *buff = (uint8_t*)item->entry.data;
            if (item->entry.size >= 2) {
                klogd("RAMFS: %s reset 0x%x [0x%02x 0x%02x ...] to %d bytes\n",
                      path, id->data, buff[0], buff[1], item->entry.size);
            }
            memcpy(id->data, item->entry.data, item->entry.size);
            break;
        }
    }
 
    if (islink) {
        for (size_t i = 0; i < vec_length(&ramfs.filelist); i++) {
            ramfs_ident_item_t* item = vec_at(&ramfs.filelist, i);
            if (strcmp(item->path, item_link->entry.data) == 0) {
                if (item->type != VFS_NODE_FILE) {
                    continue;
                }
                if (id->data != NULL) {
                    id->data = (void*)kmrealloc(id->data, item->entry.size);
                } else {
                    id->data = (void*)kmalloc(item->entry.size);
                }
                id->alloc_size = item->entry.size;
                memcpy(id->data, item->entry.data, item->entry.size);
                break;
            }
        }
    }

    klogd("RAMFS: opening %s finished\n", path);
    return vfs_path_to_node(path, NO_CREATE, 0);
}

int64_t ramfs_read(vfs_inode_t* this, size_t offset, size_t len, void* buff)
{
    ramfs_ident_t* id = (ramfs_ident_t*)this->ident;

    size_t retlen = len;
    if (offset + retlen > id->alloc_size) retlen = id->alloc_size - offset;
    if (offset > id->alloc_size) retlen = 0;
    if (retlen > 0) {
        memcpy(buff, ((uint8_t*)id->data) + offset, len);
        klogd("RAMFS: read %d bytes from 0x%x with offset %d\n",
              len, id->data, offset);
    }

    return retlen;
}

int64_t ramfs_write(vfs_inode_t* this, size_t offset, size_t len, const void* buff)
{
    ramfs_ident_t* id = (ramfs_ident_t*)this->ident;
    memcpy(((uint8_t*)id->data) + offset, buff, len);

    klogd("RAMFS: write %d to 0x%x with offset %d\n", len, id->data, offset);

    return 0;
}

/* Synchronizes file size (and other metadata) */
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
        this->inode->ident = NULL;
    }
    return 0;
}

int64_t ramfs_refresh(vfs_inode_t* this)
{
    (void)this;

    return 0;
}

int64_t ramfs_getdent(vfs_inode_t* this, size_t pos, vfs_dirent_t* dirent)
{
    size_t num = 0;
    for (size_t i = 0; i < vec_length(&ramfs.filelist); i++) {
        ramfs_ident_item_t* item = vec_at(&ramfs.filelist, i);
        if ((uint64_t)item->parent != (uint64_t)this) continue;
        if (num == pos) {
            strcpy(dirent->name, item->name);
            memcpy(&dirent->tm, &item->tm, sizeof(tm_t));
            dirent->type = VFS_NODE_FILE;
            dirent->size = item->entry.size;

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
