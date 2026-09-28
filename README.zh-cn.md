# HanOS - 基于微内核的通用操作系统

[English](https://github.com/jjwang/HanOS/blob/mainline/README.md) | [中文](https://github.com/jjwang/HanOS/blob/mainline/README.zh-cn.md)

![](https://img.shields.io/github/license/jjwang/HanOS)
![](https://raw.githubusercontent.com/jjwang/HanOS/image-data/badge.svg)
[![Codacy
Badge](https://app.codacy.com/project/badge/Grade/eb7d6f1d9d1741e1ad3c40889c3fb1b2)](https://app.codacy.com/gh/jjwang/HanOS/dashboard?utm_source=gh&utm_medium=referral&utm_content=&utm_campaign=Badge_grade)

HanOS 是一个用 C 从零写起的 x86-64 操作系统。它是个个人项目，最初是个经典的单体内核，现在正一步步往混合微内核方向改：文件系统、管道、终端、块设备驱动都已经跑在用户态的服务进程里，靠一个自己写的小 IPC 层互相通信。

目前已经能用的东西：

- 用 Limine 引导进入长模式，支持 SMP、APIC 定时器调度、HPET 和 RTC。
- 帧缓冲终端（由用户态的 console 服务绘制）和内核日志。
- 虚拟内存（buddy/slab/bitmap 分配器）和可共享的内存对象。
- PS/2 键盘、鼠标，以及 USB HID 指针（用来画硬件光标）。
- VFS，带一个 initrd 支撑的 ramfs 和一个只读 FAT32 驱动（FAT32 服务通过用户态的块服务读盘）。
- 用户态 shell，带 ls、cat、wc、pwd、rm、echo 这些常用小命令。

## 构建

需要 `x86_64-elf` 交叉工具链、`nasm` 和 `xorriso`。先拉一下 Limine，再构建和运行：

```
make limine
make            # 生成 cdrom.iso
make run        # 用 QEMU 启动
make run-uefi   # UEFI 方式启动（会先下载 OVMF）
```

`make run-hdd` 会生成一张 FAT 格式的磁盘镜像并从它启动（需要 `mtools` 和 `sgdisk`）。

## 目录结构

```
kernel/     内核本身（arch/x64、mm、proc、fs、ipc、router、srv）
userspace/  用户态程序：servers/、bin/、test/
libc/       一个内核和用户态共用的小 C 库
```

## 文档

`doxygen Doxyfile` 生成 HTML 文档，推送时会由 GitHub Action 自动发布到 GitHub Pages。
