# 2026-11-01: Skylake GPU rain desktop

This note describes the display path on Intel Skylake (Gen9). The desktop uses three planes: the primary plane scans a baked rain texture; the sprite plane holds the translucent terminal; the cursor plane sits on top. The kernel bakes the texture once. Each frame, the CPU writes the plane offset and surface address once; the pipe frame counter drives the beat. Without Gen9 the path falls back to the primary console.

## 1. Result

- Mode: native eDP (embedded DisplayPort) mode. The reference machine runs 1366×768.
- Primary: 1366×3850 texture, 3080 px period.
- Sprite: 1088×620 terminal window at 139,74.
- Status bar: 20 px. It shows the version, time and `rain:gpu`.
- Terminal background: 80% opaque. Text and cursor stay opaque.
- Rain speed: 8 px per frame, 480 px/s.
- Fallback: keep the primary console when the GPU is absent or not Gen9.

## 2. Display path

`kernel/device/display/skl_display.c` configures the eDP path: pipe A, EDP transcoder, DDI-A (Digital Display Interface A). It keeps the firmware-trained link and PLL (phase-locked loop), rewrites the timings, then enables plane 1. The desktop uses the geometry and primary plane from that mode.

The display engine composes enabled planes per pipe. Gen9 universal plane registers for pipe A:

| Plane | Control | Stride | Position | Size | Surface | Offset |
|------|------|------|------|------|------|------|
| 1 A | 0x70180 | 0x70188 | 0x7018C | 0x70190 | 0x7019C | 0x701A4 |
| 2 A | 0x70280 | 0x70288 | 0x7028C | 0x70290 | 0x7029C | 0x702A4 |

The cursor uses `CURACNTR`, `CURAPOS`, and `CURABASE`. The cursor is a 64×64 ARGB surface.

Plane control fields:

- bit 31: enable.
- bit 27:24: format. Value 4 is 32bpp XRGB/BGRX.
- bit 5:4: alpha mode. Value 2 is SW premultiply.
- bit 12:10: tiling. Value 0 is linear.

Gen9 expresses the stride in 64-byte units. The surface field holds a 32-bit GPU address. The low 16 bits of the offset field hold X; the high 16 bits hold Y. A surface write assembles the double-buffered plane state. The next vblank applies it.

## 3. Display data buffers

SKL gives each pipe 512 display data buffer (DDB) blocks. Each block holds 512 bytes. The desktop allocates:

| Plane | DDB blocks |
|------|--------|
| primary | 0..445 |
| cursor | 446..477 |
| console sprite | 478..511 |

Constraints:

- The primary plane needs at least the 445 blocks from mode set. A 1366-wide scan-out underruns below that. The pipe and vblank stop. Rain stops on the first frame.
- The cursor occupies 446..477. The sprite starts at 478. A sprite start at 446 steals the cursor DDB blocks.
- The sprite owns 34 blocks. Watermark 16 blocks. A window line spans 4352 bytes, or 8.5 blocks. Watermark plus line stays within 34 blocks.
- Primary watermark 32 blocks. Cursor watermark 32 blocks.

An underrun shows as corrupt pixels, a black right edge, or a black bottom half. The log prints `GFX: Timeout waiting for vblank on pipe 0`. The vblank status bit stays clear.

## 4. Rain texture

The texture width equals the screen width. The height equals `(period_rows + screen_rows) × font_h`. The font is the kernel PSF1 (PC Screen Font); the reference machine uses font height 14.

Reference parameters:

| Item | Value |
|----|----|
| Screen rows | 55 |
| Period rows | 4 × 55 = 220 |
| Texture height | (220 + 55) × 14 = 3850 |
| Period pixels | 220 × 14 = 3080 |
| Texture bytes | 1366 × 3850 × 4, 21 MB |

Column structure: each column gets a random start. The column repeats streaks. A streak spans 3..16 rows. The gap spans 10..59 rows. Random gaps break the image. The head sits on the higher row; the tail sits above the head. The head is bright; the tail fades with distance. Characters come from the `0/1/2/.../#$%&` set. A hash of the column seed and row number selects the character.

The period end copies the first screen. The copy covers `period_rows..period_rows+screen_rows`. The plane offset wraps over 0..3080. The wrap points carry the same content, so the seam does not jump.

