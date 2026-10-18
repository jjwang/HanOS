/**-----------------------------------------------------------------------------

 @file    ehci.c
 @brief   EHCI (USB 2.0) host controller and mass storage
 @details
 @verbatim

  The EHCI driver covers machines whose USB 2.0 ports sit on a separate
  controller from the xHCI. It runs the async schedule with one queue head
  that is re-armed for each control or bulk transfer, so no transfer ever
  overlaps another. A mass-storage device found on a port is driven with the
  Bulk-Only Transport and feeds the shared usb_msc_* interface.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>
#include <string.h>

#include <mm/mm.h>
#include <lib/klog.h>
#include <lib/vector.h>
#include <arch/x64/pci.h>
#include <arch/x64/cpu.h>
#include <arch/x64/hpet.h>
#include <proc/sched.h>
#include <device/usb/xhci.h>

/* Capability registers, relative to the MMIO base. */
#define CAP_CAPLENGTH       0x00
#define CAP_HCIVERSION      0x02
#define CAP_HCSPARAMS       0x04
#define CAP_HCCPARAMS       0x08

/* Operational registers, relative to CAPLENGTH. */
#define OP_USBCMD           0x00
#define OP_USBSTS           0x04
#define OP_USBINTR          0x08
#define OP_FRINDEX          0x0c
#define OP_CTRLDSSEGMENT    0x10
#define OP_PERIODICLISTBASE 0x14
#define OP_ASYNCLISTADDR    0x18
#define OP_CONFIGFLAG       0x40
#define OP_PORTSC(n)        (0x44 + ((n) - 1) * 4)

#define USBCMD_RS           (1u << 0)
#define USBCMD_HCRESET      (1u << 1)
#define USBCMD_PSE          (1u << 4)
#define USBCMD_ASE          (1u << 5)

#define USBSTS_HCHALTED     (1u << 12)
#define USBSTS_RECLAMATION  (1u << 13)
#define USBSTS_ASYNC        (1u << 15)

#define PORTSC_CCS          (1u << 0)
#define PORTSC_CSC          (1u << 1)
#define PORTSC_PED          (1u << 2)
#define PORTSC_PEDC         (1u << 3)
#define PORTSC_PR           (1u << 8)
#define PORTSC_PP           (1u << 12)
#define PORTSC_OWNER        (1u << 13)

/* Queue element transfer descriptor. A 64-bit capable controller also reads
 * the five high dwords of the buffer pointers (EHCI appendix B). Keep the
 * structure 64-byte aligned so the qTD chain stays 32-byte aligned. */
typedef struct[[gnu::packed]] {
    uint32_t next;
    uint32_t alt_next;
    uint32_t token;
    uint32_t buf[5];
    uint32_t buf_hi[5];
    uint32_t pad[3];
} qtd_t;

/* Queue head. Same appendix B high buffer pointers in the overlay. Pad to
 * 96 bytes so a following QH stays 32-byte aligned. */
typedef struct[[gnu::packed]] {
    uint32_t horiz;
    uint32_t epchar;
    uint32_t epcap;
    uint32_t cur_qtd;
    uint32_t next_qtd;
    uint32_t alt_qtd;
    uint32_t token;
    uint32_t buf[5];
    uint32_t buf_hi[5];
    uint32_t pad[7];
} qh_t;

#define QTD_ACTIVE          (1u << 7)
#define QTD_HALTED          (1u << 6)
#define QTD_DBE             (1u << 5)
#define QTD_BABBLE          (1u << 4)
#define QTD_XACTERR         (1u << 3)
#define QTD_MISSED          (1u << 2)
#define QTD_STALL           (1u << 0)
#define QTD_ERR_MASK        (QTD_HALTED | QTD_DBE | QTD_BABBLE | QTD_XACTERR \
                             | QTD_MISSED | QTD_STALL)

#define PID_OUT             0
#define PID_IN              1
#define PID_SETUP           2

