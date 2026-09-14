# 2026-09-14：用户态控制台输出与服务端回显

## 概述

为用户态提供写内核终端的能力，并把键盘回显移出内核，使输入服务可以回显它解码出的
按键。

## 控制台写路径

- `libc/bootinfo.h` 新增 `console_ep` 字段与 `CONSOLE_WRITE_TAG`。
- `kernel/ipc/input_srv.c` 创建第三个 endpoint `console_ep`，并启动一个小内核任务
  （`conkbd`），它接收 `CONSOLE_WRITE_TAG` 消息并用 `kprintf("%c", ...)` 把字节写到
  终端。
- spawn hook 为输入服务的 bootinfo 附加一个指向 `console_ep` 的 SEND 句柄。

## 回显移入输入服务

- `userspace/input.c` 在把按键转交内核之前，先通过控制台 endpoint 回显每个解码出的
  按键（含退格）。
- `kernel/fs/ttyfs.c` 在输入服务处于活动状态时跳过自身回显，使每个按键只回显一次。
  服务关闭时仍使用内核内回显作为回退。

## 变更文件

| 文件 | 变更 |
|------|------|
| `libc/bootinfo.h` | `console_ep` 句柄与 `CONSOLE_WRITE_TAG` |
| `kernel/ipc/input_srv.c` | 控制台 endpoint、`conkbd` 任务、句柄授予 |
| `kernel/fs/ttyfs.c` | 输入服务活动时跳过回显 |
| `userspace/input.c` | 通过控制台 endpoint 回显解码按键 |

## 测试

- 启用 `ENABLE_INPUT_SERVER`：键盘经服务可用，shell 在 `-smp 2` 与 `-smp 4` 上接受
  命令。
- 关闭该开关：使用内核内键盘驱动及其回显。
