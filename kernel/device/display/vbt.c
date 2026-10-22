/**-----------------------------------------------------------------------------

 @file    vbt.c
 @brief   VBT panel timing reader

 @details
 @verbatim

  Reads the VBT from the ACPI OpRegion at the fixed offset or through
  ASLE.RVDA, then decodes the LFP (41/42) or generic DTD (58) panel timing.
  Only the data path i915 uses for integrated graphics is implemented; the
  VBIOS/SPI paths are not part of the VBT module.

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>

#include <mm/mm.h>
#include <arch/x64/pci.h>
#include <device/display/vbt.h>

#define OPREGION_SIGNATURE  "IntelGraphicsMem"
#define OPREGION_ASLE       0x300
#define OPREGION_VBT        0x400
#define OPREGION_SIZE       0x2000

struct vbt_dvo_timing {
    uint16_t clock;             /* 10 kHz */
    uint8_t hactive_lo;
    uint8_t hblank_lo;
    uint8_t hblank_hi:4;
    uint8_t hactive_hi:4;
    uint8_t vactive_lo;
    uint8_t vblank_lo;
    uint8_t vblank_hi:4;
    uint8_t vactive_hi:4;
    uint8_t hsync_off_lo;
    uint8_t hsync_pulse_width_lo;
    uint8_t vsync_pulse_width_lo:4;
    uint8_t vsync_off_lo:4;
    uint8_t vsync_pulse_width_hi:2;
    uint8_t vsync_off_hi:2;
    uint8_t hsync_pulse_width_hi:2;
    uint8_t hsync_off_hi:2;
    uint8_t himage_lo;
    uint8_t vimage_lo;
    uint8_t vimage_hi:4;
    uint8_t himage_hi:4;
    uint8_t h_border;
    uint8_t v_border;
    uint8_t rsvd1:3;
    uint8_t digital:2;
    uint8_t vsync_positive:1;
    uint8_t hsync_positive:1;
    uint8_t non_interlaced:1;
} __attribute__((packed));

static uint16_t vbt_rd16(const uint8_t * p)
{
    return (uint16_t) p[0] | ((uint16_t) p[1] << 8);
}

static uint32_t vbt_rd32(const uint8_t * p)
{
    return (uint32_t) p[0] | ((uint32_t) p[1] << 8)
        | ((uint32_t) p[2] << 16) | ((uint32_t) p[3] << 24);
}

static void vbt_mode_finish(display_mode_t * mode)
{
    uint32_t htotal = mode->hactive + mode->hblank;
    uint32_t vtotal = mode->vactive + mode->vblank;

    mode->valid = true;
    mode->from_edid = false;
    mode->preferred = true;
    mode->pitch = mode->hactive * 4;
    if (htotal != 0 && vtotal != 0)
        mode->refresh_hz = (uint32_t) (((uint64_t) mode->pixel_clock_khz
                                        * 1000) / ((uint64_t) htotal
                                                   * vtotal));
}

/* A usable BDB carries at least one of the panel timing blocks and starts
 * with block 1. The latter rejects the stub VBT some firmware leaves at
 * OpRegion+0x400. */
static bool vbt_bdb_has_panel(const uint8_t * bdb, uint16_t bdb_size,
                              uint16_t header)
{
    uint32_t i;

    if (header < 22 || header > bdb_size || bdb[header] != 1)
        return false;

    for (i = header; i + 3 <= bdb_size;) {
        uint8_t id = bdb[i];
        uint16_t size = vbt_rd16(bdb + i + 1);

        if (id == 41 || id == 42 || id == 58)
            return true;
        if (i + 3 + size > bdb_size)
            break;
        i += 3 + size;
    }

    return false;
}

static bool vbt_bdb_valid(const uint8_t * vbt)
{
    static const char bdb_sig[16] = "BIOS_DATA_BLOCK";
    const uint8_t *bdb;
    uint16_t vbt_size = vbt_rd16(vbt + 0x18);
    uint32_t i;

    if (vbt_size < 0x30)
        return false;

    bdb = vbt + vbt_rd32(vbt + 0x1c);
    if (memcmp(bdb, bdb_sig, sizeof(bdb_sig)) == 0)
        return true;

    for (i = 0; i + 16 <= vbt_size; i += 4) {
        if (memcmp(vbt + i, bdb_sig, 16) == 0)
            return true;
    }

    return false;
}

