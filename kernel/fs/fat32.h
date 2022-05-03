#pragma once

#include <fs/vfs.h>
#include <lib/memutils.h>
#include <lib/string.h>
#include <lib/lock.h>
#include <device/storage/ata.h>

#define FAT32_ATTR_READ_ONLY            0x01
#define FAT32_ATTR_HIDDEN               0x02
#define FAT32_ATTR_SYSTEM               0x04
#define FAT32_ATTR_VOLUME_ID            0x08
#define FAT32_ATTR_DIRECTORY            0x10
#define FAT32_ATTR_ARCHIVE              0x20
#define FAT32_ATTR_LONGNAME             0x0F

typedef struct [[gnu::packed]] {
    uint32_t table_size_32;
    uint16_t extended_flags;
    uint16_t fat_version;
    uint32_t root_cluster;
    uint16_t fat_info;
    uint16_t backup_BS_sector;
    uint8_t  reserved_0[12];
    uint8_t  drive_number;
    uint8_t  reserved_1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fat_type_label[8]; 
} fat_extbs_32_t;

typedef struct [[gnu::packed]] {
    uint8_t  bios_drive_num;
    uint8_t  reserved1;
    uint8_t  boot_signature;
    uint32_t volume_id;
    uint8_t  volume_label[11];
    uint8_t  fat_type_label[8]; 
} fat_extbs_16_t;

typedef struct [[gnu::packed]] {
    uint8_t  bootjmp[3];
    uint8_t  oem_name[8];
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t  table_count;
    uint16_t root_entry_count;
    uint16_t total_sectors_16;
    uint8_t  media_type;
    uint16_t table_size_16;
    uint16_t sectors_per_track;
    uint16_t head_side_count;
    uint32_t hidden_sector_count;
    uint32_t total_sectors_32;
    uint8_t  extended_section[54];
} fat_bs_t;

typedef struct [[gnu::packed]] {
    char file_name_and_ext[8 + 3];
    uint8_t attribute;
    uint8_t file_data_1[8];
    uint16_t cluster_num_high;  /* first cluster high 16 bits */
    uint8_t file_data_2[4];
    uint16_t cluster_num_low;   /* first cluster low 16 bits */
    uint32_t file_size_bytes;   /* file size (in bytes) */
} fat_dir_entry_t;

typedef struct [[gnu::packed]] {
    uint8_t sequence_number;
    char name1[10];
    uint8_t attribute;
    uint8_t type;               /* Reserved for future usage */
    uint8_t dos_checksum;
    char name2[12];
    uint16_t first_cluster;     /* Always be zero */
    char name3[4];
} fat_lfn_entry_t;

/* The below are self-defined data structures */
typedef struct {
    uint16_t bytes_per_sector;
    uint8_t  sectors_per_cluster;
    uint16_t reserved_sector_count;
    uint8_t  num_fats;
    uint32_t sectors_per_fat;
    uint32_t root_dir_first_cluster;
    uint32_t fat_begin_lba;
    uint32_t cluster_begin_lba;
    uint32_t total_sectors;
} fat32_bs_info_t;

typedef struct {
    uint32_t cluster_offset;
} fat32_rw_info_t;

typedef struct {
    ata_device_t* device;
    lock_t lock;
    fat32_bs_info_t bs;
    fat32_rw_info_t rw;

    size_t alloc_size;
    void* data;
} fat32_ident_t;

typedef struct {
    uint8_t  name[11];
    uint8_t  attribute;
    uint32_t cluster_begin;
    uint32_t file_size_bytes;
} fat32_entry_t;

extern vfs_fsinfo_t fat32;

vfs_inode_t* fat32_mount(vfs_inode_t* at);
vfs_tnode_t* fat32_open(vfs_inode_t* this, const char* path);
int64_t fat32_mknode(vfs_tnode_t* this);
int64_t fat32_read(vfs_inode_t* this, size_t offset, size_t len, void* buff);
int64_t fat32_write(vfs_inode_t* this, size_t offset, size_t len, const void* buff);
int64_t fat32_sync(vfs_inode_t* this);
int64_t fat32_refresh(vfs_inode_t* this);

static inline void fat32_get_short_filename(char* file_name_and_ext, char* fname)
{
    int64_t i;
    char fn[9] = {0}, ext[4] = {0};

    memcpy(fn, file_name_and_ext, 8);
    memcpy(ext, &(file_name_and_ext[8]), 3);

    i = 0;
    while(true) {
        if (fn[i] == 0x20 || fn[i] == '\0') {
            fn[i] = '\0';
            break;
        }
        i++;
    }

    i = 0;
    while(true) {
        if (ext[i] == 0x20 || ext[i] == '\0') {
            ext[i] = '\0';
            break;
        }
        i++;
    }

    strcpy(fname, fn);
    if (strlen(ext) > 0) {
        strcat(fname, ".");
        strcat(fname, ext);
    }
}

static inline void fat32_name_copy(char *dst, char *src, int len)
{
    while (len--) {
        *dst++ = *src++;
        ++src;
    }
}

static inline uint32_t fat32_get_long_filename(
    fat_lfn_entry_t* lfne, fat_lfn_entry_t* lfne_last, char* fname)
{
    char fn_temp[VFS_MAX_NAME_LEN] = {0};
    uint32_t last_idx = 0, entry_num = 0, count = 0;

    while(true) {
        strcpy(fn_temp, fname);
        count++;

        uint32_t idx = lfne->sequence_number & 0x3F;
        bool first = lfne->sequence_number & 0x40;
        fat32_name_copy(fname, lfne->name1, 5);
        fat32_name_copy(&fname[5], lfne->name2, 6);
        fat32_name_copy(&fname[11], lfne->name3, 2);

        strcat(fname, fn_temp);
        if (first) entry_num = idx;

        if (idx == 1 && count == entry_num)      return count;
        if (last_idx > 0 && idx != last_idx - 1) return 1;
        if (idx == 1 && count != entry_num)      return count;

        last_idx = idx;
        if ((uint64_t)lfne == (uint64_t) lfne_last) break; 
        lfne = (fat_lfn_entry_t*)((uint64_t)lfne + sizeof(fat_lfn_entry_t));
    }

    return 0;
}

static inline uint8_t fat32_checksum(const char *name)
{
    uint8_t s = name[0];
    s = (s<<7) + (s>>1) + name[1];
    s = (s<<7) + (s>>1) + name[2];
    s = (s<<7) + (s>>1) + name[3];
    s = (s<<7) + (s>>1) + name[4];
    s = (s<<7) + (s>>1) + name[5];
    s = (s<<7) + (s>>1) + name[6];
    s = (s<<7) + (s>>1) + name[7];
    s = (s<<7) + (s>>1) + name[8];
    s = (s<<7) + (s>>1) + name[9];
    s = (s<<7) + (s>>1) + name[10];
    return s;
}