#define QTD_IOC             (1u << 15)
#define QTD_TOKEN(pid, len) (((uint32_t) (pid) << 8) | (3u << 10) \
                             | QTD_IOC | QTD_ACTIVE | ((uint32_t) (len) << 16))

/* DMA arena. */
#define EHCI_DMA_SIZE       (64 * 1024)
#define DMA_FRAMELIST       0x0000      /* 1024 * 4 bytes */
#define DMA_QH              0x1000      /* 3 queue heads */
#define DMA_QTD             0x2000      /* 16 qTDs */
#define DMA_DATA            0x3000      /* 128 KiB of transfer data */

vec_extern(pci_device_t, pci_devices);

static volatile uint8_t *mmio;
static uint32_t op;
static uint8_t nports;

static uint8_t *dma;
static uint64_t dma_phys;

static qh_t *qh_list;
static qtd_t *qtds;

static uint8_t *xfer;           /* DMA_DATA */
static uint64_t xfer_phys;

static bool ehci_inited;
static bool msc_ready;
static uint32_t msc_sectors;
static uint8_t dev_addr;
static uint8_t dev_speed;
static uint8_t dev_port;
static uint8_t bulk_in_ep, bulk_out_ep;
static uint16_t bulk_in_mps, bulk_out_mps;
static uint32_t bulk_in_toggle, bulk_out_toggle;
static uint32_t msc_tag;

static uint32_t mmio_rd(uint32_t off)
{
    return *(volatile uint32_t *) (mmio + off);
}

static void mmio_wr(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *) (mmio + off) = val;
}

static void *dma_ptr(uint32_t off)
{
    return dma + off;
}

static uint64_t dma_addr(uint32_t off)
{
    return dma_phys + off;
}

/* Delay without depending on the timer IRQ. The block server runs the storage
 * path from a syscall, where interrupts can stay masked. */
static void ehci_delay_ms(uint32_t ms)
{
    hpet_nanosleep((uint64_t) ms * 1000000ULL);
}

/* Reset the queue head overlay and re-arm the async schedule for one transfer.
 * The frame list stays all-terminate, so only the async list runs. Stop the
 * async schedule first: the controller must not read the QH while software
 * rewrites it. */
static void ehci_arm(qh_t *qh, uint8_t addr, uint8_t ep, uint8_t speed,
                     uint16_t mps, uint32_t qtd_index)
{
    mmio_wr(op + OP_USBCMD, USBCMD_RS | USBCMD_PSE);
    for (int32_t i = 0; i < 100 && (mmio_rd(op + OP_USBSTS) & USBSTS_ASYNC);
         i++)
        ehci_delay_ms(1);

    /* Clear latched errors. A host system error halts the controller. */
    mmio_wr(op + OP_USBSTS, 0x3f);

    qh->horiz = (uint32_t) dma_addr(DMA_QH) | (1u << 1);        /* QH, self */
    qh->epchar = (uint32_t) addr | ((uint32_t) ep << 8)
        | ((uint32_t) speed << 12) | (1u << 14) | (1u << 15)
        | ((uint32_t) mps << 16) | (ep == 0 ? (1u << 27) : 0)
        | (7u << 28);
    qh->epcap = 1u << 30;       /* multiplier 1; async S/C masks clear */

    /* A full/low-speed device on an EHCI root port uses the root hub TT.
     * The hub address stays 0; the port number sits in bits 29..23. */
    if (speed <= 1 && dev_port != 0)
        qh->epcap |= ((uint32_t) dev_port & 0x7f) << 23;
    qh->cur_qtd = 1;            /* invalid */
    qh->next_qtd = (uint32_t) dma_addr(DMA_QTD + qtd_index * sizeof(qtd_t));
    qh->alt_qtd = 1;
    qh->token = 0;

    asm volatile ("mfence" ::: "memory");
    mmio_wr(op + OP_ASYNCLISTADDR, (uint32_t) dma_addr(DMA_QH));
    mmio_wr(op + OP_USBCMD, USBCMD_RS | USBCMD_PSE | USBCMD_ASE);
}

