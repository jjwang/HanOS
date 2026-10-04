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
#include <mm/mm.h>
#include <arch/x64/pci.h>
#include <arch/x64/pit.h>
#include <proc/sched.h>
#include <srv/tty_srv.h>
#include <device/display/gfx.h>
#include <device/usb/xhci.h>

/* Capability registers. */
#define CAP_CAPLENGTH       0x00
#define CAP_HCSPARAMS1      0x04
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
#define USBSTS_CNR          (1u << 11)

#define PORTSC_CCS          (1u << 0)
#define PORTSC_PR           (1u << 4)
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

static bool xhci_inited;

vec_extern(pci_device_t, pci_devices);

static _Noreturn void usb_hid_thread(pid_t pid);

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

    *enq = (*enq + 1) % RING_NUM;
    if (*enq == 0)
        *cycle ^= 1;
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

/* Control transfer on EP0. Returns the completion code. */
static uint32_t control_xfer(uint8_t slot, uint8_t bm_request, uint8_t request,
                             uint16_t value, uint16_t index, uint16_t length,
                             uint64_t buf_phys)
{
    uint8_t trt = 0;

    if (length > 0)
        trt = (bm_request & 0x80) ? 3 : 2;      /* device-to-host : host-to-device */
    {
        uint64_t setup = (uint64_t) bm_request
            | ((uint64_t) request << 8)
            | ((uint64_t) value << 16)
            | ((uint64_t) index << 32)
            | ((uint64_t) length << 48);

        /* The setup and data stages chain to the status stage, which carries
         * the interrupt-on-completion. */
        ring_enqueue(ep0_ring, &ep0_enq, &ep0_cycle, ep0_ring_phys, setup, 8,
                     (TRB_SETUP << 10) | (1u << 6) | (1u << 4)
                     | ((uint32_t) trt << 16));
    }

    if (length > 0) {
        uint32_t dir = (bm_request & 0x80) ? (1u << 16) : 0;

        ring_enqueue(ep0_ring, &ep0_enq, &ep0_cycle, ep0_ring_phys, buf_phys,
                     length, (TRB_DATA << 10) | dir | (1u << 4));
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
    return wait_transfer(slot, 1);
}

static void input_clear(void)
{
    memset(input_ctx, 0, sizeof(*input_ctx));
}

static bool address_device(uint8_t slot, uint8_t port, uint8_t speed,
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
    input_ctx->slot.dev_state = slot;   /* device address */
    input_ctx->ep[0].info2 = (EP_TYPE_CONTROL << 3) | (3 << 1)
        | (max_packet << 16);
    input_ctx->ep[0].deq = ep0_ring_phys | 1u;  /* DCS = 1 */

    dcbaa[slot] = VIRT_TO_PHYS((uint64_t) dev_ctx);
    asm volatile ("mfence":::"memory");
    return send_command(slot, TRB_ADDRESS_DEVICE,
                        VIRT_TO_PHYS((uint64_t) input_ctx), NULL) == CC_SUCCESS;
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

static bool port_reset(uint8_t port, uint8_t * speed)
{
    uint32_t portsc = mmio_rd(op_base + OP_PORTSC(port));

    if (!(portsc & PORTSC_CCS))
        return false;

    mmio_wr(op_base + OP_PORTSC(port), portsc | PORTSC_PR);

    for (uint32_t i = 0; i < XHCI_TIMEOUT_MS; i++) {
        portsc = mmio_rd(op_base + OP_PORTSC(port));
        if (portsc & PORTSC_PRC)
            break;
        pit_wait(1);
    }

    if (!(portsc & PORTSC_PRC))
        return false;

    mmio_wr(op_base + OP_PORTSC(port),
            (portsc & ~(PORTSC_PRC | PORTSC_CSC)) | PORTSC_PRC | PORTSC_CSC);
    *speed = (uint8_t) (portsc & 0xf);
    return true;
}

void usb_hid_init(void)
{
    pci_device_t *dev = NULL;

    if (xhci_inited)
        return;
    xhci_inited = true;

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
    mmio_wr(op_base + OP_USBCMD, USBCMD_RS | USBCMD_INTE);

    for (uint8_t port = 1; port <= max_ports && nhids < HID_MAX; port++) {
        uint8_t speed;

        if (!port_reset(port, &speed))
            continue;

        /* The commands share one EP0 ring; reinit it per device. */
        memset(ep0_ring, 0, (RING_NUM + 1) * sizeof(xhci_trb_t));
        ep0_enq = 0;
        ep0_cycle = 1;

        uint8_t slot = 0;

        if (send_command(0, TRB_ENABLE_SLOT, 0, &slot) != CC_SUCCESS
            || slot == 0) {
            kloge("USB: enable slot on port %u failed\n", port);
            continue;
        }

        xhci_dev_ctx_t *dev_ctx = dma_alloc(sizeof(*dev_ctx), NULL);

        if (dev_ctx == NULL)
            continue;

        if (!address_device(slot, port, speed, dev_ctx)) {
            kloge("USB: address device on port %u failed (speed %u)\n", port,
                  speed);
            continue;
        }
        klogi("USB: port %u addressed as slot %u (speed %u)\n", port, slot,
              speed);

        /* Descriptors share one DMA buffer. */
        uint8_t *desc = ctrl_buf;

        uint32_t cc = control_xfer(slot, 0x80, 6, 1 << 8, 0, 18, ctrl_buf_phys);

        if (cc != CC_SUCCESS) {
            kloge("USB: device descriptor on port %u failed (cc %u)\n", port,
                  cc);
            continue;
        }

        klogi("USB: port %u idVendor %02x%02x idProduct %02x%02x\n", port,
              desc[9], desc[8], desc[11], desc[10]);

        /* Full configuration descriptor. */
        if (control_xfer(slot, 0x80, 6, 2 << 8, 0, 255,
                         ctrl_buf_phys) != CC_SUCCESS) {
            kloge("USB: config descriptor on port %u failed\n", port);
            continue;
        }

        uint16_t total = (uint16_t) (desc[2] | (desc[3] << 8));
        uint8_t cfg_value = desc[5];
        uint8_t hid_iface = 0;
        uint8_t hid_proto = 0;
        uint8_t hid_interval = 0;
        uint16_t hid_max_packet = 0;
        bool have_hid = false;
        bool found = false;

        for (uint32_t off = 0; off + 1 < total;) {
            uint8_t len = desc[off];
            uint8_t dtype = desc[off + 1];

            if (len < 2)
                break;
            if (dtype == 4) {           /* interface */
                if (desc[off + 5] == 3) {       /* HID class */
                    hid_iface = desc[off + 2];
                    hid_proto = desc[off + 7];  /* 1 keyboard, 2 mouse */
                    have_hid = true;
                } else {
                    have_hid = false;
                }
            } else if (dtype == 5 && have_hid) {        /* endpoint */
                uint8_t addr = desc[off + 2];
                uint8_t attr = desc[off + 3] & 0x3;

                if ((addr & 0x80) && attr == 3) {
                    hid_interval = desc[off + 6];
                    hid_max_packet = (uint16_t) (desc[off + 4]
                                                 | (desc[off + 5] << 8));
                    found = true;
                    break;
                }
            }
            off += len;
        }

        if (!found
            || (hid_proto != HID_PROTO_KBD && hid_proto != HID_PROTO_MOUSE)) {
            klogi("USB: port %u is not a HID boot device\n", port);
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

/* One thread drains the event ring for every HID device, so the shared event
 * ring has a single owner. */
_Noreturn static void usb_hid_thread(pid_t pid)
{
    (void) pid;

    for (int32_t i = 0; i < nhids; i++)
        hid_arm(&hids[i]);

    for (;;) {
        xhci_trb_t ev;
        bool got = false;

        while (poll_event(&ev)) {
            if (((ev.control >> 10) & 0x3f) != TRB_EV_TRANSFER)
                continue;

            uint8_t slot = (uint8_t) (ev.control >> 24);
            uint8_t dci = (uint8_t) ((ev.control >> 16) & 0x1f);
            uint32_t cc = (ev.status >> 24) & 0xff;

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
                got = true;
                break;
            }
        }

        if (!got)
            sched_sleep(1);
    }
}

/* The bring-up can involve resets and control transfers with timeouts, so run
 * it in its own thread and leave it parked afterwards. */
static _Noreturn void usb_init_thread(pid_t pid)
{
    (void) pid;
    usb_hid_init();
    for (;;)
        sched_sleep(1000);
}

void usb_hid_start(void)
{
    process_t *th = sched_new("usbinit", usb_init_thread, false);

    if (th != NULL)
        sched_add(th);
}
