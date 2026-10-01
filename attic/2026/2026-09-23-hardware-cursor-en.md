# 2026-09-23: Hardware Cursor, PS/2 and USB HID Pointers

## Overview

Drive a mouse cursor from the GPU. The Skylake display engine's hardware cursor plane composites an arrow over the console. It does not touch the scan-out framebuffer. The pointer device moves it. Both input paths are implemented: the PS/2 auxiliary port and USB HID over the xHCI controller.

## Changes

- `gfx_reg.h`: add the hardware-cursor register macros.
- The new macros cover FBC control, live surface and cursor watermarks.
- The new macros also cover display-data-buffer allocation and position masks.
- The new macros sit alongside the existing control/base/position.
- `skl_display.c`: `skl_cursor_init()` allocates and paints a 64x64 ARGB GTT surface, gives it a data-buffer allocation and watermark, and programs `CURCNTR` / `CURPOS` / `CURBASE`. `skl_cursor_set()` / `skl_cursor_move()` place it. `skl_cursor_selftest()` walks it around the screen.
- `gfx.c` / `gfx.h`: after a successful mode set, initialise the cursor, run the self test, and expose `gfx_cursor_move()`.
- New `device/usb/xhci.{c,h}`: a minimal xHCI driver that resets the controller, enumerates a HID pointer on any root port, and polls its interrupt IN endpoint from a kernel thread. The reports feed `gfx_cursor_move()`.
- `keyboard.c`: decode the three-byte PS/2 mouse packet and feed the deltas to the cursor. This remains the fallback for machines whose pointing device sits behind the PS/2 controller.
- `kmain.c`: start the USB bring-up (`usb_hid_start()`) in its own kernel thread after SMP init, so a slow or stuck enumeration cannot block the boot.
- `gfx.h`: make the header self-contained (include `stdint.h`, `stdbool.h` and `sys/pci.h`).

## Implementation

### Cursor surface and registers

- `skl_cursor_init()` allocates a 64x64 ARGB surface (`CURSOR_SIZE * CURSOR_SIZE * 4` = 16 KiB) through the same `gfx_alloc()` path as the primary plane.
- `cursor_draw()` `memset`s the surface to transparent and copies the 24x24 bitmap into the top-left corner.
- It flushes the writes with `wbinvd` before the surface becomes live.
- The cursor control and position registers are `CURACNTR` and `CURAPOS`.
- The surface-address register is `CURABASE` and the FBC register is `CUR_FBC_CTL_A`.
- The watermark registers are `CUR_WM_0..7` and `CUR_WM_TRANS`.
- The display-data-buffer allocation register is `CUR_BUF_CFG`.
- `CUR_BUF_CFG` is set to blocks 446..477, immediately above the primary plane's blocks 0..445.
- Every cursor watermark is `PLANE_WM_EN | PLANE_WM_IGNORE_LINES | PLANE_WM_BLOCKS(32)` so the small cursor fetch never starves.
- The control register uses `CURSOR_MODE_64_ARGB_AX` (`0x27`, square 64x64 ARGB with an alpha X-channel).
- The write order is FBC, control, position, then base; the `CURABASE` write arms the whole double-buffered cursor state.
- `cursor_pos()` encodes the position as a sign-magnitude value (sign bit plus a 15-bit magnitude per axis) with `CURSOR_HOT_X`/`CURSOR_HOT_Y` subtracted, so the (7,4) hotspot lands on the pointer tip.
- `skl_cursor_set()` clamps to the mode and re-writes `CURAPOS` then `CURABASE`.
- The drivers use `skl_cursor_move()` for incremental movement.
- `skl_cursor_selftest()` visits the four corners and the centre so the cursor is visible before a pointer driver exists.
- The bitmap is the DMZ-White `left_ptr`: a 24x24 ARGB white arrow with a black outline and an anti-aliased edge.

### PS/2 auxiliary pointer

- `keyboard.c` enables the auxiliary port during keyboard init.
- It sets the controller config bit with `0xA8` (enable aux). It reads the config byte with `0x20`, sets bit 1 (aux interrupt), and writes it back with `0x60`.
- It sends `0xF6` (defaults) and `0xF4` (enable) to the mouse, and registers `mouse_callback` on IRQ12.
- The callback reads one byte from port `0x60` per interrupt and runs a three-byte state machine.
- The first byte must have bit 3 set and is kept as the flags byte.
- The next two bytes are the X and Y deltas, each sign-extended from bit 4 (X) and bit 5 (Y) of the flags.
- The code uses the X delta as-is and negates the Y delta, because the screen Y axis points down while PS/2 reports up as positive.
- It then calls `gfx_cursor_move()`.

