/**-----------------------------------------------------------------------------

 @file    block.c
 @brief   Userspace AHCI (SATA) block server

 @details
 @verbatim

   Owns the AHCI controller's ABAR and a contiguous DMA region granted by the
   kernel, and answers BLOCK_GET_INFO / BLOCK_READ / BLOCK_WRITE over IPC. The
   server builds a command list, a command table and a received-FIS area in the
   DMA region, issues READ/WRITE DMA EXT and IDENTIFY DEVICE, and polls the port
   for completion. Bulk data travels in memory objects; the server copies
   through its own DMA buffer because only that memory has a known physical
   address.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <bootinfo.h>
#include <protocol.h>
#include <string.h>
#include <sysfunc.h>

/* Where the server maps an incoming request's memory object. */
#define BLOCK_BUF_VADDR     0x20000000

#define SECTOR_SIZE         512
#define DMA_SIZE            (256 * 1024)

/* Host control registers, relative to ABAR. */
#define HOST_CAP            0x00
#define HOST_GHC            0x04
#define HOST_PI             0x0c
#define PORT_BASE           0x100
#define PORT_SIZE           0x80

/* Port registers, relative to the port base. */
#define P_CLB               0x00
#define P_CLBU              0x04
#define P_FB                0x08
#define P_FBU               0x0c
#define P_IS                0x10
#define P_CMD               0x18
#define P_TFD               0x20
#define P_SIG               0x24
#define P_SSTS              0x28
#define P_SERR              0x30
#define P_CI                0x38

#define GHC_AE              0x80000000u
#define CMD_ST              0x00000001u
#define CMD_FRE             0x00000010u
#define CMD_FR              0x00004000u
#define CMD_CR              0x00008000u
#define TFD_ERR             0x00000001u
#define TFD_DF              0x00000020u
#define IS_TFES             0x40000000u
#define SSTS_DET_MASK       0x0000000fu     /* DET in bits 0..3, 3 = present */

#define FIS_REG_H2D         0x27
#define ATA_LBA_MODE        0x40

#define CMD_IDENTIFY        0xec
#define CMD_READ_DMA_EXT    0x25
#define CMD_WRITE_DMA_EXT   0x35

/* Offsets inside the DMA region. The base is page aligned, so the command
 * list (1 KiB), the FIS (256 B) and the command table (128 B) meet their
 * alignment: 0, 0x400 and 0x500. */
#define DMA_CMDLIST         0x0000
#define DMA_FIS             0x0400
#define DMA_CMDTBL          0x0500
#define DMA_DATA            0x1000

/* One command list entry. opts carries CFL, the direction bit and PRDTL. */
typedef struct[[gnu::packed]] {
    uint32_t opts;
    uint32_t prdbc;
    uint32_t ctba;
    uint32_t ctbau;
    uint32_t rsv[4];
} cmd_hdr_t;

/* A physical region descriptor: address, a reserved dword, then the byte count
 * minus one in the low 22 bits (bit 31 marks interrupt on completion). */
typedef struct[[gnu::packed]] {
    uint64_t dba;
    uint32_t rsv;
    uint32_t dbc;
} prdt_t;

typedef struct[[gnu::packed]] {
    uint8_t cfis[64];
    uint8_t acmd[16];
    uint8_t rsv[48];
    prdt_t prdt[1];
} cmd_tbl_t;

static volatile uint8_t *abar;
static uint8_t *dma;
static uint64_t dma_phys;
static int32_t port = -1;
static uint64_t sector_count;

static uint32_t r32(uint32_t off)
{
    return *(volatile uint32_t *) (abar + off);
}

static void w32(uint32_t off, uint32_t v)
{
    *(volatile uint32_t *) (abar + off) = v;
}

static uint32_t pr32(uint32_t p, uint32_t off)
{
    return r32(PORT_BASE + p * PORT_SIZE + off);
}

static void pw32(uint32_t p, uint32_t off, uint32_t v)
{
    w32(PORT_BASE + p * PORT_SIZE + off, v);
}

