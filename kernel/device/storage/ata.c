#include <core/pci.h>
#include <core/cpu.h>
#include <core/isr_base.h>
#include <device/storage/ata.h>
#include <core/panic.h>
#include <core/pit.h>
#include <lib/lock.h>
#include <lib/klog.h>
#include <lib/string.h>
#include <lib/kmalloc.h>
#include <lib/memutils.h>
#include <proc/sched.h>
#include <fs/filebase.h>
#include <fs/vfs.h>
#include <fs/fat32.h>

#define ATA_SECTOR_SIZE     512

static char ata_drive_char = 'a';
static int  cdrom_number = 0;

typedef union {
    uint8_t command_bytes[12];
    uint16_t command_words[6];
} atapi_command_t;

static ata_device_t ata_primary_master   = {.io_base = 0x1F0, .control = 0x3F6, .slave = 0};
static ata_device_t ata_primary_slave    = {.io_base = 0x1F0, .control = 0x3F6, .slave = 1};
static ata_device_t ata_secondary_master = {.io_base = 0x170, .control = 0x376, .slave = 0};
static ata_device_t ata_secondary_slave  = {.io_base = 0x170, .control = 0x376, .slave = 1};

static lock_t ata_lock;

/* Function Definition */
static int ata_read_partition_map(ata_device_t* dev, char* devname);
static uint64_t ata_max_offset(ata_device_t * dev);

static void ata_io_wait(ata_device_t* dev)
{
    /* Delay 400 nanosecond for BSY to be set:
     * Reading the Alternate Status port wastes 100ns; loop four times.
     */
    port_inb(dev->io_base + ATA_REG_ALTSTATUS);
    port_inb(dev->io_base + ATA_REG_ALTSTATUS);
    port_inb(dev->io_base + ATA_REG_ALTSTATUS);
    port_inb(dev->io_base + ATA_REG_ALTSTATUS);
}

static void ata_soft_reset(ata_device_t* dev)
{
    port_outb(dev->control, 0x04);
    port_outb(dev->control, 0x00);
}

static void ata_poll(ata_device_t *dev, int advanced_check)
{
    ata_io_wait(dev);

    uint8_t s = port_inb(dev->io_base + ATA_REG_STATUS);
    while(s & ATA_SR_BSY) {
        port_inb(dev->io_base + ATA_REG_ALTSTATUS); /* 100ns */
        s = port_inb(dev->io_base + ATA_REG_STATUS);
    }

    if (advanced_check) {
        while (true) {
            port_inb(dev->io_base + ATA_REG_ALTSTATUS); /* 100ns */
            s = port_inb(dev->io_base + ATA_REG_STATUS);
            if ((s & ATA_SR_ERR) || (s & ATA_SR_DF)) {
                ata_io_wait(dev);
                uint8_t err = port_inb(dev->io_base + ATA_REG_ERROR);
                kpanic("ATA: Device error code %d\n", err);
            }
            if (s & ATA_SR_DRQ) {
                break;
            }
            ata_io_wait(dev);
        }
    }
}

