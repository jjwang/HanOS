/**-----------------------------------------------------------------------------

 @file    xhci.c
 @brief   Minimal xHCI host controller and USB HID pointer support
 @details
 @verbatim

   Bring up the first xHCI controller, enumerate a HID boot pointer and feed
   its reports to the hardware cursor. Only what a pointer needs is
   implemented: one controller, control transfers on EP0 and a single
   interrupt IN endpoint, polled from a kernel thread. xHCI DMA is
   cache-coherent, so no explicit cache maintenance is needed.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdbool.h>
#include <stdint.h>

#include <string.h>
#include <lib/klog.h>
#include <lib/vector.h>
#include <lib/kmalloc.h>
#include <lib/spinlock.h>
#include <mm/mm.h>
#include <arch/x64/pci.h>
#include <arch/x64/pit.h>
#include <proc/sched.h>
#include <srv/tty_srv.h>
#include <device/display/gfx.h>
#include <device/usb/xhci.h>

/* Intel Lynx Point xHCI PCI config registers. */
#define XHCI_USB2PR         0xd0
#define XHCI_USB2PRM        0xd4
#define XHCI_USB2PR_HCSEL   0x7fff

/* Capability registers. */
#define CAP_CAPLENGTH       0x00
#define CAP_HCSPARAMS1      0x04
#define CAP_HCSPARAMS2      0x08
#define CAP_HCCPARAMS1      0x10
#define CAP_DBOFF           0x14
#define CAP_RTSOFF          0x18

/* Operational registers. */
#define OP_USBCMD           0x00
#define OP_USBSTS           0x04
#define OP_CRCR             0x18
#define OP_DCBAAP           0x30
#define OP_CONFIG           0x38
#define OP_PORTSC(n)        (0x400 + ((n) - 1) * 0x10)

#define USBCMD_RS           (1u << 0)
#define USBCMD_HCRST        (1u << 1)
#define USBCMD_INTE         (1u << 2)
#define USBSTS_HCH          (1u << 0)
#define USBSTS_CNR          (1u << 11)

#define PORTSC_CCS          (1u << 0)
#define PORTSC_PR           (1u << 4)
#define PORTSC_PP           (1u << 9)
#define PORTSC_CSC          (1u << 17)
#define PORTSC_PRC          (1u << 21)

/* TRB types. */
#define TRB_NORMAL          1
#define TRB_SETUP           2
#define TRB_DATA            3
#define TRB_STATUS          4
#define TRB_LINK            6
#define TRB_ENABLE_SLOT     9
#define TRB_ADDRESS_DEVICE  11
#define TRB_CONFIGURE_EP    12
#define TRB_RESET_EP        14
#define TRB_EV_TRANSFER     32
#define TRB_EV_CMD_COMPLETE 33

#define EP_TYPE_CONTROL     4
#define EP_TYPE_INT_IN      7

#define CC_SUCCESS          1

#define RING_NUM            256
#define EVT_NUM             256
#define XHCI_TIMEOUT_MS     500

/**
 * @brief xHCI transfer request block (TRB)
 */
typedef struct[[gnu::packed]] {
    uint64_t param;
    uint32_t status;
    uint32_t control;
} xhci_trb_t;

/**
 * @brief xHCI device slot context
 */
typedef struct[[gnu::packed]] {
    uint32_t dev_info;
    uint32_t dev_info2;
    uint32_t tt_info;
    uint32_t dev_state;
    uint32_t rsvd[4];
} xhci_slot_ctx_t;

/**
 * @brief xHCI endpoint context
 */
typedef struct[[gnu::packed]] {
    uint32_t info;
    uint32_t info2;
    uint64_t deq;
    uint32_t tx_info;
    uint32_t rsvd[3];
} xhci_ep_ctx_t;

/**
 * @brief xHCI input control context
 */
typedef struct[[gnu::packed]] {
    uint32_t drop_flags;
    uint32_t add_flags;
    uint32_t rsvd[6];
} xhci_icc_t;

/**
 * @brief xHCI input context for address and configure commands
 */
typedef struct[[gnu::packed]] {
    xhci_icc_t icc;
    xhci_slot_ctx_t slot;
    xhci_ep_ctx_t ep[31];
} xhci_input_ctx_t;

/**
 * @brief xHCI device context
 */
typedef struct[[gnu::packed]] {
    xhci_slot_ctx_t slot;
    xhci_ep_ctx_t ep[31];
} xhci_dev_ctx_t;

/**
 * @brief xHCI event ring segment table entry
 */
typedef struct[[gnu::packed]] {
    uint64_t base;
    uint32_t size;
    uint32_t rsvd;
} xhci_erst_entry_t;

static volatile uint8_t *mmio;
static uint32_t op_base;
static uint32_t rt_base;
static uint32_t db_base;
static uint8_t max_slots;
static uint8_t max_ports;

static xhci_trb_t *cmd_ring;
static uint64_t cmd_ring_phys;
static uint32_t cmd_enq;
static uint32_t cmd_cycle;

static xhci_trb_t *evt_ring;
static uint64_t evt_ring_phys;
static uint32_t evt_deq;
static uint32_t evt_cycle;

static uint64_t *dcbaa;
static uint64_t *scratch_array;     /* DCBAAP[0]: scratchpad page addresses */
static xhci_input_ctx_t *input_ctx;

static xhci_trb_t *ep0_ring;
static uint64_t ep0_ring_phys;
static uint32_t ep0_enq;
static uint32_t ep0_cycle;

static uint8_t *ctrl_buf;
static uint64_t ctrl_buf_phys;

#define HID_MAX             2
#define HID_PROTO_KBD       1
#define HID_PROTO_MOUSE     2

/**
 * @brief One enumerated HID interrupt endpoint
 */
typedef struct {
    bool used;
    uint8_t slot;
    uint8_t dci;
    uint16_t max_packet;
    uint8_t protocol;
    xhci_trb_t *ring;
    uint64_t ring_phys;
    uint32_t enq;
    uint32_t cycle;
    uint8_t *buf;
    uint64_t buf_phys;
    xhci_dev_ctx_t *dev_ctx;
} hid_t;

static hid_t hids[HID_MAX];
static int32_t nhids;

#define EP_TYPE_BULK_OUT    2
#define EP_TYPE_BULK_IN     6

/* USB mass storage, Bulk-Only Transport (class 8, subclass 6, protocol 0x50).
 * One device is enough for a live USB. */
static struct {
    bool ready;
    uint8_t slot;
    uint8_t out_dci, in_dci;
    uint16_t out_pkt, in_pkt;
    uint8_t out_burst, in_burst;
    xhci_trb_t *out_ring, *in_ring;
    uint64_t out_ring_phys, in_ring_phys;
    uint32_t out_enq, out_cycle, in_enq, in_cycle;
    uint8_t *cbw, *csw;
    uint64_t cbw_phys, csw_phys;
    uint8_t *data;
    uint64_t data_phys;
    uint32_t data_cap;
    uint32_t tag;
    uint32_t sectors;           /* 512-byte sectors on the device */
} msc;

static bool xhci_inited;

/* Set once the bring-up thread finished enumerating (successfully or not), so
 * a consumer can wait for the mass-storage device instead of racing startup. */
static volatile bool usb_bringup_done;

/* The HID thread and the mass-storage transfers share one event ring, so a
 * single locked drain serves both. */