/* Wait for every qTD in the chain to leave the active state. */
static bool ehci_wait(qtd_t *first, uint32_t count)
{
    uint64_t deadline = hpet_get_nanos() + 1000ULL * 1000000ULL;

    while (hpet_get_nanos() < deadline) {
        bool busy = false;

        for (uint32_t i = 0; i < count; i++) {
            if (first[i].token & QTD_ACTIVE) {
                busy = true;
                break;
            }
        }
        if (!busy)
            return true;
        ehci_delay_ms(1);
    }
    return false;
}

static bool ehci_check(qtd_t *first, uint32_t count)
{
    for (uint32_t i = 0; i < count; i++) {
        if (first[i].token & QTD_ERR_MASK)
            return false;
    }
    return true;
}

/* Fill the five buffer pointers a qTD carries. The controller walks the list
 * page by page, so a transfer that crosses a page boundary needs every page
 * pointer set. */
static void ehci_set_buffers(qtd_t *q, uint64_t addr, uint32_t len)
{
    uint32_t pages = (uint32_t) (((addr & 0xfff) + len + 0xfff) >> 12);

    for (uint32_t i = 0; i < 5; i++)
        q->buf[i] = 0;
    if (len == 0)
        return;

    q->buf[0] = (uint32_t) addr;
    for (uint32_t i = 1; i < pages && i < 5; i++)
        q->buf[i] = (uint32_t) ((addr & ~0xfffULL) + (uint64_t) i * 0x1000);
}

/* Take the controller from the firmware. The USB Legacy Support extended
 * capability keeps the ports under SMI control until the OS owned semaphore is
 * set and the BIOS owned semaphore clears. */
static void ehci_legacy_handoff(uint32_t id, uint32_t hcc)
{
    uint8_t off = (uint8_t) (hcc >> 8);

    for (int32_t i = 0; off >= 0x40 && i < 16; i++) {
        uint32_t cap = pci_ind(id, off);

        if ((cap & 0xff) == 0x01) {
            pci_outd(id, off, cap | (1u << 24));
            for (int32_t t = 0; t < 1000; t++) {
                if (!(pci_ind(id, off) & (1u << 16)))
                    break;
                ehci_delay_ms(1);
            }
            klogi("USB: EHCI legacy handoff, d0 %08x\n", pci_ind(id, off));
            if (pci_ind(id, off) & (1u << 16))
                klogw("USB: EHCI BIOS handoff timed out\n");
            return;
        }

        uint8_t next = (uint8_t) ((cap >> 8) & 0xff);

        if (next == 0 || next == off)
            break;
        off = next;
    }
}

/* Speed select (EPS) for the queue head. PORTSC reports the negotiated speed
 * directly; an enabled port that reports high-speed is the normal case for a
 * mass-storage device. */
static uint8_t ehci_port_speed(uint8_t port)
{
    uint32_t psc = mmio_rd(op + OP_PORTSC(port));

    if (!(psc & PORTSC_PED))
        return 0;
    return (uint8_t) ((psc >> 26) & 0x3u);
}

/* One control transfer. The setup packet sits at the start of the DMA data
 * area and the data stage at +0x1000, so the two never share a buffer. */
