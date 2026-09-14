# 2026-09-14：用户态输入服务

## 概述

首个用户态设备服务。PS/2 键盘中断及其 I/O 端口被交给 `/bin/input`，由它解码扫描码
并通过 IPC 把字符送回内核。启用该服务后，键盘输入不再运行在内核中。

## 共享键盘码表

`kernel/device/keyboard/keycode.{c,h}` 移到 `libc/`，使内核内回退驱动与用户态服务
共用同一份扫描码到 ASCII 的表。

## 启动协议（`libc/bootinfo.h`）

`bootinfo_t` 携带内核授予服务的资源：两个 endpoint 句柄（中断通知入、解码按键出）、
中断号，以及被授予的 I/O 端口范围。该头文件同时定义共享的消息标签。

## 内核改动

- `task_t.bootinfo` 保存服务的启动信息块；在 `task_free()` 中释放。
- `SYS_BOOTINFO`（63）把当前任务的 bootinfo 拷贝到用户空间。
- `sched_set_spawn_hook()` 在 `sched_execve()` 内部、**新任务变为可运行之前**执行
  回调，使启动者可以在不与新任务竞争的情况下附加句柄、端口范围与 bootinfo。
- `kernel/ipc/input_srv.{h,c}`：
  - 为 1 号线创建 IRQ 对象并绑定到通知 endpoint；
  - 创建通知 endpoint 与按键 endpoint；
  - 启动 `DEFAULT_INPUT_SVR`，并通过 spawn hook 附加句柄/端口/bootinfo；
  - 启动一个小内核任务（`inkbd`），把解码后的按键转投到事件总线，从而保持终端
    路径不变。
- `kconfig.h`：`DEFAULT_INPUT_SVR` 为 `/bin/input`，并新增
  `ENABLE_INPUT_SERVER` 开关（默认关闭；关闭时使用内核内驱动）。
- `kmain`：启用时从 `kshell` 启动该服务。

## 用户态服务（`userspace/input.c`）

等待中断通知，通过 `sys_ioport_access` 排空控制器，解码 make/break 扫描码（跟踪
shift、caps lock 与 ctrl，并把 Ctrl-D 作为 EOF），再用 `sys_ipc_send` 把每个按键
发给内核。经用户态 makefile 构建到 `/bin/input`。

## 用户/内核接口

`libc/sysfunc.{h,c}` 在既有 IPC、IRQ 与 I/O 端口封装之外新增 `sys_bootinfo()`。

## 变更文件

| 文件 | 变更 |
|------|------|
| `libc/keycode.{c,h}` | 共享扫描码表（从内核移出） |
| `libc/bootinfo.h` | bootinfo 块与消息标签 |
| `kernel/ipc/input_srv.{h,c}` | 启动并驱动输入服务 |
| `kernel/proc/task.{h,c}` | 每任务 bootinfo |
| `kernel/proc/syscall.{h,c}` | `SYS_BOOTINFO` |
| `kernel/proc/sched.{h,c}` | spawn hook |
| `kernel/ipc/irq.c` | IRQ 投递（接口不变） |
| `kernel/kmain.c`、`kconfig.h` | 启用时启动服务 |
| `userspace/input.c`、`userspace/GNUmakefile` | 服务本体 |
| `libc/sysfunc.{h,c}` | `sys_bootinfo()` |

## 测试

- 启用 `ENABLE_INPUT_SERVER`：日志出现 `input: server started`，键盘经服务在
  `-smp 2` 与 `-smp 4` 上可用（`ls` → `pwd`）。
- 关闭该开关：由内核内键盘驱动处理 1 号线，shell 仍可用。