/* Bring one port's command list and FIS receive area up. */
static int32_t port_start(int32_t p)
{
    port = p;

    uint32_t cmd = pr32((uint32_t) p, P_CMD);

    cmd &= ~CMD_ST;
    pw32((uint32_t) p, P_CMD, cmd);
    for (int32_t t = 0; t < 100000 && (pr32((uint32_t) p, P_CMD) & CMD_CR); t++)
        ;

    cmd = pr32((uint32_t) p, P_CMD);
    cmd &= ~CMD_FRE;
    pw32((uint32_t) p, P_CMD, cmd);
    for (int32_t t = 0; t < 100000 && (pr32((uint32_t) p, P_CMD) & CMD_FR); t++)
        ;

    uint64_t clb = dma_phys + DMA_CMDLIST;
    uint64_t fb = dma_phys + DMA_FIS;

    pw32((uint32_t) p, P_CLB, (uint32_t) clb);
    pw32((uint32_t) p, P_CLBU, (uint32_t) (clb >> 32));
    pw32((uint32_t) p, P_FB, (uint32_t) fb);
    pw32((uint32_t) p, P_FBU, (uint32_t) (fb >> 32));
    pw32((uint32_t) p, P_SERR, 0xffffffffu);
    pw32((uint32_t) p, P_IS, 0xffffffffu);

    cmd = pr32((uint32_t) p, P_CMD);
    cmd |= CMD_FRE;
    pw32((uint32_t) p, P_CMD, cmd);
    for (int32_t t = 0; t < 100000 && !(pr32((uint32_t) p, P_CMD) & CMD_FR); t++)
        ;

    cmd = pr32((uint32_t) p, P_CMD);
    cmd |= CMD_ST;
    pw32((uint32_t) p, P_CMD, cmd);
    for (int32_t t = 0; t < 100000 && !(pr32((uint32_t) p, P_CMD) & CMD_CR); t++)
        ;

    return (pr32((uint32_t) p, P_CMD) & CMD_CR) ? 0 : -1;
}

/* Issue one command to the port and poll for completion. */
static int32_t ahci_cmd(uint8_t cmd, uint64_t lba, uint16_t count,
                        uint32_t bytes, bool write)
{
    cmd_hdr_t *hdr = (cmd_hdr_t *) (dma + DMA_CMDLIST);
    cmd_tbl_t *tbl = (cmd_tbl_t *) (dma + DMA_CMDTBL);
    uint8_t *fis = tbl->cfis;

    memset(hdr, 0, sizeof(*hdr));
    memset(tbl, 0, sizeof(*tbl));

    fis[0] = FIS_REG_H2D;
    fis[1] = 0x80;              /* command, not control */
    fis[2] = cmd;
    fis[3] = 0;
    fis[4] = (uint8_t) lba;
    fis[5] = (uint8_t) (lba >> 8);
    fis[6] = (uint8_t) (lba >> 16);
    fis[7] = (cmd == CMD_IDENTIFY) ? 0 : ATA_LBA_MODE;
    fis[8] = (uint8_t) (lba >> 24);
    fis[9] = (uint8_t) (lba >> 32);
    fis[10] = (uint8_t) (lba >> 40);
    fis[11] = 0;
    fis[12] = (uint8_t) count;
    fis[13] = (uint8_t) (count >> 8);

    uint64_t ctba = dma_phys + DMA_CMDTBL;

    hdr->opts = 5 | (write ? (1u << 6) : 0) | (1u << 16);       /* CFL, PRDTL */
    hdr->ctba = (uint32_t) ctba;
    hdr->ctbau = (uint32_t) (ctba >> 32);

    if (bytes > 0) {
        tbl->prdt[0].dba = dma_phys + DMA_DATA;
        tbl->prdt[0].dbc = bytes - 1;
    }

    /* Wait for the port to go idle, then submit slot 0. */
    for (int32_t t = 0; t < 100000 && (pr32((uint32_t) port, P_CI) & 1u); t++)
        ;

    pw32((uint32_t) port, P_IS, 0xffffffffu);
    pw32((uint32_t) port, P_CI, 1);

    for (int32_t t = 0; t < 5000000; t++) {
        if (pr32((uint32_t) port, P_IS) & IS_TFES)
            return -1;
        if (!(pr32((uint32_t) port, P_CI) & 1u))
            break;
    }

    if (pr32((uint32_t) port, P_CI) & 1u)
        return -1;
    if (pr32((uint32_t) port, P_TFD) & (TFD_ERR | TFD_DF))
        return -1;

    return 0;
}