static bool ehci_control(uint8_t addr, uint8_t speed, uint16_t mps,
                         const uint8_t *setup, uint32_t len, bool in,
                         const uint8_t *out)
{
    qtd_t *q = qtds;
    uint32_t n = 0;

    memcpy(xfer, setup, 8);
    if (len > 0 && !in && out != NULL)
        memcpy(xfer + 0x1000, out, len);

    memset(&q[0], 0, sizeof(qtd_t));
    q[0].next = (uint32_t) dma_addr(DMA_QTD + sizeof(qtd_t));
    q[0].alt_next = 1;
    q[0].token = QTD_TOKEN(PID_SETUP, 8);
    q[0].buf[0] = (uint32_t) xfer_phys;
    n = 1;

    if (len > 0) {
        memset(&q[1], 0, sizeof(qtd_t));
        q[1].next = (uint32_t) dma_addr(DMA_QTD + 2 * sizeof(qtd_t));
        q[1].alt_next = 1;
        /* Control data and status stages run with DATA1. */
        q[1].token = QTD_TOKEN(in ? PID_IN : PID_OUT, len) | (1u << 31);
        ehci_set_buffers(&q[1], xfer_phys + 0x1000, len);
        n = 2;
    }

    /* Status stage: opposite direction, zero length. Keep a valid buffer
     * pointer; some controllers fault on a null pointer even for a zero-byte
     * transfer. */
    memset(&q[n], 0, sizeof(qtd_t));
    q[n].next = 1;
    q[n].alt_next = 1;
    q[n].token = QTD_TOKEN(in ? PID_OUT : PID_IN, 0) | (1u << 31);
    q[n].buf[0] = (uint32_t) (xfer_phys + 0x2000);
    n++;

    ehci_arm(&qh_list[0], addr, 0, speed, mps, 0);

    bool ok = ehci_wait(q, n) && ehci_check(q, n);

    if (!ok)
        klogw("USB: EHCI control addr %u len %u in %u failed,"
              " token %08x %08x %08x cmd %08x sts %08x\n", (uint32_t) addr,
              len, (uint32_t) in, q[0].token, q[1].token, q[n - 1].token,
              mmio_rd(op + OP_USBCMD), mmio_rd(op + OP_USBSTS));

    return ok;
}

/* One bulk transfer through the async queue head. */
static bool ehci_bulk(uint8_t addr, uint8_t ep, uint16_t mps, bool in,
                      uint32_t *toggle, uint8_t *buf, uint32_t len)
{
    qtd_t *q = qtds;

    if (!in)
        memcpy(xfer, buf, len);

    memset(q, 0, sizeof(qtd_t));
    q[0].next = 1;
    q[0].alt_next = 1;
    q[0].token = QTD_TOKEN(in ? PID_IN : PID_OUT, len) | *toggle;
    ehci_set_buffers(&q[0], xfer_phys, len);

    ehci_arm(&qh_list[0], addr, ep, dev_speed, mps, 0);

    if (!ehci_wait(q, 1) || !ehci_check(q, 1))
        return false;

    *toggle ^= (1u << 31);
    if (in)
        memcpy(buf, xfer, len);
    return true;
}

static bool ehci_port_reset(uint8_t port)
{
    uint32_t psc = mmio_rd(op + OP_PORTSC(port));

    if (!(psc & PORTSC_CCS))
        return false;

    /* Take ownership from the companion controller when the port is routed
     * there, then reset for at least 50 ms. */
    if (psc & PORTSC_OWNER)
        mmio_wr(op + OP_PORTSC(port), (psc & ~PORTSC_OWNER) | PORTSC_PP);
    ehci_delay_ms(20);

    psc = mmio_rd(op + OP_PORTSC(port));
    mmio_wr(op + OP_PORTSC(port), (psc & ~(PORTSC_PR | PORTSC_PEDC
                                           | PORTSC_CSC)) | PORTSC_PR);
    ehci_delay_ms(60);
    mmio_wr(op + OP_PORTSC(port), mmio_rd(op + OP_PORTSC(port)) & ~PORTSC_PR);

    for (int32_t i = 0; i < 100; i++) {
        if (mmio_rd(op + OP_PORTSC(port)) & PORTSC_PED) {
            /* USB reset recovery time before the first control transfer. */
            ehci_delay_ms(20);
            return true;
        }
        ehci_delay_ms(1);
    }
    return false;
}