static bool vbt_use_dvo(const uint8_t * data, uint32_t data_size,
                        const uint8_t * ptrs, uint32_t ptrs_size,
                        uint8_t panel, display_mode_t * mode)
{
    struct vbt_dvo_timing d;
    const uint8_t *p;
    uint16_t dvo_off;
    uint8_t dvo_size;

    if (ptrs_size < 1 + (uint32_t) (panel + 1) * 9)
        return false;

    p = ptrs + 1 + (uint32_t) panel * 9;
    dvo_off = vbt_rd16(p + 3);
    dvo_size = p[5];
    if (dvo_size < sizeof(d) || (uint32_t) dvo_off + sizeof(d) > data_size)
        return false;

    memcpy(&d, data + dvo_off, sizeof(d));
    mode->pixel_clock_khz = (uint32_t) d.clock * 10;
    mode->hactive = (uint16_t) ((d.hactive_hi << 8) | d.hactive_lo);
    mode->hblank = (uint16_t) ((d.hblank_hi << 8) | d.hblank_lo);
    mode->hsync_offset = (uint16_t) ((d.hsync_off_hi << 8) | d.hsync_off_lo);
    mode->hsync_pulse = (uint16_t) ((d.hsync_pulse_width_hi << 8)
                                     | d.hsync_pulse_width_lo);
    mode->vactive = (uint16_t) ((d.vactive_hi << 8) | d.vactive_lo);
    mode->vblank = (uint16_t) ((d.vblank_hi << 8) | d.vblank_lo);
    mode->vsync_offset = (uint16_t) ((d.vsync_off_hi << 4) | d.vsync_off_lo);
    mode->vsync_pulse = (uint16_t) ((d.vsync_pulse_width_hi << 4)
                                     | d.vsync_pulse_width_lo);
    mode->hsync_positive = d.hsync_positive;
    mode->vsync_positive = d.vsync_positive;
    mode->interlaced = !d.non_interlaced;

    if (mode->pixel_clock_khz < 10000 || mode->hactive < 640
        || mode->vactive < 480 || mode->hblank == 0 || mode->vblank == 0)
        return false;
    if (mode->hsync_offset + mode->hsync_pulse > mode->hblank
        || mode->vsync_offset + mode->vsync_pulse > mode->vblank)
        return false;

    vbt_mode_finish(mode);
    return true;
}

static bool vbt_use_generic(const uint8_t * generic, uint32_t size,
                            uint32_t index, display_mode_t * mode)
{
    uint16_t gdtd_size;
    uint32_t count;
    const uint8_t *d;
    uint32_t clock;

    if (generic == NULL || size < 2)
        return false;

    gdtd_size = vbt_rd16(generic);
    if (gdtd_size < 28)
        return false;
    count = (size - 2) / gdtd_size;
    if (index >= count)
        return false;

    d = generic + 2 + index * gdtd_size;
    clock = vbt_rd32(d);
    mode->pixel_clock_khz = clock;
    mode->hactive = vbt_rd16(d + 4);
    mode->hblank = vbt_rd16(d + 6);
    mode->hsync_offset = vbt_rd16(d + 8);
    mode->hsync_pulse = vbt_rd16(d + 10);
    mode->vactive = vbt_rd16(d + 12);
    mode->vblank = vbt_rd16(d + 14);
    mode->vsync_offset = vbt_rd16(d + 16);
    mode->vsync_pulse = vbt_rd16(d + 18);
    mode->hsync_positive = (d[24] & 0x80) != 0;
    mode->vsync_positive = (d[24] & 0x40) != 0;
    mode->interlaced = false;

    if (mode->pixel_clock_khz < 10000 || mode->hactive < 640
        || mode->vactive < 480 || mode->hblank == 0 || mode->vblank == 0)
        return false;

    vbt_mode_finish(mode);
    return true;
}

static uint8_t *vbt_rvda(const uint8_t * base)
{
    const uint8_t *asle = base + OPREGION_ASLE;
    uint64_t rvda = 0;
    uint32_t rvds = 0;
    uint8_t major = base[23];
    uint8_t minor = base[22];

    memcpy(&rvda, asle + 186, sizeof(rvda));
    memcpy(&rvds, asle + 194, sizeof(rvds));
    if (rvda == 0 || rvds < 0x30)
        return NULL;

    if (major > 2 || (major == 2 && minor >= 1))
        return (uint8_t *) base + rvda;
    return (uint8_t *) PHYS_TO_VIRT(rvda);
}

