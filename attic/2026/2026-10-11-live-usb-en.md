# 2026-10-11: Live USB storage over xHCI

## Goal

A live USB stick holds the kernel and initrd on its ESP and the ext2 games on its second partition. The kernel reads that partition over the same xHCI device, in QEMU and on a Skylake laptop.

## QEMU bring-up

The kernel enumerated the QEMU `usb-storage` device, ran TEST UNIT READY and READ CAPACITY, and read sector 1 once. The second read failed: the device returned a STALL on the next CBW and stayed stalled. Ruled out in turn: the syscall context, `USBCMD_INTE`, the bounce size and the DIR bit, and an idle gap. Three faults remained.

- The BOT driver rang the doorbell for the bulk-OUT endpoint only. The CSW arrives on bulk-IN, so it was never fetched.
- `READ CAPACITY` returns big-endian fields; the driver read them little-endian, so the block check failed.
- The controller fetches TRBs from a ring until one transfer descriptor completes, then stops. A CBW, a data stage and a CSW queued with one doorbell per endpoint left the CSW unfetched.

A device STALL also halts the host endpoint, and only the xHCI `Reset Endpoint` command (`TRB_RESET_EP`, type 14) clears that halt; a device ClearFeature alone leaves the pipe dead, so every retry stalls again.

Fix: ring both bulk endpoints, read the fields big-endian, drive the CBW, the data stage and the CSW as separate doorbelled stages, and add `msc_recover()` (a Bulk-Only Mass Storage Reset, ClearFeature on both bulk endpoints, and a `Reset Endpoint` per endpoint) before a retry. The HID thread and the storage transfers share one event ring through `evt_lock` and a single `xhci_drain()`.

## Real hardware

On a Skylake laptop (Sunrise Point-LP xHCI) the same code failed three times in turn, while QEMU passed each step. Comparing against Linux (`xhci.c`, `xhci-mem.c`, `xhci-ring.c`, `xhci.h`) exposed several divergences.

Port bring-up:

- The speed lives in PORTSC bits 10-13, not bits 0-3.
- A PORTSC write must keep only port power, the wake enables and the remove flag. Writing the read value back re-asserts `PORT_RESET` (bit 4, which the firmware leaves set), dropping a USB3 port to Disabled; that state survives a reboot.
- The stick is the boot device, so the firmware already configured it. Address Device needs the device returned to Default, so a USB3 port in U0 is brought back through RxDetect (PLS 5, Link State Write Strobe), not a hot reset. An empty port needs nothing; forcing RxDetect on it stalls enumeration for about ten seconds.
- The BIOS holds USB Legacy Support. The OS Owned Semaphore must be set and the BIOS Owned Semaphore polled clear, or the firmware keeps the ports under SMI control.
- The scratchpad count is `HCSPARAMS2` `((p >> 16) & 0x3e0) | ((p >> 27) & 0x1f)`; the groups were swapped.
- The input slot context Device Address stays 0; the controller assigns the slot ID.

Control transfers:

- A short packet on a control IN completes with `COMP_SHORT_PACKET` (13); treating it as failure made the config descriptor read fail. Accept 13.
- Read the config descriptor in two stages: a 9-byte header for `wTotalLength`, then that length.
- The Setup and Data stage TRBs carry no chain bit (the controller links the stages by type); the IN Data TRB carries ISP.

Bulk transfers:

- Set the endpoint MaxBurst from the SuperSpeed companion descriptor; it was left 0.
- `ring_enqueue` never wrote a LINK TRB: `*enq` was kept `% RING_NUM`, so the wrap branch never ran. Write the LINK TRB at the segment end and flip the cycle there, as Linux `inc_enq` does.
- Send the bulk data as one NORMAL TRB for the whole length with IOC and ISP.

## Peripherals

- The block server prefers the USB device, identifies the ATA disk for its model even when the data comes from the stick, and waits longer for the USB bring-up.
- The boot summary names the disk instead of the controller. `Hard Disk` is the ATA model from IDENTIFY; `U-Disk` is the SCSI INQUIRY vendor and product. Both go to the console.
- The shell gains `usb` (USB block state) and `dmesg` (kernel log ring).
- The tty buffers its serial mirror per line, so a key-by-key echo does not interleave with the kernel log.
- The input path drops the Pause sequence consumption (it swallowed a following break code), drives the repeat from the keyboard's typematic with a confirmation window, and probes the UART scratch register before enabling serial input.

## Performance

- `msc_bot` drained the event ring once per stage and then slept 1 ms. A USB transfer completes in microseconds, so every command cost at least 3 ms and loading a game ELF cost hundreds. Spin on the event ring (2000 drains) before yielding, so a fast transfer never sleeps.
- `port_reset` forced RxDetect and waited for U0 on every port. An empty port never reaches U0, so each burned the full timeout and enumeration took about ten seconds. Force the link only when a device is present or the port is Disabled; enumeration drops below a second.

## Files Changed

| File | Change |
|------|--------|
| `kernel/device/usb/xhci.c` | BOT and SCSI, shared `xhci_drain`, `msc_recover`, port bring-up, ownership handoff, control and bulk TRBs, endpoint context, ring LINK TRB, scratchpad buffers |
| `kernel/device/usb/xhci.h` | export `usb_msc_ready/sectors/read/write/wait` |
| `kernel/proc/syscall.c` | `k_usb_block_info/read/write`, `SYSCALL_KLOG` |
| `include/syscall_nr.h` | `SYSCALL_USB_BLOCK_INFO/READ/WRITE`, `SYSCALL_KLOG` |
| `kernel/lib/klog.c`, `kernel/lib/klog.h` | `klog_read` |
| `kernel/srv/block_srv.c` | print the ATA model |
| `kernel/kmain.c` | drop the PCI controller disk lines |
| `userspace/runtime/sysfunc.c`, `userspace/include/sysfunc.h` | `sys_usb_block_*`, `sys_klog` |
| `userspace/servers/block.c` | USB fallback, ATA model, longer USB wait |
| `userspace/bin/hansh.c` | `usb` and `dmesg` builtins |
| `userspace/servers/tty.c` | line-buffered serial mirror |
| `userspace/servers/input.c` | avoid a dropped break, typematic repeat |

## Testing

- `make -C kernel clean && make -C kernel` is warning-free.
- QEMU live USB (`-device qemu-xhci -device usb-storage`): `block: PART 0 start`, `ext2: read /assets/readme.txt`, `execute "/data/bin/tetris"`, no read failure.
- Physical Skylake laptop: the shell runs `tetris`, `snake` and `2048` from `/data/bin` repeatedly; `U-Disk      : Kingston DataTraveler 3.0`.
- Headless `cdrom.iso`: `name "hansh"`, no panic, no exception, no corrupt.

## Not yet done

- Exercise the shared event ring with an HID device and a mass-storage transfer at once.
- Reset the bulk data toggle without a stall, instead of resetting it inside `msc_recover`.
