/**-----------------------------------------------------------------------------

 @file    fat32.c
 @brief   Implementation of FAT32 related functions
 @details
 @verbatim

  The functions in this file are all needed by VFS requirements. The r/w of
  FAT32 are PIO 28 which need to be improved for higher speed.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>

#include <libc/string.h>

#include <device/storage/ata.h>
#include <sys/panic.h>
#include <fs/vfs.h>
#include <fs/fat32.h>
#include <fs/filebase.h>
#include <base/kmalloc.h>
#include <base/klog.h>
#include <base/klib.h>
#include <base/vector.h>

#define SECTORSHIFT                 9   /* 512 bytes */
#define SECTOR_TO_OFFSET(x)         ((x) << SECTORSHIFT)
#define CLUSTER_TO_OFFSET(x, c, s)  (SECTOR_TO_OFFSET((c) + ((x) - 2) * (s)))

/* filesystem information */
vfs_fsinfo_t fat32 = {
    .name = "fat32",
    .istemp = false,
    .filelist = {0},
    .open = fat32_open,
    .mount = fat32_mount,
    .mknode = fat32_mknode,
    .rmnode = NULL,
    .sync = fat32_sync,
    .refresh = fat32_refresh,
    .read = fat32_read,
    .getdent = fat32_getdent,
    .write = fat32_write,
    .ioctl = NULL
};

static fat32_ident_t* create_ident()
{
    fat32_ident_t* id = (fat32_ident_t*)kmalloc(sizeof(fat32_ident_t));
    memset(id, 0, sizeof(fat32_ident_t));
    return id;
}

int fat32_read_entry(
        vfs_inode_t* this, uint32_t cluster, size_t index, fat32_entry_t *dest)
{
    fat32_ident_t* id = (fat32_ident_t*)this->ident;

    /* Need to add cache to speed up reading in the future */
    uint8_t dd[512] = {0};
    ata_pio_read28(id->device,
        id->bs.cluster_begin_lba + (cluster - 2) * id->bs.sectors_per_cluster,
        1, dd);

    static bool display = false;
    if (!display) {
        uint32_t* dl = (uint32_t*)dd;
        klogv("FAT32: Read %x entry from cluster %04d:%04d - 0x%08x 0x%08x 0x%08x...\n",
            id->device, id->bs.cluster_begin_lba, cluster, dl[0], dl[1], dl[2]);
        display = true;
    }

    fat_dir_entry_t* de = (fat_dir_entry_t*)dd;

    memcpy(dest->name, de[index].file_name_and_ext, 11);
    dest->attribute = de[index].attribute;
    dest->cluster_begin = (uint32_t)de[index].cluster_num_high << 16;
    dest->cluster_begin = de[index].cluster_num_low | dest->cluster_begin;
    dest->file_size_bytes = de[index].file_size_bytes;

    dest->dir_entry_cluster = cluster;
    dest->dir_entry_index = index;

    return 1;
}

int fat32_write_entry(vfs_inode_t* this, fat32_entry_t *src)
{
    fat32_ident_t* id = (fat32_ident_t*)this->ident;
    uint32_t cluster = src->dir_entry_cluster;
    size_t index = src->dir_entry_index; 

    /* Need to add cache to speed up reading in the future */
    uint8_t dd[512] = {0};
    ata_pio_read28(id->device,
        id->bs.cluster_begin_lba + (cluster - 2) * id->bs.sectors_per_cluster,
        1, dd);

    uint32_t* dl = (uint32_t*)dd;
    klogv("FAT32: Read %x entry from cluster %04d:%04d - "
          "0x%08x 0x%08x 0x%08x...\n",
          id->device, id->bs.cluster_begin_lba, cluster,
          dl[0], dl[1], dl[2]);

    fat_dir_entry_t* de = (fat_dir_entry_t*)dd;

    memcpy(de[index].file_name_and_ext, src->name, 11);
    de[index].attribute = src->attribute;
    de[index].cluster_num_high = (src->cluster_begin >> 16) & 0x0000FFFF;
    de[index].cluster_num_low = src->cluster_begin & 0x0000FFFF;
    de[index].file_size_bytes = src->file_size_bytes;;

    char fn[VFS_MAX_NAME_LEN] = {0};
    fat32_get_short_filename(de[index].file_name_and_ext, fn);
    klogv("FAT32: Modify directory entry of %s (%d:%d) to length %d\n",
          fn, cluster, index, src->file_size_bytes);

    ata_pio_write28(id->device,
        id->bs.cluster_begin_lba + (cluster - 2) * id->bs.sectors_per_cluster,
        1, dd);

    return 1;
}

