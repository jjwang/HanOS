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
- Mount a VFS over an initrd ramfs and a read-only FAT32 driver. The FAT32 server reads the disk through a userspace block server.
- Run a userspace shell with `ls`, `cat`, `wc`, `pwd`, `rm`, and `echo`.

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
kernel/     the kernel (arch/x64, mm, proc, fs, ipc, router, srv)
userspace/  the programs: servers/, bin/, test/
libc/       a small C library shared by the kernel and userspace
```

## Documentation

`doxygen Doxyfile` generates the HTML docs. A GitHub Action publishes them to GitHub Pages on every push to `mainline`.