Scroll direction: the offset decreases. The texture moves down. Each frame subtracts 8 px. The period lasts 6.4 s.

The bake runs once. After that, each frame the CPU reads the frame counter and writes the offset and surface. The display engine fetches, composites and scans out.

## 5. GTT (Graphics Translation Table) memory

Small buffers use `gfx_fb_alloc()` for contiguous physical pages. Reference size: the terminal sprite is 1088×620, 2.7 MB.

The rain texture is 21 MB. Contiguous physical pages are unreliable after boot. `gfx_fb_alloc_scatter()` allocates page by page:

- Call `pmm_get(1)` for each page.
- Map the physical page to a contiguous GPU address with a 64-bit GTT entry.
- Save the physical page list in `gfx_fb_t.pages`.
- The CPU writes through the direct map, page by page. `layer_pixel()` splits an offset by page.
- Read the first and last GTT entries after the write. The display engine then fetches the data.
- `gfx_fb_free()` recognizes `pages`, clears the GTT, and calls `pmm_free()` for each page.

The shared GTT window runs from 64 MB to 256 MB. Reference allocations:

| Object | GPU address |
|------|----------|
| console framebuffer | 0x04001000 |
| cursor | 0x04409000 |
| rain texture | 0x0440D000 |
| console sprite | 0x05843000 |

## 6. vblank and beat

The display interrupt uses the GPU PCI INTx (legacy interrupt line). The kernel reads the interrupt line from PCI config 0x3C. The reference machine uses line 11.

Interrupt path:

- `ioapic_route_gsi()` writes the I/O APIC redirection entry.
- `irq_route_vector()` maps the line to an explicit vector. The desktop uses `0x30`.
- The IDT (interrupt descriptor table) installs gates for PCI vectors 13..23. Vector `0x30` maps to the `irq16` stub.
- Order: mask the GSI. Register the handler. Route and unmask. Enable the display interrupt.

Gen8+ pipe vblank lives in per-pipe registers:

| Register | Address | vblank bit |
|--------|------|-----------|
| `GEN8_DE_PIPE_IIR(0)` | 0x44408 | bit 0 |
| `GEN8_DE_PIPE_IMR(0)` | 0x44404 | bit 0 (0 unmasks) |
| `GEN8_DE_PIPE_IER(0)` | 0x4440C | bit 0 |

IIR is the interrupt identity register. IMR is the mask register. IER is the enable register.

The path also sets bit 31 of `GEN8_MASTER_IRQ` (0x44200). The old `DEIER`/`DEIMR`/`DEIIR` path is Haswell-compatible. Its pipe A vblank uses bit 7. Ivy Bridge bit 0 is FIFO underrun. A wrong bit leaves `IIR` at 0.

The interrupt keeps pipe A vblank. It masks the GT (graphics), misc, and port sources. A shared INTx does not loop on other sources.

The desktop rain does not depend on the vblank status bit. The per-pipe vblank IIR may stay at 0 while the sprite is active. The rain thread reads pipe frame counter 0x70040:

```c
last = gfx_pipe_frame();
for (;;) {
    sched_sleep(1);
    now = gfx_pipe_frame();
    if (now == last)
        continue;
    last = now;
    scroll -= 8;
    if (scroll < 0)
        scroll = period;
    write(PLANE_OFFSET_1_A, scroll << 16);
    write(PLANE_SURF_1_A, texture);
}
```

`gfx_wait_vblank_frame()` remains for tests. It waits for the interrupt count. On timeout it disables the vblank interrupt and polls IIR. The poll timeout is 50 ms. The PIT (programmable interval timer) tick is 1 ms. The old code wrote 50000 counts, which meant 50 seconds.

## 7. Sprite and translucency

The sprite plane holds the terminal window. The window covers four fifths of the screen. The reference machine uses 1088×620 at 139,74. The top 20 px hold the status bar.

The console service draws the terminal into its own GTT surface. The kernel attaches that surface to plane 2:

- Control: enable, XRGB, SW premultiply, linear.
- Stride: sprite stride / 64.
- Position: `(y << 16) | x`.
- Size: `(h - 1) << 16 | (w - 1)`.
- DDB: 478..511. Watermark 16.

