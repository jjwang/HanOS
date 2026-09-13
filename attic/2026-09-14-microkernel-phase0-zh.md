# 2026-09-14：微内核 Phase 0 内核机制

## 概述

混合式微内核迁移的第一步：落地后续阶段依赖的内核机制，**不改变任何可观测行为**。
本阶段尚未把任何服务搬到用户态。

完整计划见本地设计文档 `design/microkernel-phase0-zh.md` 与
`design/microkernel-plan-phase0-1-zh.md`（未纳入仓库）。

## 交付物

### uaccess（`kernel/mm/uaccess.{h,c}`）

- `user_range_ok()` 依据任务页表校验范围（页 present 且用户可访问）。这点很重要，
  因为用户代码链接在 `0xffff800040000000` 窗口，而栈在低地址。
- 当前任务用 `copy_from_user` / `copy_to_user` / `clear_user` /
  `strncpy_from_user`；跨任务 IPC 拷贝用 `copy_from_task` / `copy_to_task`
  （走页表翻译 + 内核 HHDM）。
- `vmm_query()` 返回原始叶子 PTE；`vmm_map_task_phys()` 为日后的设备/内存授权把物理
  区间映射进任务。

### 内核对象与句柄模型（`kernel/ipc/object.{h,c}`）

- `kernel_object_t`：类型、原子引用计数、`impl`、`destroy`。
- 每任务 `handle_table_t`，句柄带 generation
  （`(generation << 16) | index`），槽位复用后旧句柄失效。
- `handle_alloc` / `handle_get` / `handle_close`；权限掩码
  （`READ`/`WRITE`/`SEND`/`RECV`/`MAP`/`TRANSFER`）。
- 接入 `task_make`（初始化）、`task_fork`（重置）、`task_free`（销毁）。

### IPC（`kernel/ipc/ipc.{h,c}`）

- `endpoint_t` = 自旋锁 + 16 深消息队列；`ipc_msg_t` 携带 tag、6 个内联字和最多 2 个
  可转移句柄。
- `ipc_send` / `ipc_recv` / `ipc_recv_timeout` / `ipc_call` / `ipc_reply`。
- 跨核阻塞：`sched_wait_key()` / `sched_wake_key()` 复用已验证的 `sched_wake_*` 扫描
  模式，并带超时兜底，避免丢唤醒导致死锁。

### notify（`kernel/proc/notify.{h,c}`）

基于 endpoint 的类型化通知（`notify_publish` / `notify_subscribe`）；Phase 1 用它把
IRQ 投递给服务。

### 服务路由（`kernel/service/service.{h,c}`）

`service_register` / `service_lookup` / `service_forward`。Phase 0 不注册任何服务，
因此路由器休眠，syscall 走内核内路径。

### 新增 syscall

`EP_CREATE 50`、`IPC_SEND 51`、`IPC_RECV 52`、`IPC_CALL 53`、`IPC_REPLY 54`、
`HANDLE_CLOSE 60`。消息经 `uaccess` 与用户态互拷。

### 自测

`kernel/ipc/selftest.{h,c}`，由 `ENABLE_MICROKERNEL_SELFTEST` 控制，启动时运行，覆盖
消息队列、超时路径与句柄表，并打印 `MK: selftest PASS` / `FAIL`。

### 改造

`k_getcwd()` 与 `k_openat()` 改走 uaccess 层（路径先拷进内核再使用）。

## 尚未完成

- 其余用户缓冲区 syscall（`k_read`/`k_write` 需要 bounce buffer；
  `k_chdir`/`k_unlink`/… 的路径；`execve` 的 argv/envp；信号）。
- 用于安全拷贝的 `#PF` 修复。
- 把 `syscall_funcs[]` 接到路由器，以及用 `notify` 重实现 `eventbus`。

## 变更文件

| 文件 | 变更 |
|------|------|
| `kernel/mm/uaccess.{h,c}` | 新增用户访问辅助 |
| `kernel/mm/vmm.c`、`mm.h` | `vmm_query()`、`vmm_map_task_phys()` |
| `kernel/ipc/object.{h,c}` | 内核对象 + 句柄表 |
| `kernel/ipc/ipc.{h,c}` | endpoint 与 IPC |
| `kernel/ipc/selftest.{h,c}` | 启动自测 |
| `kernel/proc/notify.{h,c}` | 通知原语 |
| `kernel/service/service.{h,c}` | 服务路由脚手架 |
| `kernel/proc/task.{h,c}` | 句柄表、`wakeup_key`、`EVENT_IPC` |
| `kernel/proc/sched.{h,c}` | `sched_wait_key()` / `sched_wake_key()` |
| `kernel/proc/syscall.{h,c}` | IPC/句柄 syscall；`k_getcwd`/`k_openat` 走 uaccess |
| `kernel/kmain.c`、`kconfig.h` | 可选自测任务 |

## 测试

- `make -C kernel clean && make -C kernel` 无告警。
- 开启 `ENABLE_MICROKERNEL_SELFTEST` 后，`-smp 2` 与 `-smp 4` 均打印
  `MK: selftest PASS`（4 核连续 3/3）。
- 关闭该开关后，`-smp 2` 启动正常，`ls` → `pwd` 仍可用。