static spinlock_t evt_lock;
static volatile uint32_t msc_evt_seen;

vec_extern(pci_device_t, pci_devices);

static _Noreturn void usb_hid_thread(pid_t pid);
static uint32_t xhci_drain(void);

static uint32_t mmio_rd(uint32_t off)
{
    return *(volatile uint32_t *) (mmio + off);
}

static void mmio_wr(uint32_t off, uint32_t val)
{
    *(volatile uint32_t *) (mmio + off) = val;
}

static void mmio_wr64(uint32_t off, uint64_t val)
{
    mmio_wr(off, (uint32_t) val);
    mmio_wr(off + 4, (uint32_t) (val >> 32));
}

static void *dma_alloc(uint64_t size, uint64_t * phys)
{
    uint64_t pages = NUM_PAGES(size);
    uint64_t p = pmm_get(pages, 0x0, __func__, __LINE__);

    if (p == 0)
        return NULL;

    memset((void *) PHYS_TO_VIRT(p), 0, pages * PAGE_SIZE);
    if (phys != NULL)
        *phys = p;
    return (void *) PHYS_TO_VIRT(p);
}

/* Enqueue one TRB, inserting a Link TRB at a wrap, and return its physical
 * address. */
static uint64_t ring_enqueue(xhci_trb_t * ring, uint32_t * enq,
                             uint32_t * cycle, uint64_t ring_phys,
                             uint64_t param, uint32_t status, uint32_t control)
{
    xhci_trb_t *t;

    if (*enq == RING_NUM) {
        t = &ring[RING_NUM];
        t->param = ring_phys;
        t->status = 0;
        t->control = (TRB_LINK << 10) | (1u << 1) | (*cycle & 1);
        *enq = 0;
        *cycle ^= 1;
    }

    t = &ring[*enq];
    t->param = param;
    t->status = status;
    t->control = (control & ~1u) | (*cycle & 1);

    uint64_t addr = ring_phys + (uint64_t) (*enq) * sizeof(xhci_trb_t);

    (*enq)++;
    return addr;
}

static void ring_doorbell(uint8_t slot, uint8_t target)
{
    mmio_wr(db_base + 4u * slot, target);
}

/* Pop the next event TRB, or return false when none is pending. */
static bool poll_event(xhci_trb_t * out)
{
    xhci_trb_t *t = &evt_ring[evt_deq];

    asm volatile ("mfence":::"memory");
    if ((t->control & 1u) != evt_cycle)
        return false;

    *out = *t;
    evt_deq = (evt_deq + 1) % EVT_NUM;
    if (evt_deq == 0)
        evt_cycle ^= 1;
    mmio_wr64(rt_base + 0x38,
              (evt_ring_phys + (uint64_t) evt_deq * sizeof(xhci_trb_t))
              | (1u << 3));
    return true;
}

/* Wait for a command completion; on success stores the slot ID if asked. */
static uint32_t wait_command(uint8_t * slot_out)
{
    for (uint32_t i = 0; i < XHCI_TIMEOUT_MS; i++) {
        xhci_trb_t ev;

        while (poll_event(&ev)) {
            if (((ev.control >> 10) & 0x3f) == TRB_EV_CMD_COMPLETE) {
                if (slot_out != NULL)
                    *slot_out = (uint8_t) (ev.control >> 24);
                return (ev.status >> 24) & 0xff;
            }
        }
        sched_sleep(1);
    }
    return 0;
}

/* Wait for a transfer event on the given slot/endpoint. */
static uint32_t wait_transfer(uint8_t slot, uint8_t dci)
{
    for (uint32_t i = 0; i < XHCI_TIMEOUT_MS; i++) {
        xhci_trb_t ev;

        while (poll_event(&ev)) {
            if (((ev.control >> 10) & 0x3f) != TRB_EV_TRANSFER)
                continue;
            if ((uint8_t) (ev.control >> 24) == slot
                && (uint8_t) ((ev.control >> 16) & 0x1f) == dci)
                return (ev.status >> 24) & 0xff;
        }
        sched_sleep(1);
    }
    return 0;
}

/* Send a command TRB and wait for completion. */
static uint32_t send_command(uint8_t slot, uint32_t type, uint64_t param,
                             uint8_t * slot_out)
{
    ring_enqueue(cmd_ring, &cmd_enq, &cmd_cycle, cmd_ring_phys, param, 0,
                 (type << 10) | ((uint32_t) slot << 24));

    asm volatile ("mfence":::"memory");
    ring_doorbell(0, 0);
    return wait_command(slot_out);
}

/* Reset one endpoint: clears a halt and restarts its data toggle. */
static uint32_t reset_endpoint(uint8_t slot, uint8_t dci)
{
    ring_enqueue(cmd_ring, &cmd_enq, &cmd_cycle, cmd_ring_phys, 0, 0,
                 (TRB_RESET_EP << 10) | ((uint32_t) dci << 16)
                 | ((uint32_t) slot << 24));

    asm volatile ("mfence":::"memory");
    ring_doorbell(0, 0);
    return wait_command(NULL);
}

/* Control transfer on EP0. Returns the completion code. */
static uint32_t control_xfer(uint8_t slot, uint8_t bm_request, uint8_t request,
                             uint16_t value, uint16_t index, uint16_t length,
                             uint64_t buf_phys)
{
    uint8_t trt = 0;

    if (length > 0)
        trt = (bm_request & 0x80) ? 3 : 2;      /* device-to-host : host-to-device */

    uint64_t setup = (uint64_t) bm_request
        | ((uint64_t) request << 8)
        | ((uint64_t) value << 16)
        | ((uint64_t) index << 32)
        | ((uint64_t) length << 48);

    /* The controller links the control stages by TRB type, so no chain bit is
     * set; Linux's xhci_queue_ctrl_tx does the same. */
    ring_enqueue(ep0_ring, &ep0_enq, &ep0_cycle, ep0_ring_phys, setup, 8,
                 (TRB_SETUP << 10) | (1u << 6) | ((uint32_t) trt << 16));

    if (length > 0) {
        uint32_t dir = (bm_request & 0x80) ? (1u << 16) : 0;
        uint32_t isp = (bm_request & 0x80) ? (1u << 2) : 0;     /* short packet */

        ring_enqueue(ep0_ring, &ep0_enq, &ep0_cycle, ep0_ring_phys, buf_phys,
                     length, (TRB_DATA << 10) | isp | dir);
        /* The status stage runs opposite to the data stage. */
        ring_enqueue(ep0_ring, &ep0_enq, &ep0_cycle, ep0_ring_phys, 0, 0,
                     (TRB_STATUS << 10)
                     | ((bm_request & 0x80) ? 0 : (1u << 16)) | (1u << 5));
    } else {
        ring_enqueue(ep0_ring, &ep0_enq, &ep0_cycle, ep0_ring_phys, 0, 0,
                     (TRB_STATUS << 10) | (1u << 16) | (1u << 5));
    }

    asm volatile ("mfence":::"memory");
    ring_doorbell(slot, 1);     /* EP0 DCI = 1 */

    uint32_t cc = wait_transfer(slot, 1);

    /* A control IN that returns fewer bytes than requested completes with a
     * short packet (13). That is a valid transfer: the config descriptor is
     * shorter than the 255 bytes asked for. */
    if (cc == 13)
        cc = CC_SUCCESS;
    return cc;
}