`PLANE_CTL_ALPHA_SW_PREMULTIPLY` requires premultiplied source pixels. The console `premul(rgb, a)` performs the step:

```c
r = ((rgb >> 16) & 0xff) * a / 255;
g = ((rgb >> 8) & 0xff) * a / 255;
b = (rgb & 0xff) * a / 255;
return (a << 24) | (r << 16) | (g << 8) | b;
```

Alpha values:

| Element | Alpha |
|------|-------|
| Terminal background | 0xCC (80%) |
| Status bar | 0xD0 (82%) |
| Text | 0xFF |
| Block cursor | 0xFF |
| Fallback console | 0xFF |

`render_cell()` handles reverse and conceal first, then applies alpha. The foreground stays opaque. The background uses `BG_ALPHA`. Reversed cells keep the text legible.

The sprite sits above the primary. The cursor sits above the sprite. Rain shows through the translucent background and status bar.

Without the desktop, the console uses the primary. The primary is XRGB. The alpha byte is ignored. The fallback stays opaque.

## 8. Console integration

Kernel `gfx_desktop_start()` owns the desktop:

1. Compute the terminal geometry. `cols = (screen_w / 8) * 4 / 5`, `rows = (screen_h / 15) * 4 / 5`.
2. Allocate the scatter rain texture. Bake it.
3. Allocate the terminal sprite. Size `cols × 8` and `rows × 15 + 20`.
4. Write the DDB split: primary 0..445, cursor 446..477, sprite 478..511.
5. Configure the primary: surface is the rain texture, offset is the period pixel.
6. Configure the sprite: surface is the terminal buffer, centered.
7. Start the `gfxrain` kernel thread.
8. Mark the desktop active.

`console_server_start()` detects the desktop. When the desktop is active, the spawn hook maps the sprite physical pages into the console address space. The mapping uses WC (write-combining) and USER. `bootinfo` fills:

- `fb_vaddr`: sprite kernel virtual address.
- `fb_width`, `fb_height`, `fb_pitch`: sprite geometry.
- `fb_size`: sprite bytes.
- `desktop`: 1.

The console reads `bootinfo.desktop`. Desktop branch:

- `cols = fb_w / 8`.
- `rows = (fb_h - 20) / 15`.
- `term_x = 0`, `term_y = 20`.
- Draw the status bar at the top. It shows the `uname` release, the `clock_gettime` time and `rain:gpu`.
- Feed the terminal area to libvterm. Render cells with alpha.
- 33 ms loop. Refresh the status bar every second. Blink the cursor every 500 ms.
- Refresh the terminal rectangle and rain margins separately.
- Fallback rain runs at 15 fps. Refresh the left, right, top and bottom strips. Redraw the rain head and tail rows.

Fallback branch keeps the old console:

- The grid covers four fifths of the screen.
- Margins hold the CPU rain.
- The background stays opaque.

The svc monitor restarts the console. The spawn hook remaps the same sprite after restart. Plane 2 stays unchanged. The rain thread continues.

Kernel and tty bytes still use `CONSOLE_WRITE_TAG`. The console feeds the bytes to vterm. The `conflush` thread forwards the kernel log ring.

## 9. Boot order

1. `gfx_init()`: eDP mode set. The primary points at the console framebuffer.
2. `gfx_vblank_irq_start()`: route the GPU interrupt line, register the handler, enable pipe A vblank.
3. `kshell` calls `gfx_desktop_start()`: allocate the texture and sprite, program the three planes, start the rain thread.
4. `console_server_start()`: map the sprite, start the console.
5. `/bin/init` starts the shell.

No Gen9 or no GPU:

- `gfx_desktop_start()` returns false.
- `console_server_start()` uses the primary.
- The console uses the old margin rain.
- `gfx layers`, `gfx scroll`, `gfx solid`, and `gfx flip` return `desktop is active` while the desktop runs.

QEMU lacks Gen9. The CI boot test uses the fallback path. The shell marker and kernel self-tests stay unchanged.

## 10. Interfaces

Kernel display interface (`kernel/device/display/gfx.h`):

