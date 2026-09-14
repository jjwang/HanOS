# 2026-09-15：缺省启用输入与终端服务

## 概述

将用户态输入服务和终端服务作为缺省的键盘与显示路径，并修复启用终端服务后
出现的卡死。

## 变更

- `kernel/kconfig.h`：`ENABLE_INPUT_SERVER` 与 `ENABLE_CONSOLE_SERVER` 缺省
  改为 `true`，除非显式关闭，否则使用服务形式。
- `kernel/ipc/console_srv.c`：`kprintf()` 的输出先写入环形缓冲区，再由专门的
  内核线程 `conflush` 转发。系统调用运行在关中断状态，此前的忙等会占住 CPU
  并饿死终端服务，导致 shell 卡死。现在日志写入不再阻塞，转发线程在服务端
  排空时可让出 CPU。
- `userspace/console.c`：记录脏行，每批只将脏行拷贝到帧缓冲，而不是整屏。

## 变更文件

| 文件 | 变更 |
|------|------|
| `kernel/kconfig.h` | 缺省启用输入与终端服务 |
| `kernel/ipc/console_srv.c` | 终端输出改为环形缓冲区加转发线程 |
| `userspace/console.c` | 按脏行刷新帧缓冲 |

## 测试

- `make -C kernel clean && make -C kernel` 无警告，完整镜像可构建。
- SMP2 与 SMP4 无头启动均到达 `name "hansh"`，无 panic。
- shell 通过服务执行 `ls`（用退格修正了输错的字符）与 `pwd`，帧缓冲截图
  显示横幅、提示符与目录列表。
