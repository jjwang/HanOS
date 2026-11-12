# 2026-11-11: Move the GPU driver to userspace

Move the Intel display driver out of the kernel. The kernel keeps the Limine framebuffer for the boot splash and grants the display BARs to a new server.

## Kernel

- `pci_get_gfx_device` selection stays in `kernel/srv/gpu_srv.c`; the file also maps GTTMMADR and GMADR into `/bin/gpu`, passes the firmware mode and registers `SVC_GPU`.
- Keep a minimal kernel eDP bootstrap in `boot_display.c`: it switches the panel before the HNK splash, then the userspace server takes over the runtime path.
- Delete the kernel `gfx.c`, VBT and EDID sources, the desktop, tests and cursor code. The bootstrap has no compositor and no runtime planes.
- Share `gpu_reg.h` and `gpu_types.h` between the kernel and userspace instead of keeping two register/type copies.
- `input_srv` and `xhci` forward pointer deltas to the GPU endpoint with `GPU_CURSOR_MOVE`.
- `console_srv` maps the firmware framebuffer as the fallback and passes the GPU endpoint to `/bin/console`.

## Userspace

- Add `/bin/gpu`: GTT entry writer, memory-object allocator, rain desktop and hardware cursor. The server skips its own eDP mode set when `bootinfo.gpu_display_ready` says the kernel bootstrap already switched the panel.
- The userspace source carries no mode-set or EDID parser; the cursor is the only GPU programming path it owns besides planes and desktop.
- The GPU server prepares the rain texture and console sprite before touching the planes, then answers `GPU_CONSOLE_SURFACE` with a memory object. The reply carries width, height, stride and the desktop flag.
- `console` asks for that surface at startup and maps it write-combining at `0x21000000`. It falls back to the firmware framebuffer when the request fails.
- Add `PROT_WC` to the memory-object map path. The display engine reads DRAM, so the surface mapping must bypass the cache.

## Tests

- Kernel and userspace build without warnings.
- Base and ports ISO boot to `hansh` and `bash`.
- QEMU has no supported Intel display device, so the log shows the fallback path and the console starts on the firmware framebuffer.
