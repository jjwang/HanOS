/**-----------------------------------------------------------------------------

 @file    fat32.h
 @brief   Definition of FAT32 related data structures and functions
 @details
 @verbatim

  FAT 32 was introduced to us by Windows95-B and Windows98. FAT32 solved some of
  FAT's problems. No more 64K max clusters! Although FAT32 uses 32 bits per FAT
  entry, only the bottom 28 bits are actually used to address clusters on the
  disk (top 4 bits are reserved). With 28 bits per FAT entry, the filesystem can
  address a maximum of about 270 million clusters in a partition. This enables
  very large hard disks to still maintain reasonably small cluster sizes and
  thus reduce slack space between files.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#pragma once

#include <libc/string.h>

#include <fs/vfs.h>
#include <base/lock.h>
#include <base/time.h>
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
    uint8_t file_data_1[2];
    uint16_t create_time;       /* 0~4 sec(0~29 * 2), 5~10 min, 11~15 hour */
    uint16_t create_date;       /* 0~4 day, 5~8 mon, 9~15 year (1980+) */
    uint16_t last_visit_date;
    uint16_t cluster_num_high;  /* first cluster high 16 bits */
    uint16_t modify_time;
    uint16_t modify_date;
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
    uint8_t  name[11];
    uint8_t  attribute;
    uint32_t cluster_begin;
    uint32_t file_size_bytes;

    uint32_t dir_entry_cluster;
    size_t dir_entry_index;
} fat32_entry_t;

typedef struct {
    ata_device_t* device;
    lock_t lock;
    fat32_bs_info_t bs; 
    fat32_entry_t entry; 

    uint32_t* fat;
    size_t fat_len;
} fat32_ident_t;

typedef struct {
    fat_dir_entry_t entry;
    tm_t tm;
    char name[VFS_MAX_NAME_LEN];
    vfs_inode_t* parent;
} fat32_ident_item_t;

extern vfs_fsinfo_t fat32;

vfs_inode_t* fat32_mount(vfs_inode_t* at);
vfs_tnode_t* fat32_open(vfs_inode_t* this, const char* path);
int64_t fat32_mknode(vfs_tnode_t* this);
int64_t fat32_read(vfs_inode_t* this, size_t offset, size_t len, void* buff);
int64_t fat32_write(vfs_inode_t* this, size_t offset, size_t len, const void* buff);
int64_t fat32_sync(vfs_inode_t* this);
int64_t fat32_refresh(vfs_inode_t* this);
int64_t fat32_getdent(vfs_inode_t* this, size_t pos, vfs_dirent_t* dirent);

static inline uint32_t fat32_get_next_cluster(
        uint32_t cluster, uint32_t *fat, uint32_t fat_len)
{
    if (cluster >= fat_len / 4) return 0;
    if (fat[cluster] >= 0xFFFFFFF8) return 0;
    if (fat[cluster] == 0x0FFFFFFF) return 0;
    return fat[cluster];
}

static inline uint32_t fat32_get_free_cluster(uint32_t *fat, uint32_t fat_len)
{
    for (size_t i = 0; i < fat_len; i++) {
        if (fat[i] == 0) return i;
    }
    return 0;
}

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

/* The entry sequence (idx, first) is (3, 1), (2, 0), (1, 0) */
static inline uint32_t fat32_get_long_filename(
    fat_lfn_entry_t* lfne, fat_lfn_entry_t* lfne_last, char* fname)
{
    char fn_temp[VFS_MAX_NAME_LEN] = {0};
    uint32_t count = 0, entry_num = 0;

    fname[0] = '\0';

    while(true) {
        strcpy(fn_temp, fname);
        count++;

        uint32_t idx = lfne->sequence_number & 0x3F;
        bool first = lfne->sequence_number & 0x40;
        fat32_name_copy(fname, lfne->name1, 5);
        fat32_name_copy(&fname[5], lfne->name2, 6);
        fat32_name_copy(&fname[11], lfne->name3, 2);
        fname[13] = '\0';

        strcat(fname, fn_temp);

        if (first) entry_num = idx;
        if (idx == 1 && count == entry_num) return count;

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

static inline void fat32_get_datetime(fat_dir_entry_t* de, tm_t *t)
{
    int year  = ((de->modify_date & 0xFE00) >> 9) + 1980;
    int month = (de->modify_date & 0x01E0) >> 5;
    int day   = de->modify_date & 0x001F; 
    int hour  = ((de->modify_time & 0xF800) >> 11);
    int min   = (de->modify_time & 0x07E0) >> 5;
    int sec   = (de->modify_time & 0x001F) * 2;

    time_t create_time = secs_of_years(year - 1) +
            secs_of_month(month - 1, year) + (day - 1) * 86400 +
            hour * 3600 + min * 60 + sec;

    localtime(&create_time, t);
}
