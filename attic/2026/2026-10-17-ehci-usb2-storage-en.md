# 2026-10-17: EHCI USB 2.0 mass storage

## Goal

Drive a USB 2.0 mass-storage device on an EHCI controller. Machines with a separate EHCI keep their USB 2.0 ports there, while the xHCI scan finds no mass-storage device.

## Driver

- Scan PCI for the EHCI controller, map BAR0 and reset the controller.
- Run one async queue head. Re-arm it for each control or bulk transfer.
- Enumerate the first mass-storage device: GET_DESCRIPTOR, SET_ADDRESS, GET_CONFIGURATION, SET_CONFIGURATION.
- Walk the configuration descriptor for the mass-storage interface and its bulk endpoints.
- Run Bulk-Only Transport: CBW out, optional data stage, CSW in.
- Expose READ CAPACITY and READ/WRITE(10) through `usb_msc_*`.
- Fill the qTD buffer pointer list across pages for transfers above one page.
- Use the HPET for transfer timeouts. The block server runs reads in syscall context, where the PIT interrupt does not advance.

## Faults fixed

- QH endpoint characteristics carry the endpoint number at bits 8..11. The old shift moved endpoint 2 into the endpoint field for endpoint 1.
- Configuration descriptor fields: bConfigurationValue at byte 5, bEndpointAddress at byte 2 and bmAttributes at byte 3.
- The CSW signature is "USBS", so byte 3 is 0x53.
- A BOT write data stage used the IN endpoint for both directions.
- The qTD carried one buffer pointer, so a multi-page transfer stopped at the page boundary.

## Block server

- Skip AHCI ports with DET 0 during bring-up. Empty QEMU ports no longer spend two seconds each in the reset wait.
- Restore `port_start` and `ahci_identify` after a port matches an ATA disk.

## Files Changed

| File | Change |
|------|--------|
| `kernel/device/usb/ehci.c` | EHCI bring-up, control and bulk transfers, BOT and SCSI |
| `kernel/device/usb/xhci.h` | export the EHCI mass-storage entry points |
| `kernel/device/usb/xhci.c` | fall back to EHCI for the USB block device |
| `userspace/servers/block.c` | skip absent AHCI ports; restore disk probe |
| `design/usb-live-and-games-en.md`, `design/usb-live-and-games-zh.md` | record the EHCI USB 2.0 path |

## Testing

- Kernel and userspace build with no `-Wall -Wextra` warnings.
- QEMU q35 with `ich9-usb-ehci1` and `usb-storage`: EHCI reads 524288 sectors. `ls /data/bin` reads the ext2 partition over EHCI and lists `2048`, `snake`, `tetris`.
- QEMU q35 with `qemu-xhci`: xHCI still enumerates the mass-storage device and the keyboard.
- Headless `cdrom.iso`: serial log contains `name "hansh"` and no panic, exception or corrupt line.
