# HanOS - 基于微内核的通用操作系统

[English](https://github.com/jjwang/HanOS/blob/mainline/README.md) | [中文](https://github.com/jjwang/HanOS/blob/mainline/README.zh-cn.md)

![](https://img.shields.io/github/license/jjwang/HanOS)
![](https://raw.githubusercontent.com/jjwang/HanOS/image-data/badge.svg)
[![Codacy Badge](https://app.codacy.com/project/badge/Grade/eb7d6f1d9d1741e1ad3c40889c3fb1b2)](https://app.codacy.com/gh/jjwang/HanOS/dashboard?utm_source=gh&utm_medium=referral&utm_content=&utm_campaign=Badge_grade)

HanOS 面向 x86-64，用 C 编写。文件系统、管道、终端运行在用户态。块设备驱动运行在用户态。服务进程通过 IPC 层通信。

当前功能：

- 用 Limine 引导进入长模式。支持 SMP、APIC 定时器调度、HPET、RTC。
- 用户态 console 服务绘制帧缓冲终端。内核写日志。
- 虚拟内存用 buddy、slab、bitmap 分配器。支持可共享内存对象。
- 读取 PS/2 键盘和鼠标。USB HID 指针驱动硬件光标。
- VFS 挂载 initrd ramfs 和只读 FAT32 驱动。FAT32 服务经用户态块服务读盘。
- 用户态 shell 提供 `ls`、`cat`、`wc`、`pwd`、`rm`、`echo`。
- 用户态链接 musl。内核保留一个精简的 freestanding C 库。

## 构建

安装 `x86_64-elf` 交叉工具链、`nasm`、`xorriso`。先克隆 Limine，再构建并运行：

```
make limine
make            # 生成 cdrom.iso
make run        # 用 QEMU 启动
make run-uefi   # 以 UEFI 方式启动（先下载 OVMF）
```

`make run-hdd` 生成 FAT 格式磁盘镜像，并从它启动。需要 `mtools` 和 `sgdisk`。

## 目录结构

```
kernel/     内核（arch/x64、mm、proc、fs、ipc、router、srv）与其 C 库（libc/）
userspace/  用户态程序（servers/、bin/、test/）与 HanOS 运行时
include/    内核与用户态共享的线协议头
musl/       musl 构建与 HanOS 移植层
```

musl 的构建说明见 `musl/README.md`。

## 文档

`doxygen Doxyfile` 生成 HTML 文档。GitHub Action 在推送到 `mainline` 时发布到 GitHub Pages。
