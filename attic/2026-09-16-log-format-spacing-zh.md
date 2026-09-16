# 2026-09-16：日志格式空格

## 概述

清除此前 printf 格式符重构遗留的空格填充，使数值不再带有多余前导空格（例如
`model 0x a` 变为 `model 0xa`）。

## 变更

- 十六进制标识改用零填充，宽度有意义时保留（`%02x:%02x.%01x`、
  `%04x:%04x`、`%02x`、`%03x`、`%04x`）。
- 地址去掉人为宽度（`%11lx`/`%11x` 改为 `%lx`/`%x`）。
- 小型十进制字段不再指定宽度（`%2d`、`%4d`、`%8d`、`%11d` 改为 `%d`）。
- 涉及文件：`kernel/sys/cpu.c`、`kernel/sys/pci.c`、
  `kernel/device/storage/ata.c`、`kernel/device/display/gfx.c`、
  `kernel/proc/syscall.c`、`kernel/proc/elf.c`、`kernel/fs/fat32.c`、
  `kernel/fs/vfs.c`、`kernel/mm/pmm.c`、`kernel/mm/buddy.c`、
  `kernel/kmain.c`。

## 测试

- `make -C kernel clean && make -C kernel` 无警告，完整镜像可构建。
- 串口日志现在显示 `CPU 0: model 0xa, family 0x6` 与
  `PCI: 00:00.0 - 8086:29c0`；启动横幅显示 `Monitor : 40 x 30 cm`、
  `Memory : 1068 MB`；启动到达 `name "hansh"`，无 panic。