static void input_clear(void)
{
    memset(input_ctx, 0, sizeof(*input_ctx));
}

/* Retry a control transfer: a device just past its port reset may not answer
 * the first request. */
static uint32_t control_xfer_retry(uint8_t slot, uint8_t bm_request,
                                   uint8_t request, uint16_t value,
                                   uint16_t index, uint16_t length,
                                   uint64_t buf_phys)
{
    uint32_t cc = 0;

    for (int32_t i = 0; i < 2; i++) {
        cc = control_xfer(slot, bm_request, request, value, index, length,
                          buf_phys);
        if (cc == CC_SUCCESS)
            break;
        pit_wait(20);
    }
    return cc;
}

static uint32_t address_device(uint8_t slot, uint8_t port, uint8_t speed,
                               xhci_dev_ctx_t * dev_ctx)
{
    uint32_t max_packet = 8;

    if (speed >= 4)
        max_packet = 512;
    else if (speed == 3)
        max_packet = 64;

    input_clear();
    input_ctx->icc.add_flags = (1u << 0) | (1u << 1);
    input_ctx->slot.dev_info = ((uint32_t) speed << 20) | (1u << 27);
    input_ctx->slot.dev_info2 = (uint32_t) port << 16;
    /* The Device Address stays 0: the controller assigns slot ID as the
     * address, and a non-zero value here fails Address Device on real Intel
     * controllers. */
    input_ctx->ep[0].info2 = (EP_TYPE_CONTROL << 3) | (3 << 1)
        | (max_packet << 16);
    input_ctx->ep[0].deq = ep0_ring_phys | 1u;  /* DCS = 1 */

    dcbaa[slot] = VIRT_TO_PHYS((uint64_t) dev_ctx);
    asm volatile ("mfence":::"memory");
    return send_command(slot, TRB_ADDRESS_DEVICE,
                        VIRT_TO_PHYS((uint64_t) input_ctx), NULL);
}

static bool configure_endpoint(uint8_t slot, uint8_t dci, uint8_t interval,
                               uint16_t max_packet, uint64_t ring_phys)
{
    input_clear();
    input_ctx->icc.add_flags = (1u << 0) | (1u << dci);
    /* Context Entries is the highest valid DCI; the endpoint context array is
     * indexed by DCI - 1. */
    input_ctx->slot.dev_info = (uint32_t) dci << 27;
    xhci_ep_ctx_t *ep = &input_ctx->ep[dci - 1];

    ep->info = (uint32_t) interval << 16;
    ep->info2 = (EP_TYPE_INT_IN << 3) | (3 << 1)
        | ((uint32_t) max_packet << 16);
    ep->deq = ring_phys | 1u;

    asm volatile ("mfence":::"memory");
    return send_command(slot, TRB_CONFIGURE_EP,
                        VIRT_TO_PHYS((uint64_t) input_ctx), NULL) == CC_SUCCESS;
}

/* The firmware's USB Legacy Support keeps the controller's ports under SMI
 * control after a BIOS boot, which makes Address Device fail with a USB
 * transaction error. Take OS ownership through the OS Owned Semaphore and wait
 * for the BIOS Owned Semaphore to clear. */
static void take_ownership(void)
{
    uint32_t hcc1 = mmio_rd(CAP_HCCPARAMS1);
    uint32_t off = ((hcc1 >> 16) & 0xffff) << 2;

    for (int32_t guard = 0; off != 0 && off < 0x10000 && guard < 32; guard++) {
        uint32_t d0 = mmio_rd(off);

        if (d0 == 0 || d0 == 0xffffffffu)
            break;

        if ((d0 & 0xff) == 1) { /* USB Legacy Support */
            mmio_wr(off, d0 | (1u << 24));      /* OS Owned Semaphore */

            for (uint32_t i = 0; i < 100; i++) {
                if (!(mmio_rd(off) & (1u << 16)))
                    break;
                pit_wait(1);
            }
            klogi("USB: legacy handoff, d0 %08x\n", mmio_rd(off));
            break;
        }
        off += ((d0 >> 8) & 0xff) << 2;
    }
}

static bool port_reset(uint8_t port, uint8_t * speed)
{
    uint32_t portsc = mmio_rd(op_base + OP_PORTSC(port));

    /* Keep only the bits a write should preserve: port power, the wake enables
     * and the remove flag. Never write the read value back, or PORT_RESET
     * (bit 4) is re-asserted and a USB3 port drops to Disabled. */
    uint32_t keep = portsc & (PORTSC_PP | (1u << 25) | (1u << 26)
                              | (1u << 27) | (1u << 30));
    uint32_t change = (1u << 17) | (1u << 18) | (1u << 19) | (1u << 20)
        | (1u << 21) | (1u << 22) | (1u << 23);

    /* Force the link through RxDetect (PLS = 5) with the Link State Write
     * Strobe. This returns a device the firmware already configured to
     * Default, which Address Device needs, and revives a port a prior hot
     * reset left Disabled. An empty port (no connect, not Disabled) needs
     * nothing, and waiting on it would stall the whole enumeration. */
    uint8_t pls = (uint8_t) ((portsc >> 5) & 0xf);
    uint8_t psiv = (uint8_t) ((portsc >> 10) & 0xf);

    if (portsc & PORTSC_CCS)
        klogi("USB: port %u connect pls %u speed %u\n", (uint32_t) port,
              (uint32_t) pls, (uint32_t) psiv);

    /* A USB2 port reaches Default through a USB reset on PORT_RESET. The
     * RxDetect transition below only applies to USB3 ports. */
    if ((portsc & PORTSC_CCS) && psiv >= 1 && psiv <= 3) {
        mmio_wr(op_base + OP_PORTSC(port), keep | PORTSC_PR);

        for (uint32_t i = 0; i < XHCI_TIMEOUT_MS; i++) {
            portsc = mmio_rd(op_base + OP_PORTSC(port));
            if (!(portsc & PORTSC_PR))
                break;
            pit_wait(1);
        }

        if (portsc & PORTSC_PR)
            return false;
        if (!(portsc & PORTSC_CCS))
            return false;

        mmio_wr(op_base + OP_PORTSC(port), keep | change);

        *speed = (uint8_t) ((portsc >> 10) & 0xf);
        return true;
    }

    if ((portsc & PORTSC_CCS) || pls == 4) {
        mmio_wr(op_base + OP_PORTSC(port), keep | (5u << 5) | (1u << 16));

        for (uint32_t i = 0; i < XHCI_TIMEOUT_MS; i++) {
            portsc = mmio_rd(op_base + OP_PORTSC(port));
            if (((portsc >> 5) & 0xf) == 0)
                break;
            pit_wait(1);
        }
    }

    if (!(portsc & PORTSC_CCS))
        return false;

    /* Clear the change bits. */
    mmio_wr(op_base + OP_PORTSC(port), keep | change);

    *speed = (uint8_t) ((portsc >> 10) & 0xf);
    return true;
}

/* USB interface/base class names for the boot log. */
static const char *usb_class_name(uint8_t cls)
{
    switch (cls) {
    case 0x01:
        return "audio";
    case 0x02:
        return "communications";
    case 0x03:
        return "HID";
    case 0x05:
        return "physical";
    case 0x06:
        return "image";
    case 0x07:
        return "printer";
    case 0x08:
        return "mass storage";
    case 0x09:
        return "hub";
    case 0x0a:
        return "CDC data";
    case 0x0b:
        return "smart card";
    case 0x0d:
        return "content security";
    case 0x0e:
        return "video";
    case 0x0f:
        return "personal healthcare";
    case 0x10:
        return "audio/video";
    case 0xdc:
        return "diagnostic";
    case 0xe0:
        return "wireless";
    case 0xef:
        return "miscellaneous";
    case 0xfe:
        return "application specific";
    case 0xff:
        return "vendor specific";
    default:
        return "unknown";
    }
}

