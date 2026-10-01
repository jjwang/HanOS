# 2026-09-14：用户态控制台服务

## 概述

- 把 framebuffer 所有权与文本渲染移出内核。
- 用户态 `/bin/console` 服务映射 framebuffer。
- 服务维护后台缓冲，渲染内核控制台输出。
- 内核把输出转发给服务，停止触碰 framebuffer。

## 控制台服务（`userspace/console.c`）

- 在内核授予的 endpoint 上接收控制台字节。
- 维护后台缓冲，用共享 gohufont 字形渲染。
- 排空队列后一次性 blit 到 framebuffer。
- 处理换行、回车、退格、制表、行滚动。
- 实现最小 ANSI SGR 颜色子集。
- 内核彩色输出由此渲染。
- 字体由 `userspace/font.asm` 链接。
- 字体 incbin 的 PSF 字形与内核终端一致。

## 内核管线（`kernel/ipc/console_srv.{h,c}`）

- 创建控制台 endpoint 并启动 `DEFAULT_CONSOLE_SVR`。
- spawn hook 把 framebuffer 物理区间映射进新任务。
- 映射带用户可访问与写合并属性。
- 通过 `bootinfo` 传递 framebuffer 几何、光标位置与颜色。
- `console_write_buf()` 把字节缓冲按 IPC 消息分块转发给服务。
- 服务未激活时返回失败，调用方可回退到内核内终端。

## 转发

- 控制台服务激活时，`kprintf()`（`kernel/base/klog.c`）把格式化缓冲发给服务。
- 同时跳过内核内 `term_putch()`/`term_refresh()`。
- 服务接管显示期间，`kernel/fs/ttyfs.c` 与 `kupdateui` 任务跳过光标/刷新调用。

## 接口新增

- `libc/bootinfo.h`：framebuffer 虚拟地址、宽、高、pitch、大小、光标位置与颜色。
- `IPC_RECV_NB`（64）：非阻塞接收，供服务单次 blit 前排空队列。
- `term_get_pos()` 访问器；`IPC_QUEUE_LEN` 提升到 64。

## 配置

- `kernel/kconfig.h` 中的 `ENABLE_CONSOLE_SERVER`（默认关闭）与 `DEFAULT_CONSOLE_SVR`。
- `kmain` 在 `kshell` 中启动该服务。

## 变更文件

| 文件 | 变更 |
|------|------|
| `kernel/ipc/console_srv.{h,c}` | 启动控制台服务并转发输出 |
| `kernel/base/klog.c` | 把 `kprintf` 输出转发给服务 |
| `kernel/fs/ttyfs.c`、`kernel/kmain.c` | 服务激活时跳过内核终端调用 |
| `kernel/device/display/term.{h,c}` | 新增 `term_get_pos()` 访问器 |
| `kernel/proc/syscall.{h,c}` | 新增 `IPC_RECV_NB` |
| `kernel/ipc/ipc.h` | 扩大 endpoint 队列 |
| `libc/bootinfo.h`、`libc/sysfunc.{h,c}` | 新增 framebuffer 字段与 `sys_ipc_recv_nb` |
| `userspace/console.c`、`userspace/font.asm` | 新增服务本体及其字体 |
| `userspace/GNUmakefile` | 构建 `console` |

## 测试

- 启用控制台服务后，screendump 显示已渲染文本。
- 运行 shell 命令后画面发生变化。
- 无异常。
- 输入服务与控制台服务同时启用，在 `-smp 2` 与 `-smp 4` 上均正常。
- 两者关闭时，内核内终端与键盘路径保持不变。