### xHCI controller bring-up

- `usb_hid_init()` finds the controller by reading the PCI class register of every device (`0x0C` serial bus, subclass `0x03`, prog-if `0x30` xHCI).
- It enables memory and bus-master access and maps BAR0 through `PHYS_TO_VIRT`.
- It refuses to continue if `HCCPARAMS1` reports a 64-byte context size, because that needs a different context layout.
- The controller is reset with `USBCMD.HCRST`; the code waits for `USBCMD.HCRST` to clear and `USBSTS.CNR` to clear.
- All shared structures are allocated with `dma_alloc()` (contiguous physical pages, zeroed, accessed through the direct map).
- The allocations include the device-context base array, the command ring and the event ring.
- The event ring carries an ERST entry.
- They also include the device and input contexts, the EP0 ring and the interrupt ring.
- They also include a 64-byte interrupt buffer and a 512-byte control buffer.
- `OP_CONFIG` is set to the slot count. `OP_DCBAAP` points at the array. `OP_CRCR` points at the command ring.
- `ERSTSZ`, `ERSTBA` and `ERDP` describe the event ring. `IMAN` is set to enable interrupts. The controller is started with `USBCMD_RS | USBCMD_INTE`.
- TRBs carry the cycle bit. Rings append a Link TRB at a wrap, and `poll_event()` flips the consumer cycle bit when it passes the wrap, so the controller and the driver agree on ring ownership.

### Enumeration and descriptor parsing

- For each root port, `port_reset()` requires `PPORTSC.CCS`, sets `PR`, waits for `PRC`, acknowledges `PRC`/`CSC`, and reads the port speed.
- `ENABLE_SLOT` gives a slot ID.
- `address_device()` clears the input context, sets the slot's speed and root-port number, and sets the device address to the slot ID.
- It programmes EP0 as a control endpoint with the max packet size chosen from the speed (8/64/512), pointing at the EP0 ring with DCS set.
- Control transfers on EP0 go through `control_xfer()`, which builds a setup TRB, an optional data TRB (direction from bit 7 of the request) and a status TRB that runs opposite to the data stage.
- The driver reads the 18-byte device descriptor, then the full configuration descriptor.
- It walks the variable-length records looking for a HID-class interface (class 3).
- It then locates the interrupt IN endpoint (address bit 7 set, attributes `0b11`).
- It records the endpoint interval and max packet size, issues `SET_CONFIGURATION`, and sends HID `SET_PROTOCOL(boot)`.
- It calls `configure_endpoint()` for DCI 3 (`EP_TYPE_INT_IN`, interval and max packet from the descriptor, DCS set, pointing at the interrupt ring).
- The first port that yields a HID pointer wins.

### Report polling

- `usb_hid_init()` spawns the `usbmouse` thread.
- Each iteration enqueues a normal TRB on the interrupt ring (`TRB_NORMAL` with the interrupt-on-completion bit), rings the doorbell for the slot and DCI 3, and waits for the matching transfer event. It restarts on a short/zero completion.
- On success it reads the report in `int_buf`: byte 1 and byte 2 are the signed X and Y deltas and byte 0 is the button mask.
- USB HID reports Y with the same sign as the screen, so the code passes the deltas to `gfx_cursor_move()` unchanged. It logs the first eight reports.
- The bring-up runs in the `usbinit` thread created by `usb_hid_start()`, so its resets and control-transfer timeouts cannot block the boot. It parks once done.

## Notes

- The cursor is its own plane. It composites over whatever the console draws and does not disturb the framebuffer.
- Most Skylake laptops expose the touchpad and any USB mouse through the xHCI controller. The USB path matters there.
- The PS/2 path only sees a device that the EC still presents as PS/2.
- The USB path is polled (no xHCI interrupt handling yet). Buttons and the wheel are ignored for now.
- The xHCI enumeration assumes 32-byte device contexts and does not handle hubs, streams or USB 3.1 Link PM.

## Testing

- `make -C kernel clean && make -C kernel` builds with no warnings and the full image builds.
- QEMU with `-device qemu-xhci -device usb-mouse`: the controller is brought up and the mouse enumerates (`idVendor 0627 idProduct 0001`). Its HID interrupt IN endpoint is configured, and injected mouse moves are decoded (`pointer 40,25`, `pointer -30,-20`). The boot still reaches `name "hansh"` with no panic.
- QEMU without an xHCI controller: the driver reports no controller and the boot is unchanged.
- Target Skylake/HD520 machine: the arrow appears at the centre of the panel, walks the corners during the self test, then follows the touchpad or USB mouse.