/* Notes: need to consider multiple sectors which are not continuous */
int64_t fat32_read(vfs_inode_t* this, size_t offset, size_t len, void* buff)
{
    fat32_ident_t* id = (fat32_ident_t*)this->ident;

    uint32_t cluster = id->entry.cluster_begin;
    klogi("FAT32: Read %4d bytes from cluster %d, offset %d\n", len, cluster, offset);

    size_t sector_num = DIV_ROUNDUP(offset + len, id->bs.bytes_per_sector);
    uint8_t* dd = (uint8_t*)kmalloc(sector_num * id->bs.bytes_per_sector);

    uint32_t temp_cluster = cluster;
    size_t temp_readlen = 0;
    while (true) {
        ata_pio_read28(id->device,
                       id->bs.cluster_begin_lba + (temp_cluster - 2) * id->bs.sectors_per_cluster,
                       id->bs.sectors_per_cluster,
                       &dd[temp_readlen]);
        temp_readlen += id->bs.bytes_per_sector * id->bs.sectors_per_cluster;
        temp_cluster = fat32_get_next_cluster(temp_cluster, id->fat, id->fat_len);
        if (temp_cluster == 0) break;
        klogi("FAT32:                      cluster %d, bytes per cluster %d\n",
              temp_cluster, id->bs.bytes_per_sector * id->bs.sectors_per_cluster);
    }

    size_t retlen = MIN(sector_num * id->bs.bytes_per_sector - offset, len);
    memcpy(buff, &dd[offset], retlen);
    kmfree(dd);

    return retlen;
}

int64_t fat32_write(vfs_inode_t* this, size_t offset, size_t len, const void* buff)
{
    fat32_ident_t* id = (fat32_ident_t*)this->ident;

    uint32_t cluster = id->entry.cluster_begin;

    size_t sector_num = DIV_ROUNDUP(offset + len, id->bs.bytes_per_sector);
    uint8_t* dd = (uint8_t*)kmalloc(sector_num * id->bs.bytes_per_sector);

    fat32_read(this, 0, sector_num * id->bs.bytes_per_sector, dd);
    memcpy(dd + offset, buff, len);

    klogi("FAT32: Write %4d bytes from cluster %d, offset %d\n",
          len, cluster, offset);

    uint32_t temp_cluster = cluster;
    size_t temp_writelen = 0;

    while (true) {
        klogv("FAT32: Write cluster %04d:%04d\n",
              id->bs.cluster_begin_lba, cluster);
        ata_pio_write28(id->device,
                id->bs.cluster_begin_lba 
                    + (cluster - 2) * id->bs.sectors_per_cluster,
                id->bs.sectors_per_cluster,
                &dd[temp_writelen]);

        temp_writelen += id->bs.bytes_per_sector * id->bs.sectors_per_cluster;
        if (temp_writelen >= sector_num * id->bs.bytes_per_sector) break;
    
        temp_cluster = fat32_get_next_cluster(cluster, id->fat, id->fat_len);
        if (temp_cluster == 0) {
            /* Allocate a new cluster and update cluster chain in FAT table */
            temp_cluster = fat32_get_free_cluster(id->fat, id->fat_len);

            /* Update FAT Table */
            id->fat[cluster] = temp_cluster;
            id->fat[temp_cluster] = 0x0FFFFFFF;

            size_t fat_sector_no = DIV_ROUNDUP(cluster * 4, id->bs.bytes_per_sector);

            ata_pio_write28(id->device,
                    id->bs.fat_begin_lba + fat_sector_no - 1, 1,
                    (uint8_t*)id->fat
                        + (fat_sector_no - 1) * id->bs.bytes_per_sector);

            size_t temp_fat_sector_no = DIV_ROUNDUP(temp_cluster * 4,
                    id->bs.bytes_per_sector);

            if (temp_fat_sector_no != fat_sector_no) {
                ata_pio_write28(id->device,
                    id->bs.fat_begin_lba + temp_fat_sector_no -1, 1,
                    (uint8_t*)id->fat
                        + (temp_fat_sector_no - 1) * id->bs.bytes_per_sector);
            }
        }
        cluster = temp_cluster;

        klogi("FAT32:                       cluster %d, bytes per cluster %d\n",
              cluster, id->bs.bytes_per_sector * id->bs.sectors_per_cluster);
    } 
    
    size_t retlen = MIN(sector_num * id->bs.bytes_per_sector - offset, len);
    kmfree(dd);
    
    /* Update the file entry */
    if (offset + len > id->entry.file_size_bytes) {
        id->entry.file_size_bytes = offset + len;
        fat32_write_entry (this, &(id->entry));
    }

    return retlen;
}

