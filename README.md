# HanOS - Microkernel-based General Purpose Operating System

[English](https://github.com/jjwang/HanOS/blob/mainline/README.md) | [中文](https://github.com/jjwang/HanOS/blob/mainline/README.zh-cn.md)

![](https://img.shields.io/github/license/jjwang/HanOS)
![](https://raw.githubusercontent.com/jjwang/HanOS/image-data/badge.svg)
![](https://raw.githubusercontent.com/jjwang/HanOS/image-data/kernel-badge.svg)
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

A local build needs an `x86_64-elf` cross toolchain plus:

- `nasm`, `xorriso`, `perl`, `curl`, `git`, `build-essential`
- `mtools`, `gdisk` (`sgdisk`), `e2fsprogs` for `release/hdd.img`
- `qemu-system-x86` for `make run`

Clone Limine, then build and run:

```
make limine
make            # builds cdrom.iso and release/hdd.img
make run        # boots cdrom.iso in QEMU (KVM)
make run-uefi   # boots under UEFI (downloads OVMF first)
make run-hdd    # boots release/hdd.img
```

`make run-hdd` boots the disk image. It needs `mtools` and `sgdisk`.

The CI does not install a cross toolchain. It defines `x86_64-elf-gcc` as `gcc -fno-pie` on Ubuntu 24.04 and builds with the host compiler.

## Layout

```
kernel/     the kernel (arch/x64, mm, proc, fs, ipc, router, srv) and its C library (lib/)
userspace/  the programs (servers/, bin/, test/) and the HanOS runtime
include/    wire headers shared by the kernel and userspace
3rdparty/   HanOS port files for musl and libvterm; build/ holds downloads and output
```

See `3rdparty/musl/README.md` for the musl build.

## Programs

Programs live in the initrd and on the ext2 data partition. The shell looks in `/bin` and then `/data/bin`, so a name runs from any directory. The live USB image and `make run-hdd` use the same layout.

### Games

- `tetris`: stack the falling pieces and clear lines.
- `2048`: merge equal tiles and reach 2048.
- `snake`: eat the `$` food, grow, and avoid the walls and your tail.

| Game | Keys |
|------|------|
| `tetris` | A/D or arrows move; W/Up rotate; S/Down soft drop; Space hard drop; P pause; Q quit |
| `2048` | W/A/S/D or arrows move; R restart; Q quit |
| `snake` | W/A/S/D or arrows turn; P pause; Q quit |

### Tools

- `ls`, `cat`, `wc`, `pwd`, `rm`, `echo`, `edit`
- `dmesg [keyword...]`, `usb`, `lspci`
- `help`, `hansh`

## Hardware

Tested physical machines:

- Haswell-ULT (i5-4300U, Lynx Point-LP): the kernel routes the USB2 ports from EHCI to xHCI, the live U disk enumerates, the ext2 partition mounts, the panel runs at its native eDP mode, and the games run.
- Broadwell-U (i5-5200U, Wildcat Point-LP): the same USB2 route and native eDP path work; the ext2 partition mounts, and the games run.
- Skylake (i5-6200U, Sunrise Point-LP): xHCI storage enumerates, native eDP comes from EDID, and the games run.

QEMU covers BIOS and UEFI boot, AHCI disks, and EHCI/xHCI USB storage.

Known limits:

- USB hotplug is not supported.
- Plug the U disk in before boot.
- The USB2 route covers Lynx Point and Wildcat Point. Other Intel PCH generations need a quirk.
- A full-speed device on EHCI without xHCI routing is not validated.
- A panel without EDID uses the firmware eDP timing. Native mode stays unavailable when the firmware scans at 800x600.
- Other Intel GPU and PCH IDs need a table entry.

See `attic/2026/2026-10-22-gen75-edp-and-edid-en.md` for the graphics work. See `attic/2026/2026-10-18-haswell-usb2-ports-en.md` for the USB work.

## CI/CD

Three GitHub Actions workflows live in `.github/workflows/`:

- `build.yml` runs on every push and pull request on `ubuntu-24.04`. It installs the build tools and QEMU, defines `x86_64-elf-gcc` as `gcc -fno-pie`, builds the kernel, userspace, `cdrom.iso` and `release/hdd.img`, then boots the ISO under QEMU 8.2 with TCG. `scripts/ci-boot-test.sh` waits for the shell and the boot markers `name "hansh"`, `MK: selftest PASS`, `MK: misc PASS` and `MK: signalfd PASS`. A missing marker or a `panic`, `exception` or `corrupt` line fails the job. A passing run prints the serial log.
- `doxygen-pages.yml` runs on a push to `mainline`, generates the Doxygen HTML and deploys it to [GitHub Pages](https://jjwang.github.io/HanOS/).
- `main.yml` regenerates the line-count badge on the `image-data` branch.

Hosted runners have no `/dev/kvm`, so the boot test uses TCG. Local `make run` uses KVM.

## Documentation

`doxygen Doxyfile` generates the HTML docs.
