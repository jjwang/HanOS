/**-----------------------------------------------------------------------------

 @file    block.c
 @brief   Userspace ATA PIO block server

 @details
 @verbatim

   Owns the ATA PIO ports granted by the kernel and answers BLOCK_GET_INFO /
   BLOCK_READ / BLOCK_WRITE over IPC. Port I/O goes through the range-checked
   IOPORT_ACCESS syscall. The server never dereferences client pointers: bulk
   data travels in memory objects.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <libc/bootinfo.h>
#include <libc/protocol.h>
#include <libc/string.h>
#include <libc/sysfunc.h>

#define ATA_IO_BASE     0x1F0
#define ATA_CTRL_BASE   0x3F6

#define ATA_REG_DATA        0
#define ATA_REG_ERROR       1
#define ATA_REG_SECCOUNT0   2
#define ATA_REG_LBA0        3
#define ATA_REG_LBA1        4
#define ATA_REG_LBA2        5
#define ATA_REG_HDDEVSEL    6
#define ATA_REG_COMMAND     7
#define ATA_REG_STATUS      7
#define ATA_REG_CONTROL     0x206       /* 0x3F6 - 0x1F0 */

#define ATA_SR_BSY          0x80
#define ATA_SR_DRQ          0x08
#define ATA_SR_ERR          0x01

#define ATA_CMD_IDENTIFY    0xEC
#define ATA_CMD_READ_PIO    0x20
#define ATA_CMD_WRITE_PIO   0x30

#define ATA_SECTOR_SIZE     512

static uint8_t inb(uint16_t port)
{
    return (uint8_t) sys_ioport_access(0, port, 1, 0);
}

static void outb(uint16_t port, uint8_t val)
{
    sys_ioport_access(1, port, 1, val);
}

static uint16_t inw(uint16_t port)
{
    return (uint16_t) sys_ioport_access(0, port, 2, 0);
}

static void outw(uint16_t port, uint16_t val)
{
    sys_ioport_access(1, port, 2, val);
}

/* 400 ns delay: read the alternate status register four times. */
static void io_wait(void)
{
    for (int i = 0; i < 4; i++)
        inb(ATA_IO_BASE + ATA_REG_CONTROL);
}

static int wait_not_busy(void)
{
    for (int t = 0; t < 0x100000; t++) {
        uint8_t st = inb(ATA_IO_BASE + ATA_REG_STATUS);
        if (st & ATA_SR_ERR)
            return -1;
        if (!(st & ATA_SR_BSY) && (st & ATA_SR_DRQ))
            return 0;
    }
    return -1;
}

/* IDENTIFY the primary master. Returns 0 and fills sector size/count. */
static int ata_init(uint64_t *sector_size, uint64_t *sector_count)
{
    outb(ATA_IO_BASE + ATA_REG_HDDEVSEL, 0xA0);
    io_wait();
    outb(ATA_IO_BASE + ATA_REG_SECCOUNT0, 0);
    outb(ATA_IO_BASE + ATA_REG_LBA0, 0);
    outb(ATA_IO_BASE + ATA_REG_LBA1, 0);
    outb(ATA_IO_BASE + ATA_REG_LBA2, 0);
    outb(ATA_IO_BASE + ATA_REG_COMMAND, ATA_CMD_IDENTIFY);
    io_wait();

    if (inb(ATA_IO_BASE + ATA_REG_STATUS) == 0)
        return -1;              /* no device */

    if (wait_not_busy() != 0)
        return -1;

    if (inb(ATA_IO_BASE + ATA_REG_LBA1) || inb(ATA_IO_BASE + ATA_REG_LBA2))
        return -1;              /* not an ATA (ATAPI) device */

    uint16_t ident[256];
    for (int i = 0; i < 256; i++)
        ident[i] = inw(ATA_IO_BASE + ATA_REG_DATA);

    *sector_size = ATA_SECTOR_SIZE;
    *sector_count = (uint32_t) ident[60] | ((uint64_t) (uint32_t) ident[61] << 16);
    return 0;
}

