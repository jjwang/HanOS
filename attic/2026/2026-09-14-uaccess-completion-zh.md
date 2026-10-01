# 2026-09-14：完成用户缓冲区系统调用改造

## 概述

- 用户缓冲区系统调用改用用户内存访问辅助函数。
- 此前进程、信号、时间等处理程序直接解引用用户指针。

## 已改造的处理程序（`kernel/proc/syscall.c`）

- `k_execve`：路径、`argv`/`envp` 指针数组及其引用的字符串先复制到内核内存。
- 新增 `copy_exec_argv()` / `free_exec_argv()` 辅助函数，上限 32 项。
- 复制完成后运行 `sched_execve()`。
- `k_sigprocmask`、`k_sigaction`：用 `copy_from_user` / `copy_to_user` 复制信号集和动作结构。
- `k_getclock`：在内核缓冲区构造时间，再复制出去。
- `k_getrusage`：用 `clear_user` 清零结构。
- `k_getentropy`：填充内核缓冲区，再复制出去。
- `k_debug_log`、`k_runcmd`：先复制用户字符串。
- `k_pipe`：用 `copy_to_user` 写入两个描述符。
- `k_futex_wait`、`k_futex_wake`：用 `copy_from_user` 读取 futex 字。
- `k_waitpid`：用 `copy_to_user` / `clear_user` 写 `status`。

## 变更文件

| 文件 | 变更 |
|------|------|
| `kernel/proc/syscall.c` | 改造其余用户缓冲区处理程序；新增 `copy_exec_argv` 辅助函数 |

## 测试

- `make -C kernel clean && make -C kernel` 无告警，全量镜像可构建。
- shell 仍可运行 `ls` 和 `pwd`。
- 覆盖 `execve`、`waitpid`、文件 I/O 以及信号/时间路径。
