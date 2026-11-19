# 2026-11-13: Move the USB host stack to userspace

Move the xHCI and EHCI drivers, USB HID decoding and mass-storage transport to `/bin/usb`.

## Kernel

- `kernel/srv/usb_srv.c` scans for xHCI and EHCI, maps BAR0 for each, grants a 512 KiB DMA arena and the PCI config I/O ports, and passes tty/GPU endpoints.
- Delete `kernel/device/usb/xhci.c`, `ehci.c` and the USB init calls from `kmain`.
- The USB block syscalls become thin `SVC_USB` forwarders over a memory object. The block server keeps its existing interface.

## Userspace

- `/bin/usb` owns controller reset, port enumeration, HID polling and Bulk-Only Transport.
- HID keys go to the tty endpoint; pointer deltas go to the GPU endpoint.
- `USB_BLOCK_INFO/READ/WRITE` requests carry data in a memory object in `xfer[1]`.
- PCI config access uses the granted `0xCF8..0xCFF` I/O ports. DMA comes from the fixed kernel grant.

## Tests

- Kernel and userspace build without warnings.
- Base and ports ISO boot to `hansh` and `bash`.
- QEMU xHCI: the HID keyboard reaches the shell, and the USB stick reports its sector count.
- QEMU EHCI: the USB stick reports its sector count.
- CI without a USB controller boots with the AHCI block path.