static int ata_read(uint32_t lba, uint8_t count, uint8_t *buf)
{
    outb(ATA_IO_BASE + ATA_REG_HDDEVSEL, 0xE0 | ((lba >> 24) & 0x0F));
    io_wait();
    outb(ATA_IO_BASE + ATA_REG_ERROR, 0);
    outb(ATA_IO_BASE + ATA_REG_SECCOUNT0, count);
    outb(ATA_IO_BASE + ATA_REG_LBA0, (uint8_t) lba);
    outb(ATA_IO_BASE + ATA_REG_LBA1, (uint8_t) (lba >> 8));
    outb(ATA_IO_BASE + ATA_REG_LBA2, (uint8_t) (lba >> 16));
    outb(ATA_IO_BASE + ATA_REG_COMMAND, ATA_CMD_READ_PIO);

    for (uint8_t s = 0; s < count; s++) {
        if (wait_not_busy() != 0)
            return -1;
        uint16_t *dst = (uint16_t *) (buf + s * ATA_SECTOR_SIZE);
        for (int i = 0; i < 256; i++)
            dst[i] = inw(ATA_IO_BASE + ATA_REG_DATA);
        io_wait();
    }
    return 0;
}

static int ata_write(uint32_t lba, uint8_t count, const uint8_t *buf)
{
    outb(ATA_IO_BASE + ATA_REG_HDDEVSEL, 0xE0 | ((lba >> 24) & 0x0F));
    io_wait();
    outb(ATA_IO_BASE + ATA_REG_ERROR, 0);
    outb(ATA_IO_BASE + ATA_REG_SECCOUNT0, count);
    outb(ATA_IO_BASE + ATA_REG_LBA0, (uint8_t) lba);
    outb(ATA_IO_BASE + ATA_REG_LBA1, (uint8_t) (lba >> 8));
    outb(ATA_IO_BASE + ATA_REG_LBA2, (uint8_t) (lba >> 16));
    outb(ATA_IO_BASE + ATA_REG_COMMAND, ATA_CMD_WRITE_PIO);

    for (uint8_t s = 0; s < count; s++) {
        if (wait_not_busy() != 0)
            return -1;
        const uint16_t *src = (const uint16_t *) (buf + s * ATA_SECTOR_SIZE);
        for (int i = 0; i < 256; i++)
            outw(ATA_IO_BASE + ATA_REG_DATA, src[i]);
        io_wait();
    }
    outb(ATA_IO_BASE + ATA_REG_COMMAND, 0xE7);  /* cache flush */
    return 0;
}

int main(void)
{
    bootinfo_t bi;

    while (sys_bootinfo(&bi) < 0 || bi.magic != BOOTINFO_MAGIC) {
        /* The kernel sets the bootinfo before the process is runnable. */
    }

    uint64_t sector_size = 0, sector_count = 0;
    int have_disk = (ata_init(&sector_size, &sector_count) == 0);

    for (;;) {
        sys_ipc_msg_t m;

        if (sys_ipc_recv_timeout((int64_t) bi.service_ep, &m, 1000) != 0)
            continue;

        sys_ipc_msg_t rep;
        memset(&rep, 0, sizeof(rep));
        rep.tag = m.tag;

        if (m.tag == BLOCK_GET_INFO) {
            rep.words[0] = have_disk ? sector_size : 0;
            rep.words[1] = have_disk ? sector_count : 0;
        } else if ((m.tag == BLOCK_READ || m.tag == BLOCK_WRITE)
                   && have_disk && m.xfer_count >= 1) {
            /* Bulk I/O through the transferred memory object is the next step;
             * report it as unsupported for now. */
            rep.words[0] = (uint64_t) (int64_t) BLOCK_ERR;
        } else {
            rep.words[0] = (uint64_t) (int64_t) BLOCK_ERR;
        }

        int64_t reply = (int64_t) m.xfer[0];
        if (reply != 0) {
            sys_ipc_send(reply, &rep);
            sys_handle_close(reply);
        }
    }

    return 0;
}
