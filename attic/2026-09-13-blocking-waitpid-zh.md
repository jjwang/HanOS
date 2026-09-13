# 2026-09-13：带退出状态与子进程退出唤醒的阻塞式 `waitpid`

## 概述

重写了 `kernel/proc/syscall.c` 中的 `k_waitpid()`。旧版本有两个严重 bug，且不支持
退出状态。

## 修复的 bug

1. **`pid > 0` 分支永久忙等。** 子进程已死时该分支直接落空并无限循环、不睡眠，其后
   的代码不可达。
2. **`pid` 按 64 位比较。** syscall 传入的是 32 位零扩展的 `-1`，即
   `0x00000000FFFFFFFF`（`4294967295`），因此 `pid == -1` 永不成立。`init` 的
   `wait(-1)` 于是立即返回 `ECHILD`，导致 shell 被反复重启。

## 新行为

- `pid == -1` / `pid == 0` 等任意子进程；`pid > 0` 等指定子进程。比较前先把 `pid`
  截断为 `int32_t`。
- 支持 `WNOHANG`（不再忽略 `flags`）。
- 子进程退出状态由 `sched_exit()` 存入新增的 `task_t.exit_status`，并通过 `status`
  指针返回。
- 父进程在 `sched_wait_child()` 中阻塞（10ms 超时），子进程退出时由
  `sched_wake_child_waiter()` 提前唤醒。

## 调度器支持

- `task_t` 新增 `exit_status`。
- `sched_exit(status)` 保存状态，并调用 `sched_wake_child_waiter()` 唤醒睡眠在
  `EVENT_CHILD_EXIT` 上的父进程。
- 新增 `sched_reap(tid, &status)` 取代 `sched_cleanup()`；它报告任务是被回收、仍存活
  还是已消失。它使用“有效状态”，因此即便 idle 回收者被饿死，因替换进程退出而停留在
  `TASK_DYING` 的 exec 包装进程也能被回收。
- 删除了不可达的重试循环。

## 变更文件

| 文件 | 变更 |
|------|------|
| `kernel/proc/syscall.c` | 重写 `k_waitpid()`；新增 `base/kmalloc.h`；`k_write()` 节流改用全局 HPET 时钟 |
| `kernel/proc/sched.c` | `exit_status`、`sched_wake_child_waiter()`、`sched_wait_child()`、`sched_reap()` |
| `kernel/proc/sched.h` | `sched_wait_child()`、`sched_reap()` 声明 |
| `kernel/proc/task.h` | `exit_status` 字段、`EVENT_CHILD_EXIT` |

## 测试

`-smp 2` 上运行 `ls` 后再运行 `pwd`，能返回 shell 并回收 exec 包装进程与 `ls` 任务；
启动时不再反复重启 shell。
