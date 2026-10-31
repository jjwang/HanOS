# 汉操作系统 (HanOS) - 基于微内核的通用操作系统

[English](https://github.com/jjwang/HanOS/blob/mainline/README.md) | [中文](https://github.com/jjwang/HanOS/blob/mainline/README.zh-cn.md)

![](https://img.shields.io/github/license/jjwang/HanOS)
![](https://raw.githubusercontent.com/jjwang/HanOS/image-data/badge.svg)
![](https://raw.githubusercontent.com/jjwang/HanOS/image-data/kernel-badge.svg)
[![Codacy Badge](https://app.codacy.com/project/badge/Grade/eb7d6f1d9d1741e1ad3c40889c3fb1b2)](https://app.codacy.com/gh/jjwang/HanOS/dashboard?utm_source=gh&utm_medium=referral&utm_content=&utm_campaign=Badge_grade)

汉操作系统 (HanOS) 面向 x86-64，用 C 编写。文件系统、管道、终端运行在用户态。块设备驱动运行在用户态。服务进程通过 IPC 层通信。

当前功能：

- 用 Limine 引导进入长模式。支持 SMP、APIC 定时器调度、HPET、RTC。
- 用户态 console 服务绘制帧缓冲终端。内核写日志。
- 虚拟内存用 buddy、slab、bitmap 分配器。支持可共享内存对象。
- 读取 PS/2 键盘和鼠标。USB HID 指针驱动硬件光标。
- 经 xHCI 与 EHCI 读 USB 存储，Lynx Point 的 USB2 端口路由到 xHCI。
- VFS 挂载 initrd ramfs、只读 FAT32 与数据分区上的 ext2，块服务读 AHCI 与 USB 存储。
- 用户态 shell 提供 `ls`、`cat`、`wc`、`pwd`、`rm`、`echo`、`edit`、`dmesg`、`usb`、`lspci`。
- shell 可运行终端游戏 `tetris`、`2048`、`snake`。
- 用户态链接 musl，内核保留一个精简的 freestanding C 库。

## 构建

本机构建需要 `x86_64-elf` 交叉工具链，以及：

- `nasm`、`xorriso`、`perl`、`curl`、`git`、`build-essential`
- `mtools`、`gdisk`（`sgdisk`）、`e2fsprogs`，用于 `release/hdd.img`
- `qemu-system-x86`，用于 `make run`

先克隆 Limine。然后构建、运行：

```
make limine
make            # 生成 cdrom.iso 与 release/hdd.img
make run        # 用 QEMU 启动 cdrom.iso（KVM）
make run-uefi   # 以 UEFI 方式启动（先下载 OVMF）
make run-hdd    # 启动 release/hdd.img
```

`make run-hdd` 启动磁盘镜像。需要 `mtools` 与 `sgdisk`。

CI 不安装交叉工具链。它在 Ubuntu 24.04 上把 `x86_64-elf-gcc` 定义为 `gcc -fno-pie`，用宿主编译器构建。

## 目录结构

```
kernel/     内核（arch/x64、mm、proc、fs、ipc、router、srv）与其 C 库（lib/）
userspace/  用户态程序（servers/、bin/、test/）与汉操作系统 (HanOS) 运行时
include/    内核与用户态共享的线协议头
3rdparty/   musl 与 libvterm 的汉操作系统 (HanOS) 移植层；build/ 放下载与输出
```

musl 的构建说明见 `3rdparty/musl/README.md`。

## 程序

程序放在 initrd 与 ext2 数据分区。shell 先查 `/bin`，再查 `/data/bin`。程序名可在任意目录运行。live U 盘与 `make run-hdd` 用同一布局。

### 游戏

- `tetris`：堆叠落下的方块，消行。
- `2048`：合并相同数字，凑到 2048。
- `snake`：吃 `$` 食物，变长，避开墙与自身。

| 游戏 | 操作 |
|------|------|
| `tetris` | A/D 或方向键移动；W/上键旋转；S/下键软降；空格硬降；P 暂停；Q 退出 |
| `2048` | W/A/S/D 或方向键移动；R 重开；Q 退出 |
| `snake` | W/A/S/D 或方向键转向；P 暂停；Q 退出 |

### 工具

- `ls`、`cat`、`wc`、`pwd`、`rm`、`echo`、`edit`
- `dmesg [关键词...]`、`usb`、`lspci`
- `help`、`hansh`

## 硬件

已测物理机：

- Haswell-ULT（i5-4300U，Lynx Point-LP）：内核把 USB2 端口从 EHCI 路由到 xHCI。live U 盘枚举成功。ext2 分区挂载。面板按原生 eDP 模式显示。游戏可运行。
- Broadwell-U（i5-5200U，Wildcat Point-LP）：同一 USB2 路由与原生 eDP 路径可用。ext2 分区挂载。游戏可运行。
- Skylake（i5-6200U，Sunrise Point-LP）：xHCI 存储枚举成功。EDID 给出原生 eDP 模式。游戏可运行。

QEMU 覆盖 BIOS 与 UEFI 启动、AHCI 盘、EHCI/xHCI USB 存储。

已知限制：

- USB 不支持热插拔。
- 开机前插好 U 盘。
- USB2 路由覆盖 Lynx Point 与 Wildcat Point。其他 Intel PCH 代际要补 quirk。
- EHCI 上无 xHCI 路由的全速设备未验证。
- 面板无 EDID 时用固件 eDP 时序。固件按 800x600 扫描时拿不到原生模式。
- 其他 Intel GPU 与 PCH ID 要补表项。

显卡工作记录见 `attic/2026/2026-10-22-gen75-edp-and-edid-zh.md`。USB 工作记录见 `attic/2026/2026-10-18-haswell-usb2-ports-zh.md`。

## CI/CD

`.github/workflows/` 下有三个 GitHub Actions 工作流：

- `build.yml` 在每次 push 与 pull request 时运行，环境为 `ubuntu-24.04`。它安装构建工具与 QEMU，把 `x86_64-elf-gcc` 定义为 `gcc -fno-pie`，构建内核、用户态、`cdrom.iso` 与 `release/hdd.img`，再用 QEMU 8.2 的 TCG 启动 ISO。`scripts/ci-boot-test.sh` 等待 shell 与启动标记：`name "hansh"`、`MK: selftest PASS`、`MK: misc PASS`、`MK: signalfd PASS`。缺标记，或出现 `panic`、`exception`、`corrupt` 行，任务失败。成功时打印串口日志。
- `doxygen-pages.yml` 在推送到 `mainline` 时运行，生成 Doxygen HTML 并发布到 [GitHub Pages](https://jjwang.github.io/HanOS/)。
- `main.yml` 在 `image-data` 分支更新行数徽章。

托管 runner 没有 `/dev/kvm`，启动测试用 TCG。本机 `make run` 用 KVM。

## 文档

`doxygen Doxyfile` 生成 HTML 文档。
