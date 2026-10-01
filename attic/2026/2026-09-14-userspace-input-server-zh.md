# 2026-09-14：用户态输入服务

## 概述

- 首个用户态设备服务。
- 内核把 PS/2 键盘中断及其 I/O 端口交给 `/bin/input`。
- 服务解码扫描码。
- 服务通过 IPC 把字符送回内核。
- 启用服务后，键盘输入不再运行在内核中。

## 共享键盘码表

- `kernel/device/keyboard/keycode.{c,h}` 移到 `libc/`。
- 内核内回退驱动与用户态服务共用同一键盘码表。

## 启动协议（`libc/bootinfo.h`）

- `bootinfo_t` 携带内核授予服务的资源。
- 资源含两个 endpoint 句柄。
- 句柄为中断通知入与解码按键出。
- 资源还含中断号与授权 I/O 端口范围。
- 该头文件同时定义共享消息标签。

## 内核改动

- `task_t.bootinfo` 保存服务的启动信息块。
- `task_free()` 释放该块。
- `SYS_BOOTINFO`（63）把当前任务的 bootinfo 拷贝到用户空间。
- `sched_set_spawn_hook()` 在 `sched_execve()` 内部执行回调。
- 回调在**新任务变为可运行之前**执行。
- 启动者由此附加句柄、端口范围与 bootinfo。
- 附加时不与新任务竞争。
- `kernel/ipc/input_srv.{h,c}`：
  - 为 1 号线创建 IRQ 对象，绑定到通知 endpoint。
  - 创建通知 endpoint 与按键 endpoint。
  - 启动 `DEFAULT_INPUT_SVR`。
  - 通过 spawn hook 附加句柄/端口/bootinfo。
  - 启动小内核任务（`inkbd`），把解码按键转投到事件总线。
  - 终端路径保持不变。
- `kconfig.h`：`DEFAULT_INPUT_SVR` 为 `/bin/input`。
- 新增 `ENABLE_INPUT_SERVER` 开关，默认关闭。
- 关闭时使用内核内驱动。
- `kmain`：启用时从 `kshell` 启动该服务。

## 用户态服务（`userspace/input.c`）

- 等待中断通知。
- 通过 `sys_ioport_access` 排空控制器。
- 解码 make/break 扫描码。
- 跟踪 shift、caps lock 与 ctrl。
- Ctrl-D 作为 EOF。
- 用 `sys_ipc_send` 把每个按键发给内核。
- 经用户态 makefile 构建到 `/bin/input`。

## 用户/内核接口

- `libc/sysfunc.{h,c}` 新增 `sys_bootinfo()`。
- 该封装与既有 IPC、IRQ、I/O 端口封装并列。

## 变更文件

| 文件 | 变更 |
|------|------|
| `libc/keycode.{c,h}` | 从内核移出扫描码表并共享 |
| `libc/bootinfo.h` | 新增 bootinfo 块与消息标签 |
| `kernel/ipc/input_srv.{h,c}` | 启动并驱动输入服务 |
| `kernel/proc/task.{h,c}` | 新增每任务 bootinfo |
| `kernel/proc/syscall.{h,c}` | 新增 `SYS_BOOTINFO` |
| `kernel/proc/sched.{h,c}` | 新增 spawn hook |
| `kernel/ipc/irq.c` | 保留 IRQ 投递（接口不变） |
| `kernel/kmain.c`、`kconfig.h` | 启用时启动服务 |
| `userspace/input.c`、`userspace/GNUmakefile` | 新增服务本体 |
| `libc/sysfunc.{h,c}` | 新增 `sys_bootinfo()` |

## 测试

- 启用 `ENABLE_INPUT_SERVER`：日志出现 `input: server started`。
- 键盘经服务在 `-smp 2` 与 `-smp 4` 上可用（`ls` → `pwd`）。
- 关闭该开关：内核内键盘驱动处理 1 号线。
- shell 仍可用。