static uint8_t *vbt_find(const uint8_t * base)
{
    static const char vbt_sig[4] = "$VBT";
    uint8_t *first = NULL;

    for (uint32_t i = 0; i + 4 <= OPREGION_SIZE; i += 4) {
        if (memcmp(base + i, vbt_sig, sizeof(vbt_sig)) != 0)
            continue;
        if (first == NULL)
            first = (uint8_t *) base + i;
        if (vbt_bdb_valid(base + i))
            return (uint8_t *) base + i;
    }

    return first;
}

bool vbt_panel_mode(gfx_pci_t * pci, pci_device_t dev, display_mode_t * mode)
{
    static const char sig[16] = OPREGION_SIGNATURE;
    static const char vbt_sig[4] = "$VBT";
    static const char bdb_sig[16] = "BIOS_DATA_BLOCK";
    uint32_t id = PCI_MAKE_ID(dev.bus, dev.device, dev.func);
    uint32_t asls = pci_ind(id, 0xfc);
    const uint8_t *base;
    uint8_t *vbt;
    uint8_t *bdb;
    uint16_t vbt_size;
    uint16_t bdb_size;
    uint16_t bdb_header;
    const uint8_t *opts = NULL;
    const uint8_t *ptrs = NULL;
    const uint8_t *data = NULL;
    const uint8_t *generic = NULL;
    uint32_t opts_size = 0, ptrs_size = 0, data_size = 0, generic_size = 0;
    uint8_t panel = 0;

    (void) pci;

    if (asls == 0)
        return false;

    base = (const uint8_t *) PHYS_TO_VIRT(asls);
    if (memcmp(base, sig, sizeof(sig)) != 0)
        return false;

    /* RVDA is authoritative and may point outside the OpRegion. */
    vbt = vbt_rvda(base);
    if (vbt != NULL && memcmp(vbt, vbt_sig, sizeof(vbt_sig)) != 0)
        vbt = NULL;
    if (vbt == NULL)
        vbt = vbt_find(base);
    if (vbt == NULL)
        return false;

    vbt_size = vbt_rd16(vbt + 0x18);
    if (vbt_size < 0x30)
        return false;

    bdb = vbt + vbt_rd32(vbt + 0x1c);
    if (memcmp(bdb, bdb_sig, sizeof(bdb_sig)) != 0) {
        uint32_t bdb_off = vbt_rd32(vbt + 0x1c);
        uint16_t cand_size = vbt_rd16(bdb + 20);
        uint16_t cand_header = vbt_rd16(bdb + 18);

        /* Linux validates only the BDB range, not the ASCII signature. */
        if (cand_header < 22 || cand_size < cand_header
            || bdb_off + cand_size > vbt_size
            || !vbt_bdb_has_panel(bdb, cand_size, cand_header))
            return false;

        bdb_size = cand_size;
        bdb_header = cand_header;
    } else {
        bdb_size = vbt_rd16(bdb + 20);
        bdb_header = vbt_rd16(bdb + 18);
        if (bdb_size < bdb_header || bdb_size > vbt_size)
            return false;
    }

    for (uint32_t i = bdb_header; i + 3 <= bdb_size;) {
        uint8_t block_id = bdb[i];
        uint32_t block_size = vbt_rd16(bdb + i + 1);
        const uint8_t *payload = bdb + i + 3;

        if (i + 3 + block_size > bdb_size)
            break;

        switch (block_id) {
        case 40:
            opts = payload;
            opts_size = block_size;
            break;
        case 41:
            ptrs = payload;
            ptrs_size = block_size;
            break;
        case 42:
            data = payload;
            data_size = block_size;
            break;
        case 58:
            generic = payload;
            generic_size = block_size;
            break;
        default:
            break;
        }

        i += 3 + block_size;
    }

    if (opts != NULL && opts_size >= 1) {
        panel = opts[0];
        if (panel > 15)
            panel = 0;
    }

    for (uint32_t i = 0; i < 16; i++) {
        uint8_t entry = (i == 0) ? panel : (uint8_t) ((panel + i) & 0xf);

        if (vbt_use_generic(generic, generic_size, entry, mode))
            return true;
        if (vbt_use_dvo(data, data_size, ptrs, ptrs_size, entry, mode))
            return true;
    }

    return false;
}