static int ata_device_init(ata_device_t * dev)
{
    klogi("Initializing IDE device on bus %d\n", dev->io_base);

    uint16_t bus = dev->io_base;
    uint8_t slave = dev->slave;

    port_inb(bus + ATA_REG_STATUS);
    port_outb(bus + ATA_REG_ALTSTATUS, 0);

    ata_io_wait(dev);

    port_inb(bus + ATA_REG_STATUS);
    port_outb(bus + ATA_REG_HDDEVSEL, 0xA0 | slave << 4);

    ata_io_wait(dev);

    port_inb(bus + ATA_REG_STATUS);
    port_outb(bus + ATA_REG_SECCOUNT0, 0);
    port_inb(bus + ATA_REG_STATUS);
    port_outb(bus + ATA_REG_LBA0, 0);
    port_inb(bus + ATA_REG_STATUS);
    port_outb(bus + ATA_REG_LBA1, 0);
    port_inb(bus + ATA_REG_STATUS);
    port_outb(bus + ATA_REG_LBA2, 0);
    port_inb(bus + ATA_REG_STATUS);
    
    port_outb(bus + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    
    ata_io_wait(dev);

    /* Read the status port. If it's zero, the drive does not exist. */
    uint8_t status = port_inb(bus + ATA_REG_STATUS);
    if(status == 0) return 0;
    klogi("Device status: %02x\n", status);

    klogi("Waiting for ERR or DRQ.\n");
    /* Wait for ERR or DRQ */
    {
        int timer = 0xFFFFFF;
        while (timer--) {
            status = port_inb(bus + ATA_REG_STATUS);
            if (status & ATA_SR_ERR) {
                return 0;
            }
            if (!(status & ATA_SR_BSY) && status & ATA_SR_DRQ)
                goto c1;
        }
        return 0;
    }

c1:
    {
        uint8_t mid = port_inb(bus + ATA_REG_LBA1);
        uint8_t hi = port_inb(bus + ATA_REG_LBA2);
        if(mid || hi) {
            /* The drive is not ATA. */
            return 0;
        }
   
        int timer2 = 0xFFFFFF;
        while (timer2--) {
            status = port_inb(bus + ATA_REG_STATUS);
            if ((status & ATA_SR_ERR))
                return 0;
            if ((status & ATA_SR_DRQ))
                goto c2;
        }
        return 0;
    }

c2:
    status = port_inb(bus + ATA_REG_STATUS);
    klogi("Device status: %02x\n", status);
    klogi("Reading IDENTIFY structure.\n");

    uint16_t * buf = (uint16_t *)&dev->identity;
    port_insw(bus + ATA_REG_DATA, buf, 256);
    dev->is_atapi = 0;

    uint8_t* ptr = (uint8_t *)dev->identity.model;
    for (int i = 0; i < 39; i+=2) {
        uint8_t tmp = ptr[i+1];
        ptr[i+1] = ptr[i];
        ptr[i] = tmp;
    }
    ptr[39] = '\0';

    ptr = (uint8_t *)dev->identity.serial;
    for (int i = 0; i < 19; i+=2) {
        uint8_t tmp = ptr[i+1];
        ptr[i+1] = ptr[i];
        ptr[i] = tmp;
    }
    ptr[19] = '\0';

    klogi("Device name : %s\n", dev->identity.model);
    klogi("Serial no   : %s\n", dev->identity.serial);
    klogi("Sectors (48): %d\n", (uint32_t)dev->identity.sectors_48);
    klogi("Sectors (24): %d\n", dev->identity.sectors_28);
    klogi("Max offset  : %d\n", ata_max_offset(dev));

    port_outb(dev->io_base + ATA_REG_CONTROL, 0x02);

    return 1;
}

static int atapi_device_init(ata_device_t * dev) {
    dev->is_atapi = 1;

    port_outb(dev->io_base + 1, 1);
    port_outb(dev->control, 0);

    ata_io_wait(dev);

    uint16_t bus = dev->io_base;
    uint8_t slave = dev->slave;

    port_inb(bus + ATA_REG_STATUS);
    port_outb(bus + ATA_REG_HDDEVSEL, 0xA0 | slave << 4);
    port_inb(bus + ATA_REG_STATUS);
    port_outb(bus + ATA_REG_SECCOUNT0, 0);
    port_inb(bus + ATA_REG_STATUS);
    port_outb(bus + ATA_REG_LBA0, 0);
    port_inb(bus + ATA_REG_STATUS);
    port_outb(bus + ATA_REG_LBA1, 0);
    port_inb(bus + ATA_REG_STATUS);
    port_outb(bus + ATA_REG_LBA2, 0);
    port_inb(bus + ATA_REG_STATUS);
    
    port_outb(bus + ATA_REG_COMMAND, ATA_CMD_IDENTIFY_PACKET);

    ata_io_wait(dev);
    
    /* Read the status port. If it's zero, the drive does not exist. */
    uint8_t status = port_inb(bus + ATA_REG_STATUS);
    klogi("Waiting for status.\n");
    while(status & ATA_SR_BSY) {
        uint32_t i = 0;
        for(i = 0; i < 0x0FFFFFFF; i++) {}
        status = port_inb(bus + ATA_REG_STATUS);
    }
    
    if(status == 0) return 0;

    klogi("Status indicates presence of a drive. Polling while STAT_BSY...\n");
    while(status & ATA_SR_BSY) {
        status = port_inb(bus + ATA_REG_STATUS);
    }
    klogi("Done.\n");

    uint8_t mid = port_inb(bus + ATA_REG_LBA1);
    uint8_t hi = port_inb(bus + ATA_REG_LBA2);
    if(mid || hi) {
        return 0;
    }

    klogi("Waiting for ERR or DRQ.\n");
    /* Wait for ERR or DRQ */
    while(!(status & (ATA_SR_ERR | ATA_SR_DRQ))) {
        status = port_inb(bus + ATA_REG_STATUS);
    }

    if(status & ATA_SR_ERR) {
        /* There was an error on the drive. Forget about it. */
        return 0;
    }

    klogi("Device status: %02x\n", status);
    klogi("Reading IDENTIFY structure.\n");

    uint16_t * buf = (uint16_t *)&dev->identity;
    port_insw(bus + ATA_REG_DATA, buf, 256);
    dev->is_atapi = 0;

    uint8_t* ptr = (uint8_t *)dev->identity.model;
    for (int i = 0; i < 39; i+=2) {
        uint8_t tmp = ptr[i+1];
        ptr[i+1] = ptr[i];
        ptr[i] = tmp;
    }
    ptr[39] = '\0';

    ptr = (uint8_t *)dev->identity.serial;
    for (int i = 0; i < 19; i+=2) {
        uint8_t tmp = ptr[i+1];
        ptr[i+1] = ptr[i];
        ptr[i] = tmp;
    }
    ptr[19] = '\0';

    klogi("Device name:  %s\n", dev->identity.model);
    klogi("Serial no:    %s\n", dev->identity.serial);

    /* Detect medium */
    atapi_command_t command;
    command.command_bytes[0] = 0x25;
    command.command_bytes[1] = 0;
    command.command_bytes[2] = 0;
    command.command_bytes[3] = 0;
    command.command_bytes[4] = 0;
    command.command_bytes[5] = 0;
    command.command_bytes[6] = 0;
    command.command_bytes[7] = 0;
    command.command_bytes[8] = 0; /* bit 0 = PMI (0, last sector) */
    command.command_bytes[9] = 0; /* control */
    command.command_bytes[10] = 0;
    command.command_bytes[11] = 0;

    port_outb(bus + ATA_REG_FEATURES, 0x00);
    port_outb(bus + ATA_REG_LBA1,  0x08);
    port_outb(bus + ATA_REG_LBA2, 0x08);
    port_outb(bus + ATA_REG_COMMAND, ATA_CMD_PACKET);

    /* poll */
    while (1) {
        uint8_t status = port_inb(dev->io_base + ATA_REG_STATUS);
        if ((status & ATA_SR_ERR)) goto atapi_error;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRDY)) break;
    }

    for (int i = 0; i < 6; ++i) {
        port_outw(bus, command.command_words[i]);
    }

    /* poll */
    while (1) {
        uint8_t status = port_inb(dev->io_base + ATA_REG_STATUS);
        if ((status & ATA_SR_ERR)) goto atapi_error_read;
        if (!(status & ATA_SR_BSY) && (status & ATA_SR_DRDY)) break;
        if ((status & ATA_SR_DRQ)) break;
    }

    uint16_t data[4];
    port_insw(bus, data, 4);

