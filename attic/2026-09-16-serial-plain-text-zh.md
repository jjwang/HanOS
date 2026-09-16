# 2026-09-16：串口输出改为纯文本

## 概述

不支持 ANSI 转义的串口监视器会把内核日志显示成方块和问号，因为每一行都带有
SGR 颜色码。现从串口流中剥离 ANSI 转义序列。

## 变更

- `kernel/base/klog.c`：串口数据先经过 `serial_keep()` 过滤，在
  `serial_write()` 之前移除 ANSI CSI 序列（`ESC [ ... 终止字节`）。状态跨字符
  保留，即使序列被拆开也能移除。内核终端与终端服务仍接收带颜色的输出。

## 测试

- `make -C kernel clean && make -C kernel` 无警告，完整镜像可构建。
- QEMU 串口日志现在不含任何 ESC 字节，也没有其他不可打印字节；每行都是可读的
  纯文本，启动仍到达 `name "hansh"`，无 panic。
