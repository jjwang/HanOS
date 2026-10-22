# 2026-10-22: Native eDP on Haswell, Broadwell and Skylake

## Result

Haswell-ULT, Broadwell-U (i5-5200U) and Skylake laptops boot at the panel's native mode. The console, games and hardware cursor work. The kernel selects the mode from Limine EDID, OpRegion EDID, OpRegion VBT, eDP AUX EDID, then the firmware's eDP transcoder timing.

## Symptom

- Haswell reported `EDID size: 0` and stayed at 800x600.
- `dmesg edid` reported no usable source.
- A first native attempt flashed the panel, then showed a garbled image.
- Skylake regressed to 800x600 during the Haswell work.

## Root causes

### No EDID on Haswell

- Limine forwarded no EDID.
- OpRegion ASLE mailbox #5 reported no panel EDID.
- The eDP AUX DDC returned I2C NACK at address 0x50.
- PCH SMBus returned no EDID.

### The firmware keeps the native timing in the eDP transcoder

The GOP scales the 800x600 pipe up to the panel. The pipe source stays 800x600. The eDP transcoder keeps the native timing. Reading the transcoder timing gives the panel mode without EDID.

### Haswell plane registers differ from Gen9

- `DSPSTRIDE` is in bytes on Gen7.5. Gen9 `PLANE_STRIDE` is in 64-byte units.
- The primary plane format field is bits 29:26 on Gen7.5. Gen9 uses bits 27:24.

The first native mode set used 64-byte stride units and a truncated format field. The plane fetched 85-byte rows and a 16bpp format. The panel showed garbage.

### AUX clock fields differ by generation

- Gen7.5 needs `PRECHARGE_2US(3)` and `BIT_CLOCK_2X(cdclk/2000)`.
- Gen9 derives the clock from CDCLK and keeps the firmware sync fields.

The first fix wrote the Gen7.5 fields on both platforms. Skylake AUX broke and its mode set failed.

### Cursor registers

- `CURABASE` is 0x70084. `CURAPOS` is 0x70088.
- The two registers were swapped during the cursor work. The cursor disappeared.
- `MCURSOR_GAMMA_ENABLE` without a gamma LUT gave a red tint. The cursor runs without it.

### EDID selection

The selector read only DTD0. It now prefers the DTD marked by the EDID preferred-timing bit. Without that bit it scans all four DTDs and takes the highest-resolution non-interlaced timing. This follows `howto.txt` and i915.

## Dead ends

These paths returned no panel timing on the Haswell laptop:

- OpRegion VBT at 0x400. the firmware writes `$VBT`, but the BDB is a stub whose blocks start at 4.
- VGA shadow VBT at 0xc0b60. Same stub.
- PCI ROM. The ROM mapped and scanned, but held no `$VBT`.
- GPU SPI and PCH SPI. The PCH FREG registers read 0x7fff7fff for every region.
- Cached EDID scan in low memory and the top ROM.
- PCH SMBus byte and block reads at 0x50.

Remove the code for these paths. The VBT module keeps only the OpRegion and ASLE.RVDA path that i915 uses for integrated graphics.

## Fixes

### Mode selection

- Add `kernel/device/display/edid.c`, `edid_read.h` and `vbt.c`, `vbt.h`.
- Resolve the mode in i915 order: Limine EDID, OpRegion EDID, OpRegion VBT, AUX EDID, eDP timing.
- Power the panel VDD before an AUX read, then write `DPCD 0x600 = D0`.
- Read the native timing from the eDP transcoder as the last fallback.

### Platform branches

- Add `gfx_generation()` for Gen7.5, Gen8 and Gen9 by device ID.
- List Haswell, Broadwell and Skylake GT device IDs.
- Use 2 MB GTT with 32-bit entries on Gen7.5. Use 8 MB with 64-bit entries on Gen8 and Gen9.
- Keep the Gen9 AUX sync fields. Program the Gen7.5 bit clock and precharge.
- Use bytes for the Gen7.5 plane stride. Use 64-byte units on Gen9.
- Keep the Gen7.5 plane format field at bits 29:26.
- Skip the Gen9 cursor watermark and data-buffer registers on Gen7.5.
- Restore `CURABASE` and `CURAPOS`.

## Files Changed

| File | Change |
|------|--------|
| `kernel/device/display/gfx.c` | generation table, GTT sizing, mode source order, eDP timing |
| `kernel/device/display/edid.c`, `edid_read.h` | EDID validation, OpRegion and AUX readers |
| `kernel/device/display/vbt.c`, `vbt.h` | OpRegion/RVDA VBT panel timing |
| `kernel/device/display/display_mode.c`, `.h` | multi-DTD EDID selection |
| `kernel/device/display/skl_display.c`, `.h` | Gen9/Gen7.5 branches, AUX control, cursor |
| `kernel/device/display/gfx_reg.h` | Gen7.5 clocks, AUX fields, plane stride note |
| `kernel/device/display/gfx.h` | platform fields |
| `kernel/kmain.c` | accept extended EDID size |
| `attic/2026/2026-10-22-gen75-edp-and-edid-en.md`, `-zh.md` | this note |

## Testing

- Kernel and userspace build with no `-Wall -Wextra` warnings.
- QEMU reaches `hansh` with no panic, exception or corrupt line.
- Haswell-ULT: native panel mode, terminal and games; hardware cursor visible.
- Broadwell-U (i5-5200U): native panel mode after the same Gen7.5 path.
- Skylake: native panel mode from EDID; cursor visible.
- `dmesg edid` reports the selected source as one line.

## TODO

- EDID-less panels: the eDP timing depends on the GOP mode. Add a VBIOS/VBT body reader for firmware that never scales the pipe.
- Standard timings: add the EDID standard-timing fallback from `howto.txt`.
- Split the Gen7.5 modeset into `hsw_display.c` when the shared file grows further.
- Add the remaining Skylake GT IDs after hardware tests.
- Cursor: verify the ARGB channel order on Gen7.5.