#define htonl(l)  ( (((l) & 0xFF) << 24) | (((l) & 0xFF00) << 8) | (((l) & 0xFF0000) >> 8) | (((l) & 0xFF000000) >> 24))
    uint32_t lba, blocks;;
    memcpy(&lba, &data[0], sizeof(uint32_t));
    lba = htonl(lba);
    memcpy(&blocks, &data[2], sizeof(uint32_t));
    blocks = htonl(blocks);

    dev->atapi_lba = lba;
    dev->atapi_sector_size = blocks;

    if (!lba) return 1;

    klogi("Finished! LBA = %x; block length = %x\n", lba, blocks);
    return 1;

atapi_error_read:
    kloge("ATAPI error; no medium?\n");
    return 0;

atapi_error:
    kloge("ATAPI early error; unsure\n");
    return 0;
}

static int ata_device_detect(ata_device_t * dev) {
    ata_soft_reset(dev);
    port_outb(dev->io_base + ATA_REG_HDDEVSEL, 0xA0 | dev->slave << 4);
    ata_io_wait(dev);
    
    unsigned char cl = port_inb(dev->io_base + ATA_REG_LBA1); /* CYL_LO */
    unsigned char ch = port_inb(dev->io_base + ATA_REG_LBA2); /* CYL_HI */

    klogi("Device detected: %2x %2x\n", cl, ch);
    if (cl == 0xFF && ch == 0xFF) {
        /* Nothing here */
        return 0;
    }
    if ((cl == 0x00 && ch == 0x00) ||
        (cl == 0x3C && ch == 0xC3)) {
        /* Parallel ATA device, or emulated SATA */
        if(!ata_device_init(dev)) {
            klogw("Init ATA device failed\n");
            return 0;
        }

        char devname[64];
        char devchar[2] = {ata_drive_char, '\0'};
        strcpy((char *)&devname, "/dev/hd");
        strcat((char *)&devname, (char *)&devchar);

        vfs_tnode_t* tnode = vfs_path_to_node(devname, CREATE, VFS_NODE_BLOCK_DEVICE);
        vfs_inode_t* inode = vfs_alloc_inode(VFS_NODE_BLOCK_DEVICE, 0777, 0, NULL, tnode);
        tnode->inode = inode;
        inode->ident = (void*)dev;
         
        ata_read_partition_map(dev, devname);

        ata_drive_char++;
        return 1;
    } else if ((cl == 0x14 && ch == 0xEB) ||
               (cl == 0x69 && ch == 0x96)) {
        klogi("Detected ATAPI device at io-base %3x, control %3x, slave %d\n", dev->io_base, dev->control, dev->slave);

        if (!atapi_device_init(dev)) {
            klogw("Init ATAPI device (maybe a CDROM) failed\n");
            return 0;
        }

        char devname[64];
        char devchar[2] = {(char)('1' + cdrom_number), '\0'};
        strcpy((char *)&devname, "/dev/cdrom");
        strcat((char *)&devname, (char *)&devchar);

        vfs_tnode_t* tnode = vfs_path_to_node(devname, CREATE, VFS_NODE_BLOCK_DEVICE);
        vfs_inode_t* inode = vfs_alloc_inode(VFS_NODE_BLOCK_DEVICE, 0777, 0, NULL, tnode);
        tnode->inode = inode;
        inode->ident = (void*)dev;

        cdrom_number++;
        return 2;
    }

    /* TODO: ATAPI, SATA, SATAPI */
    return 0;
}