/* Configure the two bulk endpoints of the mass-storage device in one command. */
static bool msc_configure(uint8_t slot, uint8_t out_dci, uint16_t out_pkt,
                          uint64_t out_ring, uint8_t in_dci, uint16_t in_pkt,
                          uint64_t in_ring)
{
    input_clear();
    input_ctx->icc.add_flags = (1u << 0) | (1u << out_dci) | (1u << in_dci);
    input_ctx->slot.dev_info =
        ((out_dci > in_dci) ? out_dci : in_dci) << 27;

    xhci_ep_ctx_t *eo = &input_ctx->ep[out_dci - 1];

    eo->info2 = (EP_TYPE_BULK_OUT << 3) | (3 << 1)
        | ((uint32_t) msc.out_burst << 8) | ((uint32_t) out_pkt << 16);
    eo->deq = out_ring | 1u;

    xhci_ep_ctx_t *ei = &input_ctx->ep[in_dci - 1];

    ei->info2 = (EP_TYPE_BULK_IN << 3) | (3 << 1)
        | ((uint32_t) msc.in_burst << 8) | ((uint32_t) in_pkt << 16);
    ei->deq = in_ring | 1u;

    asm volatile ("mfence":::"memory");
    return send_command(slot, TRB_CONFIGURE_EP,
                        VIRT_TO_PHYS((uint64_t) input_ctx), NULL) == CC_SUCCESS;
}

/* BOT error recovery: reset the device, clear the bulk endpoint halts and
 * restart the endpoints so a stalled command does not wedge the pipe. */
static void msc_recover(void)
{
    uint8_t out_addr = (uint8_t) ((msc.out_dci >> 1)
                                  | ((msc.out_dci & 1) ? 0x80 : 0));
    uint8_t in_addr = (uint8_t) ((msc.in_dci >> 1)
                                 | ((msc.in_dci & 1) ? 0x80 : 0));

    control_xfer(msc.slot, 0x21, 0xFF, 0, 0, 0, 0);
    control_xfer(msc.slot, 0x02, 0x01, 0, out_addr, 0, 0);
    control_xfer(msc.slot, 0x02, 0x01, 0, in_addr, 0, 0);
    reset_endpoint(msc.slot, msc.out_dci);
    reset_endpoint(msc.slot, msc.in_dci);
}

/* Bulk-Only Transport: CBW (out), optional data, CSW (in). The data buffer is
 * the shared bounce buffer at msc.data_phys. Each stage is doorbelled and waited
 * for on its own, because the controller stops fetching a ring once a transfer
 * descriptor completes. */

/* Drain events until want have arrived. Spin first, since a USB transfer
 * completes in microseconds; yield only when it is slow. */
static void msc_wait_events(uint32_t want)
{
    for (uint32_t i = 0; i < XHCI_TIMEOUT_MS && msc_evt_seen < want; i++) {
        for (uint32_t j = 0; j < 2000 && msc_evt_seen < want; j++)
            xhci_drain();

        if (msc_evt_seen < want)
            sched_sleep(1);
    }
}

static bool msc_bot_once(const uint8_t * cb, uint8_t cb_len, uint32_t data_len,
                         bool in)
{
    uint8_t *cbw = msc.cbw;

    memset(cbw, 0, 31);
    cbw[0] = 0x55;              /* "USBC" */
    cbw[1] = 0x53;
    cbw[2] = 0x42;
    cbw[3] = 0x43;
    cbw[4] = (uint8_t) msc.tag;
    cbw[5] = (uint8_t) (msc.tag >> 8);
    cbw[6] = (uint8_t) (msc.tag >> 16);
    cbw[7] = (uint8_t) (msc.tag >> 24);
    msc.tag++;
    cbw[8] = (uint8_t) data_len;
    cbw[9] = (uint8_t) (data_len >> 8);
    cbw[10] = (uint8_t) (data_len >> 16);
    cbw[11] = (uint8_t) (data_len >> 24);
    cbw[12] = in ? 0x80 : 0x00;
    cbw[13] = 0;
    cbw[14] = cb_len;
    memcpy(cbw + 15, cb, cb_len);

    /* CBW (out). */
    msc_evt_seen = 0;
    ring_enqueue(msc.out_ring, &msc.out_enq, &msc.out_cycle, msc.out_ring_phys,
                 msc.cbw_phys, 31, (TRB_NORMAL << 10) | (1u << 5));
    asm volatile ("mfence":::"memory");
    ring_doorbell(msc.slot, msc.out_dci);

    msc_wait_events(1);
    if (msc_evt_seen < 1) {
        klogw("USB: msc CBW timeout\n");
        return false;
    }

    /* Data stage: one NORMAL TRB for the whole length, like Linux. */
    if (data_len != 0) {
        msc_evt_seen = 0;
        if (in)
            ring_enqueue(msc.in_ring, &msc.in_enq, &msc.in_cycle,
                         msc.in_ring_phys, msc.data_phys, data_len,
                         (TRB_NORMAL << 10) | (1u << 2) | (1u << 5));
        else
            ring_enqueue(msc.out_ring, &msc.out_enq, &msc.out_cycle,
                         msc.out_ring_phys, msc.data_phys, data_len,
                         (TRB_NORMAL << 10) | (1u << 5));
        asm volatile ("mfence":::"memory");
        ring_doorbell(msc.slot, in ? msc.in_dci : msc.out_dci);

        msc_wait_events(1);
        if (msc_evt_seen < 1) {
            klogw("USB: msc DATA timeout len %u\n", data_len);
            return false;
        }
    }

    /* CSW (in). */
    msc_evt_seen = 0;
    ring_enqueue(msc.in_ring, &msc.in_enq, &msc.in_cycle, msc.in_ring_phys,
                 msc.csw_phys, 13, (TRB_NORMAL << 10) | (1u << 2) | (1u << 5));
    asm volatile ("mfence":::"memory");
    ring_doorbell(msc.slot, msc.in_dci);

    msc_wait_events(1);
    if (msc_evt_seen < 1) {
        klogw("USB: msc CSW timeout\n");
        return false;
    }

    return msc.csw[0] == 0x55 && msc.csw[1] == 0x53 && msc.csw[2] == 0x42
        && msc.csw[3] == 0x53 && msc.csw[12] == 0;
}

/* Retry a command after the BOT error recovery. */
static bool msc_bot(const uint8_t * cb, uint8_t cb_len, uint32_t data_len,
                    bool in)
{
    for (int32_t attempt = 0; attempt < 3; attempt++) {
        if (msc_bot_once(cb, cb_len, data_len, in))
            return true;

        klogw("USB: msc op %02x len %u attempt %ld failed csw %02x%02x st %u\n",
              cb[0], data_len, (int64_t) attempt, msc.csw[0], msc.csw[1],
              msc.csw[12]);
        msc_recover();
    }

    return false;
}

