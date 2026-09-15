# 2026-09-15：进程数据结构

## 概述

为进程（此前的 “task”）数据结构给出更清晰的命名与类型，并将按 CPU 线性
查找 pid 改为全局进程表。

## 变更

- 将 `task_t` 重命名为 `process_t`，`task_id_t` 重命名为 `pid_t`。
  `fs/vfs.h` 中原有的 int32 `pid_t` 改名为 `posix_pid_t`（仅被未使用的
  `siginfo_t` 引用），因此 `pid_t` 现在表示进程 id。
- `task_regs_t`、`task_status_t`、`task_mode_t`、`task_priority_t` 以及
  `TASK_*` 常量相应改为 `process_*` 与 `PROC_*`。
- 将 `kernel/proc/task.{c,h}` 重命名为 `kernel/proc/process.{c,h}`。
- id 字段 `tid`/`ptid` 改名为 `pid`/`ppid`；删除未使用的 `tstack_limit`、
  `aux` 字段以及 `event_t` 中的死字段；将结构体按上下文、身份、资源、
  地址空间和信号分组。
- 新增 `io_port_range_t`（与 `bootinfo_t` 共用）和 `signal_state_t`。
- 新增全局进程表：`process.c` 中按 pid 键控的侵入式链式哈希，通过
  `process_lookup()` 暴露。`sched_find_task()` 与
  `sched_get_task_status()` 改用它，不再扫描每个运行队列。
- 调度器全局变量改名为 `run_queues`、`running_task`、`idle_task`、
  `tick_count` 和 `run_queue_lock`。

## 变更文件

| 文件 | 变更 |
|------|------|
| `kernel/proc/process.{c,h}` | 进程结构体、类型与全局进程表 |
| `kernel/proc/task.{c,h}` | 重命名为 `process.{c,h}` |
| `kernel/proc/sched.{c,h}` | 使用 `process_t`/`pid_t`，调度器状态改名 |
| `libc/bootinfo.h` | 共用 `io_port_range_t` |
| 内核全局 | `task`/`tid` 命名替换为 `process`/`pid` |

## 测试

- `make -C kernel clean && make -C kernel` 无警告，完整镜像可构建。
- SMP2 与 SMP4 启动均到达 `name "hansh"`，无 panic；shell 仍可运行
  `ls`/`pwd`，死亡进程可被回收。