int ata_init(void)
{
    ata_device_detect(&ata_primary_master);
    ata_device_detect(&ata_primary_slave);

    ata_device_detect(&ata_secondary_master);
    ata_device_detect(&ata_secondary_slave);

    return 1;
}

static uint64_t ata_max_offset(ata_device_t * dev)
{
    uint64_t sectors = dev->identity.sectors_48;
    if (!sectors) {
        /* Fall back to sectors_28 */
        sectors = dev->identity.sectors_28;
    }

    return sectors * ATA_SECTOR_SIZE;
}

void ata_pio_read28(ata_device_t* dev, uint32_t lba, uint8_t sector_count, uint8_t* target)
{
    uint16_t bus = dev->io_base;
    uint8_t slave = dev->slave;

    ata_io_wait(dev);

    port_outb(bus + ATA_REG_HDDEVSEL,  0xE0 | slave << 4 | ((lba & 0x0f000000) >> 24));
    ata_io_wait(dev);
   
    port_outb(bus + ATA_REG_ERROR, 0x00);
    port_outb(bus + ATA_REG_SECCOUNT0, sector_count);
    port_outb(bus + ATA_REG_LBA0, (lba & 0x000000ff) >>  0);
    port_outb(bus + ATA_REG_LBA1, (lba & 0x0000ff00) >>  8);
    port_outb(bus + ATA_REG_LBA2, (lba & 0x00ff0000) >> 16);
    port_outb(bus + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    for(uint64_t i = 0; i < sector_count; i++) {
        ata_poll(dev, 1);

        /* Transfer the data! */
        port_insw(bus + ATA_REG_DATA, (void *)target, 256);
        target += 512;
        ata_io_wait(dev);
    }

    ata_poll(dev, 0);
}

void ata_pio_write28(ata_device_t* dev, uint32_t lba, uint8_t sector_count, uint8_t* source)
{
    uint16_t bus = dev->io_base;
    uint8_t slave = dev->slave;

    ata_io_wait(dev);

    port_outb(bus + ATA_REG_HDDEVSEL,  0xE0 | slave << 4 | ((lba & 0x0f000000) >> 24));
    ata_io_wait(dev);

    port_outb(bus + ATA_REG_ERROR, 0x00);
    port_outb(bus + ATA_REG_SECCOUNT0, sector_count);
    port_outb(bus + ATA_REG_LBA0, (lba & 0x000000ff) >>  0); 
    port_outb(bus + ATA_REG_LBA1, (lba & 0x0000ff00) >>  8); 
    port_outb(bus + ATA_REG_LBA2, (lba & 0x00ff0000) >> 16);
    port_outb(bus + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    ata_io_wait(dev);

    for(uint64_t i = 0; i < sector_count; i++) {
        ata_poll(dev, 1);

        /* Transfer the data! */
        uint16_t* od = (uint16_t*)source;
        for (size_t idx = 0; idx < 256; idx++) {
            port_outw(bus + ATA_REG_DATA, od[idx]);
        }
        source += 512;
        ata_io_wait(dev);
    }

    ata_poll(dev, 0);
    port_outb(bus + ATA_REG_COMMAND, ATA_CMD_CACHE_FLUSH);
    ata_poll(dev, 0);
}

static int ata_read_partition_map(ata_device_t* dev, char* devname)
{
    mbr_t mbr;
    ata_pio_read28(dev, 0, 1, (uint8_t*)&mbr);

    if (mbr.signature[0] == 0x55 && mbr.signature[1] == 0xAA) {
        klogi("Partition table found.\n");

        klogi(         "              status, type, lba 1st sector, sector count\n");
        for (int i = 0; i < 4; ++i) {
            if (mbr.partitions[i].status & 0x80) {
                klogi("#%2d: @%d + %d\n", i+1, mbr.partitions[i].lba_start, mbr.partitions[i].sector_count);
            } else {
                klogi("#%2d: inactive     %02x,   %02x, %14d, %12d\n", i + 1, 
                            mbr.partitions[i].status, mbr.partitions[i].type,
                            mbr.partitions[i].lba_start, mbr.partitions[i].sector_count);
            }
        }

        for (int i = 0; i < 4; ++i) {
            /* It is a FAT32 partition */
            if (mbr.partitions[i].type == 0x0B
                || mbr.partitions[i].type == 0x0C
                || mbr.partitions[i].type == 0x1C)
            {
                char partition_name[2], partition_path[VFS_MAX_PATH_LEN];
                partition_name[0] = '0' + i;
                partition_name[1] = '\0';

                strcpy(partition_path, "/disk/");
                strcat(partition_path, partition_name);

                vfs_path_to_node(partition_path, CREATE, VFS_NODE_FOLDER);
                vfs_mount(devname, partition_path, "fat32");
                break;
            }
        }

        klogi("ATA: Reading partitions of %s finished\n", devname);
        return 0;
    } else {
        kloge("Did not find partition table.\n");
        kloge("Signature was %02x %02x instead of 0x55 0xAA\n", mbr.signature[0], mbr.signature[1]);

        kloge("Parsing anyone yields:\n");

        for (int i = 0; i < 4; ++i) {
            if (mbr.partitions[i].status & 0x80) {
                klogi("#%d: @%d+%d\n", i+1, mbr.partitions[i].lba_start, mbr.partitions[i].sector_count);
            } else {
                klogi("#%d: inactive\n", i+1);
            }
        }
    }

    return 1;
}