int64_t fat32_sync(vfs_inode_t* this)
{
    fat32_ident_t* id = (fat32_ident_t*)this->ident;
    (void)id;

    return 0;
}

int64_t fat32_getdent(vfs_inode_t* this, size_t pos, vfs_dirent_t* dirent)
{
    size_t num = 0;

    for (size_t i = 0; i < vec_length(&fat32.filelist); i++) {
        fat32_ident_item_t* item = vec_at(&fat32.filelist, i);
        if ((uint64_t)item->parent != (uint64_t)this) continue;
        if (num == pos) {
            strcpy(dirent->name, item->name);
            memcpy(&dirent->tm, &item->tm, sizeof(tm_t));
            dirent->type = VFS_NODE_FILE;
            if (item->entry.attribute & FAT32_ATTR_DIRECTORY) {
                dirent->type = VFS_NODE_FOLDER;
            }
            return 0;
        }
        num++;
    }

    return -1;
}

int64_t fat32_refresh(vfs_inode_t* this)
{
    fat32_ident_t* id = (fat32_ident_t*)this->ident;

    uint64_t cluster_len = id->bs.sectors_per_cluster * id->bs.bytes_per_sector;
    uint64_t temp_len = cluster_len;
    uint8_t* temp_buffer = (uint8_t*)kmalloc(temp_len);
    uint32_t temp_cluster = id->entry.cluster_begin;

    if (temp_cluster < 2) temp_cluster = 2;

    klogi("FAT32: Read %4d bytes from cluster %d when refreshing (type %d)\n",
            temp_len, temp_cluster, this->type);

    while (true) {
        ata_pio_read28(id->device, id->bs.cluster_begin_lba
                + (temp_cluster - 2) * id->bs.sectors_per_cluster,
                id->bs.sectors_per_cluster, temp_buffer);
        temp_cluster = fat32_get_next_cluster(
                temp_cluster, id->fat, id->fat_len);

        for (size_t i = 0; i < temp_len / sizeof(fat_dir_entry_t);) {
            fat_dir_entry_t *fe, *fe1;

            fe  = (fat_dir_entry_t*)(temp_buffer + i * sizeof(fat_dir_entry_t));
            if (fe->attribute == 0) {
                i++;
                continue;
            }
            fe1  = (fat_dir_entry_t*)(temp_buffer + temp_len - sizeof(fat_dir_entry_t));

            char fn[VFS_MAX_NAME_LEN] = {0};
            bool lfn_meet = false;
            uint8_t lfn_checksum = 0;

            /* Firstly we processed the long file name */
            if (fe->attribute & FAT32_ATTR_LONGNAME) {
                lfn_meet = true;
                lfn_checksum = ((fat_lfn_entry_t*)fe)->dos_checksum;
                size_t count = fat32_get_long_filename(
                        (fat_lfn_entry_t*)fe, (fat_lfn_entry_t*)fe1, fn);
                if (count > 0) {
                    i += count;
                    klogi("FAT32: file long name \"%s\"\n", fn);
                } else {
                    if (temp_cluster == 0) break;
                    if (temp_cluster >= 0xFFFFFFF8) break;

                    /* Need load more data from disk */
                    /* The below code needs further test */
                    uint8_t* buf = (uint8_t*)kmalloc(
                           temp_len + cluster_len - i * sizeof(fat_dir_entry_t));
                    memcpy(buf, &temp_buffer[i * sizeof(fat_dir_entry_t)],
                           temp_len - i * sizeof(fat_dir_entry_t));    
                    ata_pio_read28(id->device, id->bs.cluster_begin_lba
                                   + (temp_cluster - 2) * id->bs.sectors_per_cluster,
                                   id->bs.sectors_per_cluster,
                                   &buf[i * sizeof(fat_dir_entry_t)]);
                    kmfree(temp_buffer);
                    temp_buffer = buf;
                    temp_len += cluster_len - i * sizeof(fat_dir_entry_t);
                    i = 0;
                    /* Continue the loop processing */
                    continue;
                }
            }

            /* Then short file name should be consistent with long name */
            fe  = (fat_dir_entry_t*)(temp_buffer + i * sizeof(fat_dir_entry_t));
            if (strlen(fn) == 0) {
                fat32_get_short_filename ((char*)fe->file_name_and_ext, fn);
            }
            if (!(lfn_meet && lfn_checksum ==
                    fat32_checksum((char*)fe->file_name_and_ext)))
            {
                klogi("FAT32: file attribute %d, name \"%s\"\n", fe->attribute, fn);
            }

            fat32_ident_item_t* item =
                (fat32_ident_item_t*)kmalloc(sizeof(fat32_ident_item_t));
            if (item != NULL) {
                memcpy(&item->entry, fe, sizeof(fat_dir_entry_t));
                strcpy(item->name, fn);
                fat32_get_datetime(fe, &item->tm);
                item->parent = this;
                vec_push_back(&fat32.filelist, (void*)item);
            }
            i++;
        }
        if (temp_cluster == 0) break;
        if (temp_cluster >= 0xFFFFFFF8) break;
        klogi("FAT32:                      cluster 0x%x, bytes per cluster %d\n",
                temp_cluster,
                id->bs.bytes_per_sector * id->bs.sectors_per_cluster);
    }   

    kmfree(temp_buffer);

    return 0;
}

