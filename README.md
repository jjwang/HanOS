# HanOS - Microkernel-based General Purpose Operating System

[English](https://github.com/jjwang/HanOS/blob/mainline/README.md) | [中文](https://github.com/jjwang/HanOS/blob/mainline/README.zh-cn.md)

![](https://img.shields.io/github/license/jjwang/HanOS)
![](https://raw.githubusercontent.com/jjwang/HanOS/image-data/badge.svg)
[![Codacy Badge](https://app.codacy.com/project/badge/Grade/eb7d6f1d9d1741e1ad3c40889c3fb1b2)](https://app.codacy.com/gh/jjwang/HanOS/dashboard?utm_source=gh&utm_medium=referral&utm_content=&utm_campaign=Badge_grade)

HanOS targets x86-64 and is written in C. The filesystem, pipes, terminal, and block driver run as userspace servers. The servers communicate over a small IPC layer.

Current features:

- Boot Limine into long mode; run SMP, an APIC-timer scheduler, HPET, and RTC.
- Draw a framebuffer terminal from a userspace console server; log to the kernel.
- Manage virtual memory with buddy, slab, and bitmap allocators; share memory objects.
- Read a PS/2 keyboard and mouse; drive the hardware cursor from a USB HID pointer.
- Drive USB mass storage over xHCI and EHCI, and route Lynx Point USB2 ports to xHCI.
- Mount a VFS over an initrd ramfs, a read-only FAT32 server and an ext2 server on the data partition, and read AHCI and USB storage through the block server.
- Run a userspace shell with `ls`, `cat`, `wc`, `pwd`, `rm`, `echo`, `edit`, `dmesg`, `usb`, and `lspci`.
- Run the terminal games `tetris`, `2048`, and `snake` from the shell.
- Link userspace against musl; keep the kernel on a small freestanding C library.

## Building

Install an `x86_64-elf` cross toolchain, `nasm`, and `xorriso`. Clone Limine, then build and run:

```
make limine
make            # builds cdrom.iso
make run        # boots it in QEMU
make run-uefi   # boots under UEFI (downloads OVMF first)
```

`make run-hdd` builds a FAT-formatted disk image and boots from it. It needs `mtools` and `sgdisk`.

## Layout

```
kernel/     the kernel (arch/x64, mm, proc, fs, ipc, router, srv) and its C library (lib/)
userspace/  the programs (servers/, bin/, test/) and the HanOS runtime
include/    wire headers shared by the kernel and userspace
3rdparty/   HanOS port files for musl and libvterm; build/ holds downloads and output
```

See `3rdparty/musl/README.md` for the musl build.

## Games

The ext2 data partition carries three terminal games. The shell looks in `/bin` and then `/data/bin`, so the names run from any directory. The live USB image and `make run-hdd` use the same layout.

- `tetris`: stack the falling pieces and clear lines.
- `2048`: merge equal tiles and reach 2048.
- `snake`: eat the `$` food, grow, and avoid the walls and your tail.

| Game | Keys |
|------|------|
| `tetris` | A/D or arrows move; W/Up rotate; S/Down soft drop; Space hard drop; P pause; Q quit |
| `2048` | W/A/S/D or arrows move; R restart; Q quit |
| `snake` | W/A/S/D or arrows turn; P pause; Q quit |

## Hardware

Tested physical machines:

- Haswell-ULT (i5-4300U, Lynx Point-LP): the kernel routes the USB2 ports from EHCI to xHCI, the live U disk enumerates, the ext2 partition mounts, and the games run.
- Skylake (i5-6200U, Sunrise Point-LP): xHCI storage enumerates, the internal AHCI disk reports its model, the ext2 partition mounts, and the games run.

QEMU covers BIOS and UEFI boot, AHCI disks, and EHCI/xHCI USB storage.

Known limits:

- USB hotplug is not supported.
- Plug the U disk in before boot.
- The Lynx Point USB2 route is PCH-specific. Other Intel generations need a quirk.
- A full-speed device on EHCI without xHCI routing is not validated.

See `attic/2026/2026-10-18-haswell-usb2-ports-en.md` for the USB work.

## Documentation

`doxygen Doxyfile` generates the HTML docs. A GitHub Action publishes them to GitHub Pages on every push to `mainline`.