/* Read one descriptor into the control data buffer (xfer + 0x1000). */
static bool ehci_get_descriptor(uint8_t addr, uint8_t speed, uint16_t mps,
                                uint8_t type, uint8_t index, uint16_t len)
{
    uint8_t setup[8];

    setup[0] = 0x80;            /* device-to-host, standard, device */
    setup[1] = 6;               /* GET_DESCRIPTOR */
    setup[2] = 0;
    setup[3] = type;
    setup[4] = index;
    setup[5] = 0;
    setup[6] = (uint8_t) len;
    setup[7] = (uint8_t) (len >> 8);

    return ehci_control(addr, speed, mps, setup, len, true, NULL);
}

static bool ehci_set_address(uint8_t speed, uint16_t mps, uint8_t addr)
{
    uint8_t setup[8] = { 0x00, 5, (uint8_t) addr, 0, 0, 0, 0, 0 };

    return ehci_control(0, speed, mps, setup, 0, false, NULL);
}

static bool ehci_set_config(uint8_t addr, uint8_t speed, uint16_t mps,
                            uint8_t value)
{
    uint8_t setup[8] = { 0x00, 9, (uint8_t) value, 0, 0, 0, 0, 0 };

    return ehci_control(addr, speed, mps, setup, 0, false, NULL);
}

static bool msc_bot(const uint8_t *cb, uint8_t cb_len, uint32_t data_len,
                    bool in)
{
    uint8_t *cbw = xfer + 0x8000;
    uint8_t *csw = xfer + 0x8100;
    uint32_t tag = ++msc_tag;

    memset(cbw, 0, 31);
    cbw[0] = 0x55;              /* CBW signature */
    cbw[1] = 0x53;
    cbw[2] = 0x42;
    cbw[3] = 0x43;
    cbw[4] = (uint8_t) tag;
    cbw[5] = (uint8_t) (tag >> 8);
    cbw[6] = (uint8_t) (tag >> 16);
    cbw[7] = (uint8_t) (tag >> 24);
    cbw[8] = (uint8_t) data_len;
    cbw[9] = (uint8_t) (data_len >> 8);
    cbw[10] = (uint8_t) (data_len >> 16);
    cbw[11] = (uint8_t) (data_len >> 24);
    cbw[12] = in ? 0x80 : 0x00;
    cbw[13] = 0;                /* LUN */
    cbw[14] = cb_len;
    memcpy(cbw + 15, cb, cb_len);

    if (!ehci_bulk(dev_addr, bulk_out_ep, bulk_out_mps, false,
                   &bulk_out_toggle, cbw, 31))
        return false;

    if (data_len > 0) {
        uint8_t ep = in ? bulk_in_ep : bulk_out_ep;
        uint16_t mps = in ? bulk_in_mps : bulk_out_mps;

        if (!ehci_bulk(dev_addr, ep, mps, in,
                       in ? &bulk_in_toggle : &bulk_out_toggle,
                       xfer + 0x9000, data_len))
            return false;
    }

    if (!ehci_bulk(dev_addr, bulk_in_ep, bulk_in_mps, true, &bulk_in_toggle,
                   csw, 13))
        return false;

    return csw[0] == 0x55 && csw[1] == 0x53 && csw[2] == 0x42 && csw[3] == 0x53
        && csw[12] == 0;
}

