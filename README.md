# HanOS - Microkernel-based General Purpose Operating System

[English](https://github.com/jjwang/HanOS/blob/mainline/README.md) | [中文](https://github.com/jjwang/HanOS/blob/mainline/README.zh-cn.md)

![](https://img.shields.io/github/license/jjwang/HanOS)
![](https://raw.githubusercontent.com/jjwang/HanOS/image-data/badge.svg)
[![Codacy
Badge](https://app.codacy.com/project/badge/Grade/eb7d6f1d9d1741e1ad3c40889c3fb1b2)](https://app.codacy.com/gh/jjwang/HanOS/dashboard?utm_source=gh&utm_medium=referral&utm_content=&utm_campaign=Badge_grade)

HanOS is an operating system for x86-64, written from scratch in C. It's a
hobby project that started as a classic monolith and is gradually turning into
a hybrid microkernel: the filesystem, pipes, terminal and block driver already
run as userspace servers and talk to each other over a small IPC layer.

So far it has:

- Limine boot into long mode, SMP, an APIC-timer scheduler, HPET and RTC.
- A framebuffer terminal (drawn by a userspace console server) and a kernel log.
- Virtual memory (buddy/slab/bitmap allocators) and shareable memory objects.
- PS/2 keyboard and mouse, plus USB HID pointer support for the hardware cursor.
- A VFS with an initrd-backed ramfs and a read-only FAT32 driver (the FAT32
  server reads the disk through a userspace block server).
- A userspace shell with the usual small commands: ls, cat, wc, pwd, rm, echo.

## Building

You'll need an `x86_64-elf` cross toolchain, `nasm` and `xorriso`. Clone Limine
first, then build and run:

```
make limine
make            # builds cdrom.iso
make run        # boots it in QEMU
make run-uefi   # boots under UEFI (downloads OVMF first)
```

`make run-hdd` builds a FAT-formatted disk image and boots from it instead
(needs `mtools` and `sgdisk`).

## Layout

```
kernel/     the kernel itself (arch/x64, mm, proc, fs, ipc, router, srv)
userspace/  the programs: servers/, bin/, test/
libc/       a small C library shared by the kernel and userspace
```

## Documentation

`doxygen Doxyfile` generates the HTML docs, and a GitHub Action publishes them
to GitHub Pages on every push to `mainline`.