| Interface | Role |
|------|------|
| `gfx_desktop_start()` | Build the persistent rain desktop |
| `gfx_desktop_active()` | Report desktop state |
| `gfx_desktop_console_fb()` | Return the console sprite surface |
| `gfx_fb_alloc()` / `gfx_fb_free()` | Contiguous GTT surface |
| `gfx_fb_alloc_scatter()` | Scatter-page rain texture |
| `gfx_flip_to()` | Write plane 1 surface |
| `gfx_wait_vblank_frame()` | Wait for one frame |
| `gfx_pipe_frame()` | Read the pipe A frame counter |
| `gfx_status` command | Print vblank, IIR, IER, IMR, master and frame count |

The `desktop` field in `bootinfo` tells the console to use the desktop or the fallback.

Console constants (`userspace/servers/console.c`):

| Constant | Role |
|------|------|
| `STATUS_H` | Status bar height, 20 px |
| `BG_ALPHA` | Terminal background alpha, 0xCC |
| `STATUS_ALPHA` | Status bar alpha, 0xD0 |

Shell commands:

- `gfx status`: read the display state.
- `gfx flip`, `gfx layers`, `gfx scroll`, `gfx solid`: debug cases. They return `desktop is active` while the desktop runs.

## 11. Limits

- Rain is a baked texture scroll. Every column moves at one speed. No live random generation.
- The texture period spans four screens. It repeats every 6.4 s at 1366×768.
- One sprite plane. The terminal window covers four fifths of the screen.
- The sprite DDB holds 34 blocks. A wider window raises the line width and the underrun risk.
- The primary DDB stays at 445 blocks. Shrinking it stops the pipe and vblank.
- The per-pipe vblank IIR may stay clear while the sprite is active. The desktop rain uses the frame counter.
- The vblank interrupt remains for `gfx status` and debug cases.
- No GPU render engine. Rain comes from the CPU, not a shader.
- Translucency uses SW premultiply. Terminal background 80%. Status bar 82%. Fallback opaque.
- The status bar version and time come from the misc service. It shows `00:00:00` before misc starts.
- Rain texture 21 MB. 1920×1080 uses 42 MB.
- Programs that use the whole sprite run inside it. Rain still shows through the translucent background.
- Fallback rain refreshes at 15 fps. Margins and the terminal rectangle refresh separately. The redraw covers the head and tail rows. Cursor blinks every 500 ms.

## 12. Verification

QEMU:

- `make` produces `cdrom.iso` and `release/hdd.img`.
- `scripts/ci-boot-test.sh` uses the fallback path.
- Check for `name "hansh"`, the four `MK:` markers, and no `panic`, `exception`, or `corrupt` lines.

Skylake:

- Boot shows full-screen rain, a centered translucent terminal, the top status bar and the hardware cursor.
- The shell runs `ls`, `dmesg`, and `crashtest`.
- Games run inside the terminal. Rain shows through the background.
- `dmesg GFX` shows:

```text
GFX: desktop: rain 1366x768 period 3080, console 1088x620 at 139,74
GFX: desktop: rain thread running
GFX: desktop: rain offset ...
console: attached fb ... 1088x620 ... sprite
```

- `gfx status` reads vblank, IIR, IER, IMR, master and the frame count.
- Rain speed, density and transparency are tunable:
  - `LAYER_SCROLL_SPEED`: 4, 8, 12 map to 240, 480, 720 px/s.
  - Texture streak gap: density.
  - `BG_ALPHA`, `STATUS_ALPHA`: transparency.

## 13. Files

- `kernel/device/display/gfx.c`, `gfx.h`, `gfx_reg.h`: display driver, planes, GTT, desktop.
- `kernel/device/display/skl_display.c`: eDP mode set, cursor.
- `kernel/device/display/fb.c`, `fb.h`: boot framebuffer and font.
- `kernel/srv/console_srv.c`: sprite mapping, console start.
- `kernel/kmain.c`: boot order.
- `kernel/proc/syscall.c`: `gfx` commands and limits.
- `userspace/servers/console.c`: status bar, terminal, alpha, fallback.
- `include/bootinfo.h`: `desktop` field.
- `kernel/arch/x64/{isr_base.s,idt.c,ioapic.c}`: PCI interrupt vectors and I/O APIC.
