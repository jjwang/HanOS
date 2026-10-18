# 2026-10-18: Haswell USB 2.0 storage and xHCI routing

## Result

The Haswell-ULT laptop boots HanOS from a U disk, mounts the ext2 data partition and runs the games. The shell starts after the boot summary, the `U-Disk` line and the disk line print.

## Symptom

- The firmware booted from the U disk. The kernel saw no mass-storage device.
- `dmesg usb` showed the EHCI controller with no storage and the xHCI controller with 13 empty ports.
- EHCI port 1 reported `CCS=1` and full speed. The first control transfer failed.

## Root cause

Haswell routes the USB2 pins of the blue ports to EHCI. The EHCI controller has no companion (`N_CC=0`) and its root hub TT rejected the split transaction for the full-speed device. The xHCI controller held the USB3 pins, so it saw no device.

The EHCI driver had three more faults:

- A 64-bit capable controller (`HCCPARAMS` bit 0) reads the Appendix B high buffer pointers. The driver used 32-byte qTDs and 48-byte QHs, so the controller read the next qTD as high pointers and raised Host System Error.
- The bulk queue head hard-coded high speed. The device was full speed.
- The status-stage qTD had a null buffer pointer. The driver read the port registers before the controller left Halted.

## Fixes

### Route USB2 to xHCI

- Read `USB2PR` at xHCI PCI config 0xd0 and `USB2PRM` at 0xd4.
- Clear the low 15 route bits, then set the bits allowed by the mask.
- Write the result back to 0xd0 before the controller scan.
- Guard the write by Intel 9c31 and 8c31. xHCI handles USB2 natively, so the full-speed device needs no EHCI TT.

### xHCI

- Wait for `USBSTS.HCH` to clear after `USBCMD.RS`.
- Set `PORTSC.PP` on every unpowered port and wait 500 ms.
- Reset USB2 ports through `PORTSC.PR`; reset USB3 ports through RxDetect.
- Start Address Device at the speed PORTSC reports, then try slower speeds.
- Run the BIOS legacy handoff before starting the controller.

### EHCI

- Add the five high buffer-pointer dwords to qTD and QH. Pad qTD to 64 bytes and QH to 96 bytes.
- Read the first 8 descriptor bytes at address 0, set the address, then read the device descriptor.
- Wait 20 ms after the port reset for the USB reset recovery.
- Use the enumerated speed in the bulk QH.
- Stop the async schedule, wait for `USBSTS.ASS` to clear, rewrite the QH, then set `ASE` again.
- Clear `USBSTS` bits 0..5 before each arm. Give the status qTD a valid buffer pointer.
- Set the QH multiplier to 1 and the root-hub TT port for full/low speed.
- Power ports the firmware left off and wait for the link state.
- Take the BIOS legacy handoff and scan both EHCI controllers.
- Use HPET delays. PIT waits do not advance in the block syscall context.

### AHCI

- Wake the AHCI interface before clearing `ST`.
- Reset a port that reports DET 0 with a valid signature. Skylake can report DET 0 while the disk is powered down; Haswell reports DET 1 or 3.
- Skip a DET 0 port whose signature has the `0xffff` high half. These are stale empty-port signatures; a reset wait costs about 800 ms per port.
- Wait 500 ms for DET 0 and 2 s for DET 1/2.
- Identify the ATA disk before the USB preference takes effect, so the model stays in the summary when the data comes from the U disk.

### Boot flow

- Wait up to 5 s for the USB bring-up thread before the shell. The `U-Disk` and controller lines belong to the boot summary.
- Probe the block server with a 6 s timeout before the shell. Retry in `blkprobe` on timeout.
- Retry the ext2 mount ten times. The block server may still wait for USB on the first request.
- Grow the kernel log ring to 48 KiB. A 64 KiB ring resets the Haswell before serial init.
- Add `dmesg [keyword...]`. A line prints when it contains every keyword, case-insensitive.

## Files Changed

| File | Change |
|------|--------|
| `kernel/device/usb/ehci.c` | EHCI controller, 64-bit structures, handoff, port power, BOT |
| `kernel/device/usb/xhci.c` | xHCI USB2 routing, USB2 reset, port power, HCH wait, speed |
| `kernel/device/usb/xhci.h` | EHCI and bring-up exports |
| `kernel/kmain.c` | USB bring-up wait and block probe timing |
| `kernel/srv/block_srv.c`, `kernel/srv/block_srv.h` | bounded block probe and background retry |
| `kernel/lib/klog.h` | 48 KiB log ring |
| `userspace/bin/hansh.c` | `dmesg [keyword...]`; help entries |
| `userspace/servers/ext2.c` | mount retry |
| `userspace/servers/block.c` | AHCI port bring-up; USB preference |
| `attic/2026/2026-10-18-haswell-usb2-ports-en.md`, `attic/2026/2026-10-18-haswell-usb2-ports-zh.md` | this note |

## Testing

- Kernel and userspace build with no `-Wall -Wextra` warnings.
- QEMU EHCI: mass storage and ext2 reads pass.
- QEMU xHCI: SuperSpeed storage and high-speed keyboard enumerate.
- Headless `cdrom.iso`: `name "hansh"` appears; no panic, exception or corrupt line.
- Haswell: the xHCI route report prints; the U disk shows 30277632 sectors; `/data` mounts; `tetris`, `2048` and `snake` run.
- Skylake: `U-Disk : Kingston DataTraveler 3.0` and `Hard Disk : SanDisk SD8TB8U256G1` print before the shell.
- Haswell and Skylake boot order: `U-Disk`, `Hard Disk` and the USB lines print before the shell.

## TODO

- Hotplug: rescan after boot. Handle connect change. Add a `usbrescan` command.
- EHCI full speed: the TT path has no test on a machine without xHCI routing. Add a companion/UHCI driver. Decide the fallback after a hardware matrix.
- EHCI BOT recovery: send ClearFeature on a halted endpoint. Reinitialize the QH and retry. xHCI has `msc_recover`.
- EHCI transfer size: one qTD covers about 8 KiB. Add qTD chains and raise the bounce buffer to 64 KiB.
- Block size: check the READ CAPACITY block length. Support 4Kn media.
- Partition discovery: parse MBR. Stop hard-coding ext2 partition index 1. Select a partition by type or UUID.
- Hubs: enumerate downstream devices. The drivers scan root ports only.
- Multiple controllers, devices and LUNs: the drivers stop at the first mass-storage device.
- UAS: xHCI runs BOT only. Add UAS.
- xHCI routing: move the PCH IDs into a table. Add other Intel PCH generations and quirks.
- Power: suspend, resume, over-current and port power recovery.
- Performance: event-driven completion. Batch bulk transfers. Reduce busy polling.
- Boot: replace the 5 s USB wait and the 6 s block probe with a readiness signal. Enter the shell early when no USB is present.
- DMA and IOMMU: record the EHCI 32-bit limit. Add IOMMU mappings when the firmware enables it.
- Test matrix: Haswell, Skylake, QEMU EHCI and xHCI, USB2 and USB3 sticks, hubs and hotplug.