int64_t fat32_mknode(vfs_tnode_t* this)
{
    this->inode->ident = create_ident();
    return 0;
}

int fat32_compare_entry_and_path(fat32_entry_t *ent, const char *path)
{
    const char *ext = strchr(path, '.');

    char name[12];
    memset(name, 0, 12);
    memset(name, ' ', 11);

    if (ext != NULL) {
        memcpy(name, path, MIN(ext - path - 1, 8));
        memcpy(name + 8, ext, MIN(strlen(ext), 3));
    } else {
        memcpy(name, path, MIN(strlen(path), 8));
    }

    for (size_t i = 0; i < 11; i++)
        name[i] = toupper(name[i]);

    return memcmp(ent->name, name, 11);
}

void fat32_dump_entry(fat32_entry_t fe)
{
    if(fe.attribute == 0 && fe.cluster_begin == 0) return;

    klogi("Dump FAT32 file entry information:\n");

    klog_lock();
    char name[12] = {0};
    memcpy(name, fe.name, 11);
    klogu("  Name         : %s\n", name);

    if (fe.attribute & FAT32_ATTR_READ_ONLY)   klogu("  Attribute    : readonly\n");
    if (fe.attribute & FAT32_ATTR_HIDDEN)      klogu("  Attribute    : hidden\n");
    if (fe.attribute & FAT32_ATTR_SYSTEM)      klogu("  Attribute    : system\n");
    if (fe.attribute & FAT32_ATTR_VOLUME_ID)   klogu("  Attribute    : volumeid\n");
    if (fe.attribute & FAT32_ATTR_DIRECTORY)   klogu("  Attribute    : directory\n");
    if (fe.attribute & FAT32_ATTR_ARCHIVE)     klogu("  Attribute    : archive\n");
    if (fe.attribute & FAT32_ATTR_LONGNAME)    klogu("  Attribute    : longname\n");

    klogu("  Cluster begin: %d\n", fe.cluster_begin);
    klogu("  File size    : %d\n", fe.file_size_bytes);
    klogu("  Dent cluster : %d\n", fe.dir_entry_cluster);
    klogu("  Dent index   : %d\n", fe.dir_entry_index);

    klog_unlock();
}

