# 2026-10-11: USB mass storage (xHCI BOT) for a live USB

## Goal

A live USB stick holds the kernel and initrd on its ESP and the ext2 games on its second partition. The block server reads that ext2 partition over the same USB device.

## Symptom

The kernel enumerated the QEMU `usb-storage` device, ran TEST UNIT READY and READ CAPACITY, and read sector 1 once. The second read failed. The device returned a STALL on the next CBW and stayed stalled. The block server saw no disk.

## Investigation

Ruled out in turn:

- **Syscall context.** A kernel-thread probe read sector 1 through the same path and failed the same way. The caller does not matter.
- **Interrupt enable.** Clearing `USBCMD_INTE` kept the failure.
- **Bounce size and the DIR bit.** A smaller bounce buffer and dropping bit 16 changed nothing.
- **Idle gap.** The failure looked tied to a sleep between transfers, but the same setup passed on another boot. The behaviour is a race, not a fixed gap.

Event logging exposed two faults:

- The BOT driver rang the doorbell for the bulk-OUT endpoint only. The CSW arrives on bulk-IN, whose ring no code doorbelled. The transfer produced two events, never three.
- `READ CAPACITY` returns big-endian fields. The driver read them little-endian, so the 512-byte block check failed.

The remaining fault is structural. The controller fetches TRBs from a ring until one transfer descriptor completes, then stops. A CBW, a data stage and a CSW queued with one doorbell per endpoint left the CSW unfetched. After a stall the endpoint sits Halted, and a device ClearFeature does not restart it.

## Root cause

A Bulk-Only Transport transfer needs three doorbelled stages on two endpoints. One doorbell per endpoint does not fetch all three descriptors. A device STALL halts the host endpoint, and only the xHCI `Reset Endpoint` command clears that halt. Clearing the halt on the device alone leaves the pipe dead, so every retry stalls again.

## Fix

- Ring the doorbell for both bulk endpoints.
- Read the `READ CAPACITY` fields big-endian.
- Drive the CBW, the data stage and the CSW as separate stages, each with its own doorbell and wait.
- Add `msc_recover()`: a Bulk-Only Mass Storage Reset, a ClearFeature(ENDPOINT_HALT) on both bulk endpoints, and an xHCI `Reset Endpoint` (`TRB_RESET_EP`) command per endpoint. Retry a command after recovery.
- Share the one event ring between the HID thread and the mass-storage transfers through `evt_lock` and a single `xhci_drain()`.

## Physical machine

The live USB booted to the shell and ran the games under QEMU, but the games would not start on a Skylake laptop. The controller there reserves scratchpad buffers: `HCSPARAMS2` reports one or more and the controller reads their page addresses from `DCBAAP[0]`. QEMU reports zero, so the missing array went unnoticed. Without it the first command never completes and the controller stays unresponsive.

Allocate the scratchpad array and its pages, point `DCBAAP[0]` at the array, and log the count.

## Integration

- Add syscalls `SYS_USB_BLOCK_INFO/READ/WRITE` (`0x434-0x436`) and `sys_usb_block_*`.
- `usb_msc_wait()` blocks the first query until bring-up finishes, so the block server does not race the enumeration.
- The block server falls back to the USB device when it finds no AHCI disk; `blk_read` and `blk_write` dispatch on `use_usb`.

## Files Changed

| File | Change |
|------|--------|
| `kernel/device/usb/xhci.c` | USB mass storage BOT and SCSI; shared `xhci_drain`; `msc_recover` with Reset Endpoint; scratchpad buffers |
| `kernel/device/usb/xhci.h` | export `usb_msc_ready/sectors/read/write/wait` |
| `kernel/proc/syscall.c` | `k_usb_block_info/read/write` |
| `include/syscall_nr.h` | `SYSCALL_USB_BLOCK_INFO/READ/WRITE` |
| `userspace/runtime/sysfunc.c` | `sys_usb_block_*` |
| `userspace/include/sysfunc.h` | declarations |
| `userspace/servers/block.c` | USB fallback and `blk_read`/`blk_write` |

## Testing

- `make -C kernel clean && make -C kernel` is warning-free.
- Boot from the USB stick with `-device qemu-xhci -device usb-storage`: the shell execs `tetris` from `/data/bin`, 5 runs, no fault, no read failure.
- `block: GET_INFO sector_size 512 count 524288` and `block: PART 1 start 198656 count 325599` on every boot.
- Headless boot of `cdrom.iso`: `name "hansh"`, no panic, no exception, no corrupt.
- AHCI boot of the same image: `execute "/data/bin/tetris"` still works.

## Not yet done

- Confirm the scratchpad allocation on the physical laptop.
- Exercise the shared event ring with an HID device and a mass-storage transfer at once.
- Reset the bulk data toggle without a stall, instead of resetting it inside `msc_recover`.