static bool msc_scsi_ready(void)
{
    uint8_t cb[6] = { 0x00, 0, 0, 0, 0, 0 };    /* TEST UNIT READY */

    return msc_bot(cb, 6, 0, false);
}

/* Ask the device for its vendor and product strings and report them, so the
 * boot log names the stick instead of the xHCI controller. */
static void msc_inquiry(void)
{
    uint8_t cb[6] = { 0x12, 0, 0, 0, 36, 0 };

    if (!msc_bot(cb, 6, 36, true))
        return;

    char vendor[9];
    char product[17];

    memcpy(vendor, msc.data + 8, 8);
    vendor[8] = '\0';
    memcpy(product, msc.data + 16, 16);
    product[16] = '\0';
    for (int32_t i = 7; i >= 0 && vendor[i] == ' '; i--)
        vendor[i] = '\0';
    for (int32_t i = 15; i >= 0 && product[i] == ' '; i--)
        product[i] = '\0';
    kprintf("\033[36mU-Disk      \033[0m: %s %s\n", vendor, product);
}

static bool msc_read_capacity(void)
{
    uint8_t cb[10] = { 0x25, 0, 0, 0, 0, 0, 0, 0, 0, 0 };

    if (!msc_bot(cb, 10, 8, true))
        return false;
    uint32_t last = ((uint32_t) msc.data[0] << 24)
        | ((uint32_t) msc.data[1] << 16) | ((uint32_t) msc.data[2] << 8)
        | (uint32_t) msc.data[3];
    uint32_t bs = ((uint32_t) msc.data[4] << 24)
        | ((uint32_t) msc.data[5] << 16) | ((uint32_t) msc.data[6] << 8)
        | (uint32_t) msc.data[7];

    if (bs != 512)
        return false;
    msc.sectors = last + 1;
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

bool usb_msc_ready(void)
{
    return msc.ready || ehci_msc_ready();
}

uint32_t usb_msc_sectors(void)
{
    return msc.ready ? msc.sectors : ehci_msc_sectors();
}

bool usb_msc_read(uint32_t lba, uint32_t count, void *buf)
{
    if (!msc.ready)
        return ehci_msc_read(lba, count, buf);
    if (count == 0 || count * 512 > msc.data_cap)
        return false;
    if (!msc_read10(lba, (uint16_t) count))
        return false;
    memcpy(buf, msc.data, (size_t) count * 512);
    return true;
}

bool usb_msc_write(uint32_t lba, uint32_t count, const void *buf)
{
    if (!msc.ready)
        return ehci_msc_write(lba, count, buf);
    if (count == 0 || count * 512 > msc.data_cap)
        return false;
    memcpy(msc.data, buf, (size_t) count * 512);
    return msc_write10(lba, (uint16_t) count);
}

/* Route the USB2 ports to xHCI. The Lynx Point BIOS leaves them on EHCI by
 * default, but a full-speed device on EHCI needs a companion/TT that this PCH
 * does not provide. xHCI handles USB2 natively. */
static bool xhci_route_usb2(pci_device_t *dev)
{
    if (dev->vendor_id != 0x8086)
        return false;
    /* Lynx Point and Wildcat Point, LP and H. */
    if (dev->device_id != 0x9c31 && dev->device_id != 0x8c31
        && dev->device_id != 0x9cb1 && dev->device_id != 0x8cb1)
        return false;

    uint32_t id = PCI_MAKE_ID(dev->bus, dev->device, dev->func);
    uint32_t mask = pci_ind(id, XHCI_USB2PRM);
    uint32_t route = pci_ind(id, XHCI_USB2PR);
    uint32_t want = XHCI_USB2PR_HCSEL & mask;

    route &= ~XHCI_USB2PR_HCSEL;
    route |= want;
    pci_outd(id, XHCI_USB2PR, route);
    uint32_t now = pci_ind(id, XHCI_USB2PR);

    klogi("USB: xHCI USB2 route %08x mask %08x now %08x\n", route, mask,
          now);
    return want != 0 && (now & want) == want;
}

void usb_hid_init(void)
{
    pci_device_t *dev = NULL;

    if (xhci_inited)
        return;
    xhci_inited = true;

    spinlock_init(&evt_lock);

    /* List every USB host controller once. UHCI and OHCI entries matter on
     * machines that route full-speed ports to a companion. */
    for (uint64_t i = 0; i < vec_length(&pci_devices); i++) {
        pci_device_t *d = &vec_at(&pci_devices, i);
        uint32_t id = PCI_MAKE_ID(d->bus, d->device, d->func);
        uint32_t cls = pci_ind(id, PCI_CLASS_PERIHPERALS);

        if ((cls >> 24) == PCI_CLASS_SERIAL_BUS
            && ((cls >> 16) & 0xff) == 0x03)
            klogi("USB: PCI %02x:%02x.%x %04x:%04x class %06x\n",
                  d->bus, d->device, d->func, d->vendor_id, d->device_id,
                  cls >> 8);
    }

    for (uint64_t i = 0; i < vec_length(&pci_devices); i++) {
        pci_device_t *d = &vec_at(&pci_devices, i);
        uint32_t cls = pci_ind(PCI_MAKE_ID(d->bus, d->device, d->func),
                               PCI_CLASS_PERIHPERALS);

        /* The scan does not fill device_class/subclass/prog_if, so read the
         * class register here. */
        if ((cls >> 24) == PCI_CLASS_SERIAL_BUS
            && ((cls >> 16) & 0xff) == 0x03
            && ((cls >> 8) & 0xff) == PCI_SERIAL_USB_XHCI) {
            dev = d;
            break;
        }
    }
    bool routed = false;

    if (dev != NULL)
        routed = xhci_route_usb2(dev);

    /* The routed controller owns the USB2 ports. Skip the EHCI reset and its
     * settle wait; it has no device left to enumerate. */
    if (routed)
        klogi("USB: EHCI skipped after USB2 route\n");
    else
        ehci_init();

    if (dev == NULL) {
        klogi("USB: no xHCI controller found\n");
        return;
    }

    uint32_t id = PCI_MAKE_ID(dev->bus, dev->device, dev->func);

    pci_outw(id, PCI_CONFIG_COMMAND, pci_inw(id, PCI_CONFIG_COMMAND) | 0x6);

    pci_bar_t bar;

    pci_get_bar(&bar, id, 0);
    mmio = (volatile uint8_t *) PHYS_TO_VIRT(bar.u.address);
    vmm_map(NULL, (uint64_t) mmio, (uint64_t) bar.u.address,
            NUM_PAGES(bar.size ? bar.size : PAGE_SIZE), VMM_FLAGS_MMIO);

    uint8_t caplen = mmio[CAP_CAPLENGTH];
    uint32_t hcs1 = mmio_rd(CAP_HCSPARAMS1);
    uint32_t hcc1 = mmio_rd(CAP_HCCPARAMS1);

    /* A 64-byte context size would need a different device/input context
     * layout, so leave the controller alone rather than corrupt memory. */
    if (hcc1 & (1u << 2)) {
        kloge("USB: xHCI uses 64-byte contexts, unsupported; skipping\n");
        return;
    }

    max_slots = (uint8_t) (hcs1 & 0xff);
    max_ports = (uint8_t) (hcs1 >> 24);
    op_base = caplen;
    db_base = mmio_rd(CAP_DBOFF) & ~0x3u;
    rt_base = mmio_rd(CAP_RTSOFF) & ~0x1fu;
    klogi("USB: xHCI %04x:%04x caplen %u slots %u ports %u\n", dev->vendor_id,
          dev->device_id, caplen, max_slots, max_ports);


    /* Reset and wait for the controller to come back. */
    mmio_wr(op_base + OP_USBCMD, 0);
    mmio_wr(op_base + OP_USBCMD, USBCMD_HCRST);
    while (mmio_rd(op_base + OP_USBCMD) & USBCMD_HCRST)
        ;
    while (mmio_rd(op_base + OP_USBSTS) & USBSTS_CNR)
        ;

    dcbaa = dma_alloc((max_slots + 1) * sizeof(uint64_t), NULL);
    cmd_ring = dma_alloc((RING_NUM + 1) * sizeof(xhci_trb_t), &cmd_ring_phys);
    evt_ring = dma_alloc(EVT_NUM * sizeof(xhci_trb_t), &evt_ring_phys);
    xhci_erst_entry_t *erst = dma_alloc(sizeof(*erst), NULL);
    input_ctx = dma_alloc(sizeof(*input_ctx), NULL);
    ep0_ring = dma_alloc((RING_NUM + 1) * sizeof(xhci_trb_t), &ep0_ring_phys);
    ctrl_buf = dma_alloc(512, &ctrl_buf_phys);

    if (dcbaa == NULL || cmd_ring == NULL || evt_ring == NULL || erst == NULL
        || input_ctx == NULL || ep0_ring == NULL || ctrl_buf == NULL) {
        kloge("USB: out of memory for xHCI structures\n");
        return;
    }

    /* Some controllers reserve scratchpad buffers and read their addresses
     * from DCBAAP[0]. A missing array wedges the first command and leaves the
     * whole controller unresponsive. */
    uint32_t hcs2 = mmio_rd(CAP_HCSPARAMS2);
    uint32_t nscratch = ((hcs2 >> 16) & 0x3e0) | ((hcs2 >> 27) & 0x1f);

    if (nscratch > 0) {
        scratch_array =
            dma_alloc((uint64_t) nscratch * sizeof(uint64_t), NULL);

        if (scratch_array == NULL) {
            kloge("USB: out of memory for scratchpad array\n");
            return;
        }
        for (uint32_t i = 0; i < nscratch; i++) {
            uint64_t phys;

            if (dma_alloc(PAGE_SIZE, &phys) == NULL) {
                kloge("USB: out of memory for scratchpad page\n");
                return;
            }
            scratch_array[i] = phys;
        }
        dcbaa[0] = VIRT_TO_PHYS((uint64_t) scratch_array);
        klogi("USB: %u scratchpad buffers\n", nscratch);
    }

    mmio_wr(op_base + OP_CONFIG, max_slots);
    mmio_wr64(op_base + OP_DCBAAP, VIRT_TO_PHYS((uint64_t) dcbaa));

    cmd_enq = 0;
    cmd_cycle = 1;
    ep0_enq = 0;
    ep0_cycle = 1;
    mmio_wr64(op_base + OP_CRCR, cmd_ring_phys | 1u);

    erst->base = evt_ring_phys;
    erst->size = EVT_NUM;
    evt_deq = 0;
    evt_cycle = 1;
    mmio_wr(rt_base + 0x28, 1);         /* ERSTSZ */
    mmio_wr64(rt_base + 0x30, VIRT_TO_PHYS((uint64_t) erst));   /* ERSTBA */
    mmio_wr64(rt_base + 0x38, evt_ring_phys);   /* ERDP */
    mmio_wr(rt_base + 0x00, 1u << 1);   /* IMAN: IE */
    take_ownership();

    mmio_wr(op_base + OP_USBCMD, USBCMD_RS | USBCMD_INTE);

    /* Wait for the controller to leave the halted state. Port registers read
     * as zero while HCH stays set. */
    for (uint32_t i = 0; i < 1000; i++) {
        if (!(mmio_rd(op_base + OP_USBSTS) & USBSTS_HCH))
            break;
        pit_wait(1);
    }
    if (mmio_rd(op_base + OP_USBSTS) & USBSTS_HCH)
        klogw("USB: xHCI stays halted\n");

    /* Power every port the firmware left off. Devices stay disconnected
     * while PORTSC.PP is clear, so the scan sees no CCS. */
    uint32_t powered = 0;

    for (uint8_t port = 1; port <= max_ports; port++) {
        uint32_t portsc = mmio_rd(op_base + OP_PORTSC(port));

        if (!(portsc & PORTSC_PP)) {
            mmio_wr(op_base + OP_PORTSC(port), PORTSC_PP);
            powered++;
        }
    }
    if (powered)
        klogi("USB: xHCI powered %u ports\n", powered);

    /* The boot device was configured by the firmware. A controller reset and
     * handoff drop the port state; give the link time to report CCS again. */
    pit_wait(200);

    for (uint8_t port = 1; port <= max_ports; port++) {
        uint8_t port_speed;

        if (!port_reset(port, &port_speed))
            continue;

        /* Give the device time to settle after the port reset before it is
         * addressed. */
        pit_wait(100);

        /* The commands share one EP0 ring; reinit it per device. */
        memset(ep0_ring, 0, (RING_NUM + 1) * sizeof(xhci_trb_t));
        ep0_enq = 0;
        ep0_cycle = 1;

        uint8_t slot = 0;
        uint32_t slot_cc = send_command(0, TRB_ENABLE_SLOT, 0, &slot);

        if (slot_cc != CC_SUCCESS || slot == 0) {
            kloge("USB: enable slot on port %u failed cc %u\n", port, slot_cc);
            continue;
        }

        /* Try the speed the port reports, then slower speeds. Address Device
         * only succeeds with the matching speed. Once addressed, a second
         * Address Device returns Context State Error, so read the descriptor
         * here. */
        uint8_t *desc = ctrl_buf;
        uint8_t first = (port_speed >= 1 && port_speed <= 4) ? port_speed : 4;
        uint8_t speed = first;
        bool addressed = false;
        xhci_dev_ctx_t *dev_ctx = NULL;

        for (uint32_t ci = 0; ci < 4 && !addressed; ci++) {
            dev_ctx = dma_alloc(sizeof(*dev_ctx), NULL);

            if (dev_ctx == NULL)
                break;

            memset(ep0_ring, 0, (RING_NUM + 1) * sizeof(xhci_trb_t));
            ep0_enq = 0;
            ep0_cycle = 1;
            speed = (uint8_t) (first - ci);
            if (speed < 1)
                break;

            uint32_t addr_cc = address_device(slot, port, speed, dev_ctx);

            if (addr_cc != CC_SUCCESS) {
                klogi("USB: port %u speed %u address cc %u\n", port, speed,
                      addr_cc);
                continue;
            }

            uint32_t dcc = control_xfer_retry(slot, 0x80, 6, 1 << 8, 0, 18,
                                              ctrl_buf_phys);

            if (dcc == CC_SUCCESS && desc[0] == 18)
                addressed = true;
            break;
        }

        if (!addressed) {
            kloge("USB: address device port %u failed\n", port);
            continue;
        }
        klogi("USB: port %u addressed as slot %u (speed %u)\n", port, slot,
              speed);

        klogi("USB: port %u idVendor %02x%02x idProduct %02x%02x maxpkt %u\n",
              port, desc[9], desc[8], desc[11], desc[10], desc[7]);

        uint16_t vid = (uint16_t) (desc[8] | (desc[9] << 8));
        uint16_t pid = (uint16_t) (desc[10] | (desc[11] << 8));

        /* Configuration descriptor: read the 9-byte header first to learn
         * wTotalLength, then read exactly that many bytes. A large single
         * control read of 255 bytes fails on real controllers. */
        uint32_t cfg_cc = control_xfer_retry(slot, 0x80, 6, 2 << 8, 0, 9,
                                             ctrl_buf_phys);
        uint16_t cfg_total = (uint16_t) (desc[2] | (desc[3] << 8));

        if (cfg_cc == CC_SUCCESS && cfg_total > 9) {
            if (cfg_total > 255)
                cfg_total = 255;
            cfg_cc = control_xfer_retry(slot, 0x80, 6, 2 << 8, 0, cfg_total,
                                        ctrl_buf_phys);
        }
        if (cfg_cc != CC_SUCCESS) {
            kloge("USB: config descriptor on port %u failed cc %u total %u\n",
                  port, cfg_cc, cfg_total);
            continue;
        }

        uint16_t total = (uint16_t) (desc[2] | (desc[3] << 8));
        uint8_t cfg_value = desc[5];
        uint8_t hid_iface = 0;
        uint8_t hid_proto = 0;
        uint8_t hid_interval = 0;
        uint16_t hid_max_packet = 0;
        uint8_t iface_class = desc[5];  /* bDeviceClass as the fallback */
        bool have_iface = false;
        bool have_hid = false;
        bool found = false;
        bool have_msc = false;
        uint8_t msc_sub = 0, msc_proto = 0;
        uint8_t msc_out_addr = 0, msc_in_addr = 0;
        uint16_t msc_out_pkt = 0, msc_in_pkt = 0;
        uint8_t msc_out_burst = 0, msc_in_burst = 0;
        int32_t msc_last_ep = 0;

        for (uint32_t off = 0; off + 1 < total;) {
            uint8_t len = desc[off];
            uint8_t dtype = desc[off + 1];

            if (len < 2)
                break;
            if (dtype == 4) {           /* interface */
                if (!have_iface) {
                    iface_class = desc[off + 5];
                    have_iface = true;
                }
                have_hid = (desc[off + 5] == 3);
                have_msc = (desc[off + 5] == 8);
                if (have_hid) {
                    hid_iface = desc[off + 2];
                    hid_proto = desc[off + 7];  /* 1 keyboard, 2 mouse */
                }
                if (have_msc) {
                    msc_sub = desc[off + 6];
                    msc_proto = desc[off + 7];
                }
            } else if (dtype == 5) {    /* endpoint */
                uint8_t addr = desc[off + 2];
                uint8_t attr = desc[off + 3] & 0x3;
                uint16_t pkt = (uint16_t) (desc[off + 4]
                                           | (desc[off + 5] << 8));

                if (have_hid && (addr & 0x80) && attr == 3) {
                    hid_interval = desc[off + 6];
                    hid_max_packet = pkt;
                    found = true;
                    break;
                }
                if (have_msc && attr == 2) {
                    if (addr & 0x80) {
                        msc_in_addr = addr;
                        msc_in_pkt = pkt;
                        msc_last_ep = 1;
                    } else {
                        msc_out_addr = addr;
                        msc_out_pkt = pkt;
                        msc_last_ep = 2;
                    }
                }
            } else if (dtype == 0x30) { /* SuperSpeed endpoint companion */
                if (msc_last_ep == 1)
                    msc_in_burst = desc[off + 2] & 0x0f;
                else if (msc_last_ep == 2)
                    msc_out_burst = desc[off + 2] & 0x0f;
            }
            off += len;
        }

        /* USB mass storage, Bulk-Only Transport. */
        if (have_msc && msc_sub == 6 && msc_proto == 0x50
            && msc_out_addr != 0 && msc_in_addr != 0 && !msc.ready) {
            if (control_xfer(slot, 0x00, 9, cfg_value, 0, 0, 0) != CC_SUCCESS) {
                kloge("USB: mass storage set configuration failed\n");
                continue;
            }

            msc.slot = slot;
            msc.out_dci = (uint8_t) (2 * (msc_out_addr & 0x0f));
            msc.in_dci = (uint8_t) (2 * (msc_in_addr & 0x0f) + 1);
            msc.out_pkt = msc_out_pkt;
            msc.in_pkt = msc_in_pkt;
            msc.out_burst = msc_out_burst;
            msc.in_burst = msc_in_burst;
            msc.out_ring = dma_alloc((RING_NUM + 1) * sizeof(xhci_trb_t),
                                     &msc.out_ring_phys);

            msc.in_ring = dma_alloc((RING_NUM + 1) * sizeof(xhci_trb_t),
                                    &msc.in_ring_phys);
            msc.cbw = dma_alloc(31, &msc.cbw_phys);
            msc.csw = dma_alloc(13, &msc.csw_phys);
            msc.data_cap = 64 * 1024;
            msc.data = dma_alloc(msc.data_cap, &msc.data_phys);
            msc.out_enq = 0;
            msc.out_cycle = 1;
            msc.in_enq = 0;
            msc.in_cycle = 1;
            msc.tag = 1;

            if (msc.out_ring != NULL && msc.in_ring != NULL
                && msc.cbw != NULL && msc.csw != NULL && msc.data != NULL
                && msc_configure(slot, msc.out_dci, msc.out_pkt,
                                 msc.out_ring_phys, msc.in_dci, msc.in_pkt,
                                 msc.in_ring_phys)) {
                if (msc_scsi_ready() && msc_read_capacity()) {
                    msc.ready = true;
                    klogi("USB: mass storage %u sectors (slot %u)\n",
                          msc.sectors, slot);
                    msc_inquiry();
                } else {
                    kloge("USB: mass storage SCSI init failed\n");
                }
            } else {
                kloge("USB: mass storage configure failed\n");
            }
            continue;
        }

        if (!found
            || (hid_proto != HID_PROTO_KBD && hid_proto != HID_PROTO_MOUSE)) {
            klogi("USB: port %u device %04x:%04x class %02x (%s)\n", port,
                  vid, pid, iface_class, usb_class_name(iface_class));
            continue;
        }

        if (nhids >= HID_MAX) {
            klogi("USB: port %u HID device ignored, %d slots in use\n",
                  port, nhids);
            continue;
        }

        if (control_xfer(slot, 0x00, 9, cfg_value, 0, 0, 0) != CC_SUCCESS) {
            kloge("USB: set configuration on port %u failed\n", port);
            continue;
        }

        /* HID SET_PROTOCOL(boot). */
        ctrl_buf[0] = 0;
        control_xfer(slot, 0x21, 0x0b, 0, hid_iface, 1, ctrl_buf_phys);

        hid_t *h = &hids[nhids];
        uint16_t pkt = hid_max_packet ? hid_max_packet : 8;

        h->slot = slot;
        h->dci = 3;
        h->protocol = hid_proto;
        h->max_packet = pkt;
        h->dev_ctx = dev_ctx;
        h->ring = dma_alloc((RING_NUM + 1) * sizeof(xhci_trb_t), &h->ring_phys);
        h->buf = dma_alloc(pkt, &h->buf_phys);
        h->enq = 0;
        h->cycle = 1;

        if (h->ring == NULL || h->buf == NULL
            || !configure_endpoint(slot, h->dci, hid_interval ? hid_interval : 10,
                                   pkt, h->ring_phys)) {
            kloge("USB: configure endpoint on port %u failed\n", port);
            continue;
        }

        h->used = true;
        nhids++;
        klogi("USB: HID %s on port %u (packet %u, interval %u ms)\n",
              hid_proto == HID_PROTO_KBD ? "keyboard" : "pointer", port, pkt,
              hid_interval);
    }

    if (nhids == 0) {
        klogi("USB: no HID device found\n");
        return;
    }

    process_t *th = sched_new("usbhid", usb_hid_thread, false);

    if (th != NULL)
        sched_add(th);
}

/* USB HID usage (keyboard/keypad page 0x07) to ASCII, or 0. */
static char usb_hid_ascii(uint8_t usage, bool shift, bool caps)
{
    if (usage >= 0x04 && usage <= 0x1d) {
        char c = (char) ('a' + (usage - 0x04));

        if (shift ^ caps)
            c = (char) (c - 32);
        return c;
    }

    switch (usage) {
    case 0x1e:
        return shift ? '!' : '1';
    case 0x1f:
        return shift ? '@' : '2';
    case 0x20:
        return shift ? '#' : '3';
    case 0x21:
        return shift ? '$' : '4';
    case 0x22:
        return shift ? '%' : '5';
    case 0x23:
        return shift ? '^' : '6';
    case 0x24:
        return shift ? '&' : '7';
    case 0x25:
        return shift ? '*' : '8';
    case 0x26:
        return shift ? '(' : '9';
    case 0x27:
        return shift ? ')' : '0';
    case 0x28:
        return '\n';
    case 0x2a:
        return '\b';
    case 0x2b:
        return '\t';
    case 0x2c:
        return ' ';
    case 0x2d:
        return shift ? '_' : '-';
    case 0x2e:
        return shift ? '+' : '=';
    case 0x2f:
        return shift ? '{' : '[';
    case 0x30:
        return shift ? '}' : ']';
    case 0x31:
        return shift ? '|' : '\\';
    case 0x33:
        return shift ? ':' : ';';
    case 0x34:
        return shift ? '"' : '\'';
    case 0x35:
        return shift ? '~' : '`';
    case 0x36:
        return shift ? '<' : ',';
    case 0x37:
        return shift ? '>' : '.';
    case 0x38:
        return shift ? '?' : '/';
    default:
        return 0;
    }
}

static bool report_has(const uint8_t * rep, uint8_t code)
{
    for (int32_t i = 2; i < 8; i++) {
        if (rep[i] == code)
            return true;
    }
    return false;
}

/* Boot keyboard report: byte 0 modifiers, bytes 2..7 usage codes. */
static void hid_keyboard(const uint8_t * rep)
{
    static uint8_t prev[8];
    static bool caps;
    bool shift = (rep[0] & 0x22) != 0;  /* LShift | RShift */
    bool ctrl = (rep[0] & 0x11) != 0;   /* LCtrl | RCtrl */

    for (int32_t i = 2; i < 8; i++) {
        uint8_t code = rep[i];

        if (code == 0 || report_has(prev, code))
            continue;
        if (code == 0x39) {     /* caps lock */
            caps = !caps;
            continue;
        }
        if (code >= 0x4f && code <= 0x52) {
            /* Arrow keys become the VT sequence ESC [ A/B/C/D. */
            static const uint8_t arrow[4] = { 'C', 'D', 'B', 'A' };

            tty_server_deliver_key(0x1b);
            tty_server_deliver_key('[');
            tty_server_deliver_key(arrow[code - 0x4f]);
            continue;
        }

        char ch = usb_hid_ascii(code, shift, caps);

        if (ch == 0)
            continue;
        if (ctrl && (ch == 'd' || ch == 'D'))
            ch = 0x04;          /* Ctrl-D: EOF */
        tty_server_deliver_key((uint8_t) ch);
    }

    memcpy(prev, rep, 8);
}

/* Boot pointer report: byte 0 buttons, byte 1/2 X/Y deltas. */
static void hid_mouse(const uint8_t * rep)
{
    int32_t dx = (int8_t) rep[1];
    int32_t dy = (int8_t) rep[2];
    static uint32_t seen;

    if (seen < 8) {
        klogi("USB: pointer %d,%d buttons 0x%02x\n", dx, dy, rep[0]);
        seen++;
    }
    gfx_cursor_move(dx, dy);
}

static void hid_arm(hid_t * d)
{
    ring_enqueue(d->ring, &d->enq, &d->cycle, d->ring_phys, d->buf_phys,
                 d->max_packet, (TRB_NORMAL << 10) | (1u << 5));
    asm volatile ("mfence":::"memory");
    ring_doorbell(d->slot, d->dci);
}

/* Drain every pending transfer event and dispatch it. The HID thread and the
 * mass-storage transfers share the event ring, so one locked drain serves both
 * and neither steals the other's completions. */
static uint32_t xhci_drain(void)
{
    xhci_trb_t ev;
    uint32_t total = 0;

    spinlock_acquire(&evt_lock);
    while (poll_event(&ev)) {
        if (((ev.control >> 10) & 0x3f) != TRB_EV_TRANSFER)
            continue;

        uint8_t slot = (uint8_t) (ev.control >> 24);
        uint8_t dci = (uint8_t) ((ev.control >> 16) & 0x1f);
        uint32_t cc = (ev.status >> 24) & 0xff;

        total++;

        if (msc.slot != 0 && slot == msc.slot) {
            msc_evt_seen++;
            continue;
        }

        for (int32_t i = 0; i < nhids; i++) {
            hid_t *d = &hids[i];

            if (d->slot != slot || d->dci != dci)
                continue;
            if (cc == CC_SUCCESS) {
                if (d->protocol == HID_PROTO_KBD)
                    hid_keyboard(d->buf);
                else
                    hid_mouse(d->buf);
            }
            hid_arm(d);
            break;
        }
    }
    spinlock_release(&evt_lock);
    return total;
}

/* One thread drains the event ring for every HID device. */
_Noreturn static void usb_hid_thread(pid_t pid)
{
    (void) pid;

    for (int32_t i = 0; i < nhids; i++)
        hid_arm(&hids[i]);

    for (;;) {
        if (xhci_drain() == 0)
            sched_sleep(1);
    }
}

/* The bring-up can involve resets and control transfers with timeouts, so run
 * it in its own thread and leave it parked afterwards. */
static _Noreturn void usb_init_thread(pid_t pid)
{
    (void) pid;
    usb_hid_init();
    usb_bringup_done = true;
    for (;;)
        sched_sleep(1000);
}

/* Wait for the bring-up to finish, then report whether the mass-storage device
 * is usable. Returns false on timeout or when no device was found. */
bool usb_msc_wait(uint32_t timeout_ms)
{
    for (uint32_t i = 0; i < timeout_ms && !usb_bringup_done; i++)
        sched_sleep(1);
    return usb_msc_ready();
}

bool usb_bringup_wait(uint32_t timeout_ms)
{
    for (uint32_t i = 0; i < timeout_ms && !usb_bringup_done; i++)
        sched_sleep(1);
    return usb_bringup_done;
}

void usb_hid_start(void)
{
    process_t *th = sched_new("usbinit", usb_init_thread, false);

    if (th != NULL)
        sched_add(th);
}