fat32_entry_t fat32_parse_path(vfs_inode_t* this, const char *path)
{
    fat32_ident_t* id = (fat32_ident_t*)this->ident;
    fat32_entry_t root = {
        .name = {' '},
        .attribute = FAT32_ATTR_DIRECTORY,
        .cluster_begin = id->bs.root_dir_first_cluster,
        .file_size_bytes = 0,
        .dir_entry_cluster = 0, /* Need to consider */
        .dir_entry_index = 0
    };

    if (!strcmp(path, "/"))
        return root;

    uint32_t cluster = id->bs.root_dir_first_cluster;
    uint32_t i;

    path++;

    fat32_entry_t ent = {0};
    for (i = 0; ; i++) {
        if (!fat32_read_entry(this, cluster, i, &ent)) {
            break;
        }
        if (i == id->bs.sectors_per_cluster * 16) {
            i = 0;
            cluster = fat32_get_next_cluster(cluster, id->fat, id->fat_len);
            if (cluster * id->bs.sectors_per_cluster + 16 >= id->bs.total_sectors) break;
        }

        if ((ent.attribute & 0x0F) == 0x0F) continue; /* skip lfn */
        if (ent.name[0] == 0xE5) continue;
        if (ent.name[0] == 0x05) ent.name[0] = 0xE5;

        char sub_elem[VFS_MAX_PATH_LEN] = {0};
        int top_level = 0;

        const char *slash = strchr(path, '/');
        if (slash == NULL) {
            memcpy(sub_elem, path, strlen(path));
            top_level = 1;
        } else {
            memcpy(sub_elem, path, slash - path - 1);
            sub_elem[slash - path - 1] = '\0';
            path += strlen(sub_elem) + 1;
        }

        if (fat32_compare_entry_and_path(&ent, sub_elem)) {
            klogv("FAT32: [%s] matches with top level %d\n", sub_elem, top_level);
            if (top_level) {
                /* found file we were looking for */
                return ent;
            } else {
                klogv("FAT32: Found directory entry matching %s\n", sub_elem);
                cluster = ent.cluster_begin;
                i = 0;
            }
        }
    }

    fat32_entry_t err = {0};
    return err;
}

vfs_tnode_t* fat32_open(vfs_inode_t* this, const char* path)
{
    size_t last_idx = 0, cur_idx = 0, dir_count = 0;
    size_t pathlen = strlen(path);
    for (cur_idx = 0; cur_idx < pathlen; cur_idx++) {
        if (path[cur_idx] == '/') {
            if (cur_idx - last_idx > 1) dir_count++;
            if (dir_count == 2) break;
            last_idx = cur_idx;
        }
    }

    char rootpath[VFS_MAX_PATH_LEN];
    strcpy(rootpath, path);
    rootpath[cur_idx] = '\0';
    vfs_tnode_t* rootnode = vfs_path_to_node(rootpath, NO_CREATE, 0);

    klogi("FAT32: Open file %s from %s\n", &path[cur_idx], path);

    fat32_entry_t fe = fat32_parse_path(this, &path[cur_idx]); 

    if (fe.attribute != 0 && fe.cluster_begin != 0) {
        fat32_dump_entry(fe);

        /* Create parent directory */
        size_t i, pathlen = strlen(path);
        for (i = cur_idx + 1; i < pathlen; i++) {
            if (path[i] == '/') {
                static char tmpbuff[VFS_MAX_PATH_LEN];
                strcpy(tmpbuff, path);
                tmpbuff[i] = '\0';
                /* Calling here will not inc refcount */
                fat32_open(this, tmpbuff);
            }
        }

        /* Create corresponding node */ 
        if (fe.attribute & FAT32_ATTR_DIRECTORY) {
            vfs_path_to_node(path, CREATE, VFS_NODE_FOLDER);
        } else {
            vfs_path_to_node(path, CREATE, VFS_NODE_FILE);
        }

        vfs_tnode_t* tnode = vfs_path_to_node(path, NO_CREATE, 0);

        tnode->inode->fs = &fat32;
        tnode->inode->size = fe.file_size_bytes;

        fat32_ident_t* id = create_ident();
        memcpy(id, rootnode->inode->ident, sizeof(fat32_ident_t));
        memcpy(&id->entry, &fe, sizeof(fat32_entry_t));

        tnode->inode->ident = (void*)id;

        return tnode;
    } else {
        return NULL;
    }
}

