/**-----------------------------------------------------------------------------

 @file    edid.c
 @brief   EDID reading and validation

 @details
 @verbatim

  EDID sources in i915 order: ACPI OpRegion ASLE mailbox #5, then the eDP
  I2C-over-AUX DDC. AUX reads power the panel VDD first, matching i915
  intel_pps_vdd_on() followed by intel_dp_set_power(D0). The mode selector
  remains display_mode_from_edid().

 @endverbatim

 **-----------------------------------------------------------------------------
 */
#include <stdint.h>
#include <string.h>

#include <mm/mm.h>
#include <arch/x64/pci.h>
#include <device/display/edid_read.h>
#include <device/display/skl_display.h>

bool edid_validate(const uint8_t * buf)
{
    static const uint8_t header[8] = {
        0x00, 0xff, 0xff, 0xff, 0xff, 0xff, 0xff, 0x00
    };
    uint32_t sum = 0;

    if (buf == NULL || memcmp(buf, header, sizeof(header)) != 0)
        return false;

    for (uint32_t i = 0; i < 128; i++)
        sum += buf[i];
    if ((sum & 0xff) != 0)
        return false;

    /* Bytes 54/55 are the first DTD pixel clock; zero means no timing. */
    return buf[54] != 0 || buf[55] != 0;
}

bool edid_read_opregion(pci_device_t dev, uint8_t * buf, uint32_t len)
{
    static const char signature[16] = "IntelGraphicsMem";
    uint32_t id = PCI_MAKE_ID(dev.bus, dev.device, dev.func);
    uint32_t asls = pci_ind(id, 0xfc);
    const uint8_t *base;
    uint32_t mboxes;
    uint32_t phed;
    uint32_t blocks;
    uint32_t bytes;

    if (asls == 0 || len < 128)
        return false;

    base = (const uint8_t *) PHYS_TO_VIRT(asls);
    if (memcmp(base, signature, sizeof(signature)) != 0)
        return false;

    mboxes = *(const uint32_t *) (base + 0x58);
    if (!(mboxes & (1u << 4)))
        return false;

    phed = *(const uint32_t *) (base + 0x1c00);
    blocks = phed & 0x3;
    if (blocks == 0)
        return false;

    bytes = blocks * 128;
    if (bytes > len)
        bytes = len;
    memcpy(buf, base + 0x1c04, bytes);

    return edid_validate(buf);
}

bool edid_read_aux(gfx_pci_t * pci, uint8_t * buf, uint32_t len)
{
    if (pci == NULL || buf == NULL || len < 128)
        return false;

    if (!gfx_edp_panel_on(pci))
        return false;

    if (!skl_edid_read(pci, buf, len))
        return false;

    return edid_validate(buf);
}
