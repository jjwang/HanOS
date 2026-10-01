# 2026-09-14：IRQ 对象、I/O 端口授权与 IPC libc 封装

## 概述

- 新增用户态设备驱动所需的内核机制。
- 把新 IPC 与能力 syscall 暴露给用户态。
- 默认不绑定中断线。
- 内核内键盘驱动照常运行。

## IRQ 对象（`kernel/ipc/irq.{h,c}`）

- `irq_obj_t` 把一条中断线包装成内核对象。
- `irq_create(irq)` 把对象登记到按线索引的表。
- `irq_bind(io, ep)` 绑定到 endpoint。
- `irq_ack(io)` 计数确认。
- `irq_deliver(irq)` 向绑定 endpoint 发送短 IPC 消息（`IRQ_NOTIFY_TAG`）。
- 对象析构时清空表项。
- 对象释放后不再收到投递。

## 中断路由（`kernel/sys/isr.c`）

- 硬件线 IRQ0..IRQ15 走新路径。
- `exc_handler_proc()` 先尝试 `irq_deliver(excno - IRQ0)`。
- 绑定后发送通知、发 EOI，跳过内核内处理函数。
- 未绑定时沿用原有处理函数。

## I/O 端口授权

- `task_t` 新增 `io_ports[4]` 范围与 `io_port_count`。
- `IOPORT_ACCESS`（61）代表调用者执行 `in`/`out`。
- 宽度为 1/2/4，按范围校验。
- 端口不在授权范围内，返回 `EPERM`。

## 新增 syscall

| 编号 | 名称 | 用途 |
|------|------|------|
| 57 | `IRQ_BIND` | 把 IRQ 对象句柄绑定到 endpoint 句柄 |
| 58 | `IRQ_ACK` | 确认一个 IRQ 对象 |
| 61 | `IOPORT_ACCESS` | 按范围校验的端口输入/输出 |

## 用户态接口（`libc/sysfunc.{h,c}`）

- `sys_ipc_msg_t` 与内核 `ipc_msg_t` 布局一致。
- 新增封装：`sys_ep_create`、`sys_ipc_send/recv/call/reply`、`sys_irq_bind/ack`、 `sys_handle_close`、`sys_ioport_access`。

## 自测

- 内核自测在一条未使用的线上创建 IRQ 对象。
- 绑定到 endpoint，调用 `irq_deliver()`。
- 检查通知是否到达。

## 变更文件

| 文件 | 变更 |
|------|------|
| `kernel/ipc/irq.{h,c}` | 新增 IRQ 对象与投递 |
| `kernel/sys/isr.c` | 把绑定 IRQ 路由到 endpoint |
| `kernel/proc/task.h` | 授予 I/O 端口范围 |
| `kernel/proc/syscall.{h,c}` | 新增 `IRQ_BIND`/`IRQ_ACK`/`IOPORT_ACCESS` |
| `kernel/ipc/selftest.c` | 新增 IRQ 投递测试 |
| `libc/sysfunc.{h,c}` | 新增 IPC/IRQ/端口 syscall 封装 |

## 测试

- `make` 构建内核与用户态，无告警。
- 开启 `ENABLE_MICROKERNEL_SELFTEST` 后，`-smp 2` 与 `-smp 4` 均打印 `MK: selftest PASS`。
- 启动与 `ls` → `pwd` 仍可用。
- 未绑定中断线，键盘路径不受影响。