vfs_inode_t* fat32_mount(vfs_inode_t* at)
{
    vfs_inode_t* ret = vfs_alloc_inode(VFS_NODE_MOUNTPOINT, 0777, 0, &fat32, NULL);
    ret->ident = create_ident();
    fat32_ident_t* id = (fat32_ident_t*)ret->ident;

    /* Get the ATA device */
    ata_device_t* dev = at ? at->ident : NULL;
    if (dev == NULL) {
        kloge("Cannot mount FAT32 partition without a specific device\n");
    } else {
        klogi("FAT32: Mount FAT partition of device %s\n", at->mountpoint ? at->mountpoint->name : "[NULL]");
    }
    id->device = dev;

    /* Read FAT32 partition information */
    /* Notes: To make things simple, here we only consider the situation
     *        that the disk has one FAT32 partition.
     */
    mbr_t mbr;
    ata_pio_read28(dev, 0, 1, (uint8_t*)&mbr);

    if (mbr.signature[0] == 0x55 && mbr.signature[1] == 0xAA) {
        for (int i = 0; i < 4; i++) {
            /* It is a FAT32 partition */
            if (mbr.partitions[i].type == 0x0B
                || mbr.partitions[i].type == 0x0C
                || mbr.partitions[i].type == 0x1C)
            {
                uint8_t dd[512] = {0};
                ata_pio_read28(dev, mbr.partitions[i].lba_start, 1, dd);

                fat_bs_t* fat_boot = (fat_bs_t*)dd;
                if (fat_boot->table_size_16 != 0 || fat_boot->total_sectors_16 != 0) {
                    kpanic("FAT32 file system contains FAT16 parameters\n");
                }
                char vol_name[12] = {0};
                memcpy(vol_name, ((fat_extbs_32_t*)fat_boot->extended_section)->volume_label, 11);
                klogi("Partition %d: [%s] is a FAT32 partition of device 0x%x\n", i, vol_name, dev);

                /* Ref: https://www.pjrc.com/tech/8051/ide/fat32.html */
                id->bs.bytes_per_sector       = fat_boot->bytes_per_sector;
                id->bs.sectors_per_cluster    = fat_boot->sectors_per_cluster;
                id->bs.reserved_sector_count  = fat_boot->reserved_sector_count;
                id->bs.num_fats               = fat_boot->table_count;
                id->bs.sectors_per_fat        = ((fat_extbs_32_t*)fat_boot->extended_section)->table_size_32;
                id->bs.root_dir_first_cluster = ((fat_extbs_32_t*)fat_boot->extended_section)->root_cluster;
                id->bs.total_sectors          = fat_boot->total_sectors_32;

                klogi("Partition %d: OEM name %s, bytes per sector %d, sectors per cluster %d, "
                      "number of reserved sectors 0x%02x, number of FATs %d, "
                      "sectors per FAT %d, root directory first cluster 0x%2x\n",
                      i, fat_boot->oem_name, id->bs.bytes_per_sector, id->bs.sectors_per_cluster,
                      id->bs.reserved_sector_count, id->bs.num_fats,
                      id->bs.sectors_per_fat, id->bs.root_dir_first_cluster);

                /* FAT-1 sector number */
                id->bs.fat_begin_lba = mbr.partitions[i].lba_start + id->bs.reserved_sector_count;
                /* Cluster sector number */
                id->bs.cluster_begin_lba = id->bs.fat_begin_lba + id->bs.num_fats * id->bs.sectors_per_fat;

                id->fat_len = id->bs.sectors_per_fat * id->bs.bytes_per_sector;
                id->fat = (uint32_t*)kmalloc(id->fat_len);
                klogi("FAT32: Read FAT table from %d len %d\n", id->bs.fat_begin_lba, id->bs.sectors_per_fat);
                ata_pio_read28(id->device, id->bs.fat_begin_lba, id->bs.sectors_per_fat, (void*)id->fat);

                for (size_t m = 0; m < 20; m += 4) {
                    klogi("FAT32: [%04d] 0x%08x 0x%08x 0x%08x 0x%08x\n",
                          m, id->fat[m], id->fat[m+1], id->fat[m+2], id->fat[m+3]);
                }

                /* Finished reading all needed FAT32 partition information */
                break;
            }
        }
    }

    klogi("FAT32: Mount partition finished\n");
    return ret;
}