static bool msc_read_capacity(void)
{
    uint8_t cb[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    if (!msc_bot(cb, 10, 8, true))
        return false;

    uint8_t *d = xfer + 0x9000;
    uint32_t last = ((uint32_t) d[0] << 24) | ((uint32_t) d[1] << 16)
        | ((uint32_t) d[2] << 8) | d[3];

    msc_sectors = last + 1;
    return true;
}

static bool msc_read10(uint32_t lba, uint16_t count)
{
    uint8_t cb[10] = { 0x28, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    cb[2] = (uint8_t) (lba >> 24);
    cb[3] = (uint8_t) (lba >> 16);
    cb[4] = (uint8_t) (lba >> 8);
    cb[5] = (uint8_t) lba;
    cb[7] = (uint8_t) (count >> 8);
    cb[8] = (uint8_t) count;

    return msc_bot(cb, 10, (uint32_t) count * 512, true);
}

static bool msc_write10(uint32_t lba, uint16_t count)
{
    uint8_t cb[10] = { 0x2a, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    cb[2] = (uint8_t) (lba >> 24);
    cb[3] = (uint8_t) (lba >> 16);
    cb[4] = (uint8_t) (lba >> 8);
    cb[5] = (uint8_t) lba;
    cb[7] = (uint8_t) (count >> 8);
    cb[8] = (uint8_t) count;

    return msc_bot(cb, 10, (uint32_t) count * 512, false);
}

/* Probe one port, address its device and configure a mass-storage interface. */
static void ehci_enum_port(uint8_t port)
{
    if (!ehci_port_reset(port)) {
        klogw("USB: EHCI port %u reset failed\n", (uint32_t) port);
        return;
    }

    uint8_t speed = ehci_port_speed(port);
    uint16_t mps = 8;

    dev_speed = speed;
    dev_port = port;
    klogi("USB: EHCI port %u speed %u\n", (uint32_t) port, (uint32_t) speed);

    /* Read the first 8 bytes at the default address to learn bMaxPacketSize0,
     * then set the address and read the full device descriptor. */
    if (!ehci_get_descriptor(0, speed, 8, 1, 0, 8))
        return;

    mps = (uint16_t) (xfer + 0x1000)[7];
    if (mps != 8 && mps != 16 && mps != 32 && mps != 64)
        mps = 8;

    uint8_t addr = 0;
    for (uint8_t i = 1; i <= 8; i++) {
        if (i != dev_addr) {
            addr = i;
            break;
        }
    }
    if (addr == 0 || !ehci_set_address(speed, mps, addr))
        return;
    dev_addr = addr;
    ehci_delay_ms(2);

    if (!ehci_get_descriptor(addr, speed, mps, 1, 0, 18))
        return;

    klogi("USB: EHCI port %u device %02x%02x:%02x%02x\n", (uint32_t) port,
          (xfer + 0x1000)[9], (xfer + 0x1000)[8], (xfer + 0x1000)[11],
          (xfer + 0x1000)[10]);

    if (!ehci_get_descriptor(addr, speed, mps, 2, 0, 9))
        return;

    uint16_t total = (uint16_t) (xfer + 0x1000)[2]
        | ((uint16_t) (xfer + 0x1000)[3] << 8);
    if (total > 512)
        total = 512;
    if (!ehci_get_descriptor(addr, speed, mps, 2, 0, total))
        return;

    /* Walk the configuration descriptors to find the mass-storage interface
     * and its bulk endpoints. */
    uint8_t *p = xfer + 0x1000;
    uint8_t *end = p + total;
    uint8_t cfg_value = 1;
    uint8_t ep_in = 0, ep_out = 0;
    uint16_t in_mps = 64, out_mps = 64;
    bool msc = false;

    while (p + 2 <= end && p[0] >= 2 && p[1] >= 2) {
        if (p[1] == 2 && p + 9 <= end) {
            cfg_value = p[5];
        } else if (p[1] == 4 && p + 9 <= end) {
            if (p[5] == 0x08)   /* mass storage */
                msc = true;
        } else if (p[1] == 5 && p + 7 <= end) {
            uint8_t ep = p[2] & 0x0f;
            uint16_t m = (uint16_t) p[4] | ((uint16_t) p[5] << 8);

            /* Only bulk endpoints carry the mass-storage transport. */
            if ((p[3] & 0x03) == 0x02) {
                if (p[2] & 0x80) {
                    ep_in = ep;
                    in_mps = m ? m : 64;
                } else {
                    ep_out = ep;
                    out_mps = m ? m : 64;
                }
            }
        }
        p += p[0];
    }

    if (!msc || ep_in == 0 || ep_out == 0)
        return;

    if (!ehci_set_config(addr, speed, mps, cfg_value))
        return;

    bulk_in_ep = ep_in;
    bulk_out_ep = ep_out;
    bulk_in_mps = in_mps;
    bulk_out_mps = out_mps;
    bulk_in_toggle = false;
    bulk_out_toggle = false;

    if (!msc_read_capacity())
        return;
    msc_ready = true;

    klogi("USB: EHCI mass storage %u sectors on port %u\n", msc_sectors, port);
}

/* Bring up one EHCI controller and look for a mass-storage device. Machines
 * with two EHCI controllers keep different ports on each. */
static bool ehci_setup(pci_device_t *dev)
{
    uint32_t id = PCI_MAKE_ID(dev->bus, dev->device, dev->func);

    dev_addr = 0;
    dev_speed = 0;
    dev_port = 0;
    bulk_in_ep = 0;
    bulk_out_ep = 0;
    msc_tag = 0;

    pci_outw(id, PCI_CONFIG_COMMAND, pci_inw(id, PCI_CONFIG_COMMAND) | 0x6);

    pci_bar_t bar;

    pci_get_bar(&bar, id, 0);
    if (bar.u.address == NULL || bar.size == 0) {
        kloge("USB: EHCI BAR missing\n");
        return false;
    }

    mmio = (volatile uint8_t *) PHYS_TO_VIRT(bar.u.address);
    vmm_map(NULL, (uint64_t) mmio, (uint64_t) bar.u.address,
            NUM_PAGES(bar.size ? bar.size : PAGE_SIZE), VMM_FLAGS_MMIO);

    uint8_t caplen = mmio[CAP_CAPLENGTH];
    uint32_t hcs = mmio_rd(CAP_HCSPARAMS);
    uint32_t hcc = mmio_rd(CAP_HCCPARAMS);

    ehci_legacy_handoff(id, hcc);

    op = caplen;
    nports = (uint8_t) (hcs & 0x0f);

    uint64_t dma_base = pmm_get(NUM_PAGES(EHCI_DMA_SIZE), 0x0, __func__,
                                __LINE__);

    if (dma_base == 0) {
        kloge("USB: EHCI DMA allocation failed\n");
        return false;
    }
    dma = (uint8_t *) PHYS_TO_VIRT(dma_base);
    memset(dma, 0, EHCI_DMA_SIZE);
    dma_phys = dma_base;

    qh_list = dma_ptr(DMA_QH);
    qtds = dma_ptr(DMA_QTD);
    xfer = dma_ptr(DMA_DATA);
    xfer_phys = dma_addr(DMA_DATA);

    /* Halt the controller, reset it, and wait for the reset to finish. */
    mmio_wr(op + OP_USBCMD, 0);
    for (int32_t i = 0; i < 1000 && !(mmio_rd(op + OP_USBSTS) & USBSTS_HCHALTED);
         i++)
        ehci_delay_ms(1);

    mmio_wr(op + OP_USBCMD, USBCMD_HCRESET);
    for (int32_t i = 0; i < 1000 && (mmio_rd(op + OP_USBCMD) & USBCMD_HCRESET);
         i++)
        ehci_delay_ms(1);

    /* Terminate every periodic entry and point the async list at the queue
     * head. The frame list size stays the default 1024 entries. */
    uint32_t *fl = dma_ptr(DMA_FRAMELIST);

    for (uint32_t i = 0; i < 1024; i++)
        fl[i] = 1;

    memset(qh_list, 0, 3 * sizeof(qh_t));
    qh_list[0].horiz = (uint32_t) dma_addr(DMA_QH) | (1u << 1);
    qh_list[0].cur_qtd = 1;
    qh_list[0].next_qtd = 1;
    qh_list[0].alt_qtd = 1;

    mmio_wr(op + OP_CTRLDSSEGMENT, 0);
    mmio_wr(op + OP_PERIODICLISTBASE, (uint32_t) dma_addr(DMA_FRAMELIST));
    mmio_wr(op + OP_ASYNCLISTADDR, (uint32_t) dma_addr(DMA_QH));
    mmio_wr(op + OP_CONFIGFLAG, 1);

    mmio_wr(op + OP_USBINTR, 0);
    mmio_wr(op + OP_USBCMD, USBCMD_RS | USBCMD_PSE | USBCMD_ASE);
    for (int32_t i = 0; i < 1000 && (mmio_rd(op + OP_USBSTS) & USBSTS_HCHALTED);
         i++)
        ehci_delay_ms(1);
    if (mmio_rd(op + OP_USBSTS) & USBSTS_HCHALTED)
        klogw("USB: EHCI stays halted\n");
    mmio_wr(op + OP_USBSTS, 0x3f);

    klogi("USB: EHCI %04x:%04x ports %u dma %lx\n", dev->vendor_id,
          dev->device_id, nports, dma_phys);

    /* Power every port the firmware left off. A device stays disconnected
     * while PORTSC.PP is clear. */
    uint32_t powered = 0;

    for (uint8_t port = 1; port <= nports; port++) {
        uint32_t psc = mmio_rd(op + OP_PORTSC(port));

        if (psc & PORTSC_PP)
            continue;
        mmio_wr(op + OP_PORTSC(port),
                (psc & ~(PORTSC_PR | PORTSC_CSC | PORTSC_PEDC))
                | PORTSC_PP);
        powered++;
    }
    if (powered)
        klogi("USB: EHCI powered %u ports\n", powered);

    /* The boot device was configured by the firmware. A controller reset and
     * handoff drop the port state; give the link time to report CCS again. */
    ehci_delay_ms(500);

    for (uint8_t port = 1; port <= nports; port++) {
        uint32_t psc = mmio_rd(op + OP_PORTSC(port));

        if (psc & PORTSC_CCS) {
            ehci_enum_port(port);
            if (msc_ready)
                break;
        }
    }

    return msc_ready;
}

bool ehci_init(void)
{
    bool found = false;

    if (ehci_inited)
        return msc_ready;
    ehci_inited = true;

    for (uint64_t i = 0; i < vec_length(&pci_devices); i++) {
        pci_device_t *d = &vec_at(&pci_devices, i);
        uint32_t id = PCI_MAKE_ID(d->bus, d->device, d->func);
        uint32_t cls = pci_ind(id, PCI_CLASS_PERIHPERALS);

        if ((cls >> 24) != PCI_CLASS_SERIAL_BUS
            || ((cls >> 16) & 0xff) != 0x03
            || ((cls >> 8) & 0xff) != PCI_SERIAL_USB_EHCI)
            continue;

        found = true;
        if (ehci_setup(d))
            return true;
    }

    if (!found)
        klogi("USB: no EHCI controller found\n");
    else if (!msc_ready)
        klogi("USB: no EHCI mass storage found\n");
    return false;
}

bool ehci_msc_ready(void)
{
    return msc_ready;
}

uint32_t ehci_msc_sectors(void)
{
    return msc_sectors;
}

bool ehci_msc_read(uint32_t lba, uint32_t count, void *buf)
{
    if (!msc_ready || count == 0 || count * 512 > 0x2000)
        return false;
    if (!msc_read10(lba, (uint16_t) count))
        return false;
    memcpy(buf, xfer + 0x9000, (size_t) count * 512);
    return true;
}

bool ehci_msc_write(uint32_t lba, uint32_t count, const void *buf)
{
    if (!msc_ready || count == 0 || count * 512 > 0x2000)
        return false;
    memcpy(xfer + 0x9000, buf, (size_t) count * 512);
    return msc_write10(lba, (uint16_t) count);
}