static int32_t ahci_identify(void)
{
    if (ahci_cmd(CMD_IDENTIFY, 0, 1, SECTOR_SIZE, false) != 0)
        return -1;

    uint16_t *id = (uint16_t *) (dma + DMA_DATA);
    uint64_t n;

    if (id[83] & (1u << 10)) {  /* LBA48 supported */
        n = (uint32_t) id[100] | ((uint64_t) (uint32_t) id[101] << 16)
            | ((uint64_t) (uint32_t) id[102] << 32)
            | ((uint64_t) (uint32_t) id[103] << 48);
    } else {
        n = (uint32_t) id[60] | ((uint64_t) (uint32_t) id[61] << 16);
    }

    sector_count = n;
    return n > 0 ? 0 : -1;
}

/* Find the first ATA disk on an AHCI port and identify it. */
static int32_t ahci_init(void)
{
    uint32_t cap = r32(HOST_CAP);
    uint32_t pi = r32(HOST_PI);
    uint32_t nports = (cap & 0x1fu) + 1;

    w32(HOST_GHC, r32(HOST_GHC) | GHC_AE);

    for (uint32_t p = 0; p < nports; p++) {
        if (!(pi & (1u << p)))
            continue;
        if ((pr32(p, P_SSTS) & SSTS_DET_MASK) != 3)
            continue;
        if (pr32(p, P_SIG) != 0x00000101u)  /* ATA disk */
            continue;

        port = (int32_t) p;
        if (port_start(port) != 0)
            continue;
        if (ahci_identify() == 0)
            return 0;
    }

    return -1;
}

static int32_t ahci_read(uint32_t lba, uint8_t count, uint8_t *buf)
{
    uint32_t bytes = (uint32_t) count * SECTOR_SIZE;

    if (count == 0 || bytes > DMA_SIZE - DMA_DATA)
        return -1;
    if (ahci_cmd(CMD_READ_DMA_EXT, lba, count, bytes, false) != 0)
        return -1;

    memcpy(buf, dma + DMA_DATA, bytes);
    return 0;
}

static int32_t ahci_write(uint32_t lba, uint8_t count, const uint8_t *buf)
{
    uint32_t bytes = (uint32_t) count * SECTOR_SIZE;

    if (count == 0 || bytes > DMA_SIZE - DMA_DATA)
        return -1;

    memcpy(dma + DMA_DATA, buf, bytes);
    return ahci_cmd(CMD_WRITE_DMA_EXT, lba, count, bytes, true);
}

int32_t main(void)
{
    bootinfo_t bi;

    while (sys_bootinfo(&bi) < 0 || bi.magic != BOOTINFO_MAGIC) {
        /* The kernel sets the bootinfo before the process is runnable. */
    }

    bool have_disk = false;

    if (bi.block_mmio_vaddr != 0 && bi.block_mmio_size != 0
        && bi.block_dma_vaddr != 0 && bi.block_dma_size >= DMA_SIZE) {
        abar = (volatile uint8_t *) (uint64_t) bi.block_mmio_vaddr;
        dma = (uint8_t *) (uint64_t) bi.block_dma_vaddr;
        dma_phys = bi.block_dma_phys;
        memset(dma, 0, DMA_SIZE);
        have_disk = (ahci_init() == 0);
    }

    for (;;) {
        sys_ipc_msg_t m;

        if (sys_ipc_recv_timeout((int64_t) bi.service_ep, &m, 1000) != 0)
            continue;

        sys_ipc_msg_t rep;
        memset(&rep, 0, sizeof(rep));
        rep.tag = m.tag;

        if (m.tag == BLOCK_GET_INFO) {
            rep.words[0] = have_disk ? SECTOR_SIZE : 0;
            rep.words[1] = have_disk ? sector_count : 0;
        } else if ((m.tag == BLOCK_READ || m.tag == BLOCK_WRITE)
                   && have_disk && m.xfer_count >= 2) {
            /* xfer[0] = reply endpoint, xfer[1] = memory object with the data. */
            int64_t memh = (int64_t) m.xfer[1];
            uint8_t *buf = (uint8_t *) BLOCK_BUF_VADDR;
            int32_t rc = -1;

            if (sys_mem_map(memh, BLOCK_BUF_VADDR, 3) == 0) {
                if (m.tag == BLOCK_READ)
                    rc = ahci_read((uint32_t) m.words[0], (uint8_t) m.words[1],
                                   buf);
                else
                    rc = ahci_write((uint32_t) m.words[0], (uint8_t) m.words[1],
                                    buf);
                sys_mem_unmap(memh, BLOCK_BUF_VADDR);
            }
            rep.words[0] = (uint64_t) (int64_t) (rc == 0 ? BLOCK_OK : BLOCK_ERR);
            sys_handle_close(memh);
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
