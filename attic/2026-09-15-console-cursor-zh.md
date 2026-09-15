# 2026-09-15：终端服务光标

## 概述

在用户态终端服务接管屏幕后恢复闪烁光标；此前该服务只渲染文本，从不绘制
光标。

## 变更

- `userspace/console.c`：在下一个单元绘制光标字形，并每 500 毫秒闪烁一次；
  有输出绘制时保持常亮。渲染拆分为 `draw_cell()`，可绘制任意单元，包括
  光标。
- `kernel/proc/syscall.{c,h}` 与 `libc/sysfunc.{c,h}`：新增
  `IPC_RECV_TIMEOUT`（系统调用 65），即带毫秒超时的接收，供服务驱动闪烁。

## 变更文件

| 文件 | 变更 |
|------|------|
| `userspace/console.c` | 光标绘制与闪烁 |
| `kernel/proc/syscall.{c,h}` | `IPC_RECV_TIMEOUT` 处理程序与编号 |
| `libc/sysfunc.{c,h}` | `sys_ipc_recv_timeout` 封装 |

## 测试

- `make -C kernel clean && make -C kernel` 无警告，完整镜像可构建。
- 间隔抓取的帧缓冲截图显示光标在提示符行上切换；SMP2 与 SMP4 启动以及
  交互式 `ls`/`pwd` 均通过。
