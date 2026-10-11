# 2026-10-12: Live USB on real hardware (xHCI bring-up)

## Goal

Boot the live USB stick on a Skylake laptop and run the ext2 games. QEMU worked; the physical machine did not.

## Symptom

Three failures in turn:

- `USB: address device port 16 failed cc 4` — SET_ADDRESS returned a USB transaction error.
- `USB: config descriptor on port 16 failed` — the 255-byte control read never completed.
- `USB: msc DATA timeout` — a 4096-byte bulk read completed on the device (CSW status 0) but its transfer event never arrived, so `tetris` ran once and then failed with `ELF(...): File header error`.

## Root cause

Comparing against Linux (`xhci.c`, `xhci-mem.c`, `xhci-ring.c`, `xhci.h`) exposed several divergences. QEMU tolerated each; the real Sunrise Point-LP controller did not.

Port bring-up:

- **Scratchpad count.** `HCSPARAMS2` is `((p >> 16) & 0x3e0) | ((p >> 27) & 0x1f)`; the two bit groups were swapped.
- **Port speed.** The speed lives in PORTSC bits 10-13, not bits 0-3.
- **PORTSC writes.** Writing the register back with the read value re-asserted `PORT_RESET` (bit 4), which the firmware leaves set, dropping a USB3 port to Disabled across boots. A write must keep only port power, the wake enables and the remove flag.
- **Reset.** The stick is the boot device, so the firmware already configured it; Address Device needs the device returned to Default. A USB3 port in U0 is brought back with RxDetect (PLS 5, Link State Write Strobe), not a hot reset.
- **Firmware ownership.** The BIOS holds USB Legacy Support; the OS Owned Semaphore must be set and the BIOS Owned Semaphore polled clear, or the firmware keeps the ports under SMI control.

Control transfers:

- A short packet on a control IN completes with `COMP_SHORT_PACKET` (13); treating it as failure made the config descriptor read fail. Accept 13.
- The config descriptor is read in two stages: 9-byte header for `wTotalLength`, then that length.
- The Setup and Data stage TRBs carry no chain bit (the controller links the stages by type); the IN Data TRB carries ISP.
- `Reset Endpoint` is TRB type 14; 17 is `Reset Device`.

Bulk transfers:

- The endpoint context needs the SuperSpeed companion descriptor's `MaxBurst`; it was left 0.
- `ring_enqueue` never wrote a LINK TRB: `*enq` was kept `% RING_NUM`, so the wrap branch never ran. Write the LINK TRB at the segment end and flip the cycle there, as Linux `inc_enq` does.
- The bulk data stage is one NORMAL TRB for the whole length with IOC and ISP.

The input slot context's Device Address stays 0; the controller assigns the slot ID.

## Fix

Apply the points above in `kernel/device/usb/xhci.c`.

## Peripheral fixes from the same session

- `userspace/servers/block.c`: prefer the USB device; identify the ATA disk for its model even when the data comes from the stick; wait longer for the USB bring-up.
- `kernel/kmain.c`, `kernel/srv/block_srv.c`, `kernel/device/usb/xhci.c`: the boot summary names the disk instead of the controller. `Hard Disk` is the ATA model from IDENTIFY; `U-Disk` is the SCSI INQUIRY vendor and product. Both go to the console.
- `kernel/lib/klog.c` + `userspace/bin/hansh.c`: `usb` prints the USB block state, `dmesg` dumps the kernel log ring.
- `userspace/servers/tty.c`: buffer the serial mirror per line so a key-by-key echo does not interleave with the kernel log.
- `userspace/servers/input.c`: drop the Pause sequence consumption (it swallowed a following break code); drive the repeat from the keyboard's typematic with a confirmation window; probe the UART scratch register before enabling serial input.

## Follow-up: BOT wait latency

Starting a game from the stick was slow. `msc_bot` drained the event ring once per stage and then slept 1 ms; a USB transfer completes in microseconds, so every command cost at least 3 ms and loading a game ELF cost hundreds. Spin on the event ring (2000 drains) before yielding, so a fast transfer never sleeps.

## Follow-up: port enumeration stall

`port_reset` forced RxDetect and waited for U0 on every port. An empty port never reaches U0, so each burned the full timeout and enumeration took about ten seconds. Force the link only when a device is present or the port is Disabled; enumeration drops below a second.

## Files Changed

| File | Change |
|------|--------|
| `kernel/device/usb/xhci.c` | port bring-up, ownership handoff, control and bulk TRBs, endpoint context, ring LINK TRB |
| `userspace/servers/block.c` | USB preference, ATA model, longer USB wait |
| `kernel/srv/block_srv.c` | print the ATA model |
| `kernel/kmain.c` | drop the PCI controller disk lines |
| `kernel/lib/klog.c`, `kernel/lib/klog.h` | `klog_read` |
| `kernel/proc/syscall.c` | `SYSCALL_KLOG`, `k_usb_block_info/read/write` |
| `userspace/servers/tty.c` | line-buffered serial mirror |
| `userspace/servers/input.c` | avoid a dropped break, typematic repeat |
| `userspace/bin/hansh.c` | `usb` and `dmesg` builtins |

## Testing

- `make -C kernel clean && make -C kernel` is warning-free.
- QEMU live USB (`-device qemu-xhci -device usb-storage`): `block: PART 0 start`, `ext2: read /assets/readme.txt`, `execute "/data/bin/tetris"`, no msc failure.
- Physical Skylake laptop: the shell execs `tetris`, `snake` and `2048` from `/data/bin` repeatedly; `U-Disk      : Kingston DataTraveler 3.0`.
- Headless `cdrom.iso`: `name "hansh"`, no panic, no exception, no corrupt.
