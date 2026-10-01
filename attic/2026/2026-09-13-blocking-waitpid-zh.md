# 2026-09-13：带退出状态与子进程退出唤醒的阻塞式 `waitpid`

## 概述

重写 `kernel/proc/syscall.c` 中的 `k_waitpid()`。旧版本有两个 bug，且不支持退出状态。

## 修复的 bug

1. **`pid > 0` 永久忙等。** 子进程已死时，该分支落空并无限循环，不睡眠。其后的代码不可达。
2. **`pid` 按 64 位比较。** syscall 传入 32 位零扩展的 `-1`，即 `0x00000000FFFFFFFF`（`4294967295`）。`pid == -1` 永不成立。`init` 的 `wait(-1)` 立即返回 `ECHILD`。`init` 反复重启 shell。

## 新行为

- `pid == -1` / `pid == 0` 等待任意子进程；`pid > 0` 等待指定子进程。
- 比较前把 `pid` 截断为 `int32_t`。
- 支持 `WNOHANG`，不再忽略 `flags`。
- `sched_exit()` 把子进程退出状态存入新增的 `task_t.exit_status`。
- `waitpid` 通过 `status` 指针返回该状态。
- 父进程在 `sched_wait_child()` 阻塞（10ms 超时）。
- 子进程退出时，`sched_wake_child_waiter()` 提前唤醒父进程。

## 调度器支持

- `task_t` 新增 `exit_status`。
- `sched_exit(status)` 保存状态，调用 `sched_wake_child_waiter()`。
- 该调用唤醒睡眠在 `EVENT_CHILD_EXIT` 上的父进程。
- 新增 `sched_reap(tid, &status)`，取代 `sched_cleanup()`。
- 它报告任务已回收、仍存活或已消失。
- 它使用有效状态。
- exec 包装进程因替换进程退出而停留在 `TASK_DYING`。
- 即使 idle 回收者饿死，也能回收该进程。
- 删除不可达的重试循环。

## 变更文件

| 文件 | 变更 |
|------|------|
| `kernel/proc/syscall.c` | 重写 `k_waitpid()`；新增 `base/kmalloc.h`；`k_write()` 节流改用全局 HPET 时钟 |
| `kernel/proc/sched.c` | `exit_status`、`sched_wake_child_waiter()`、`sched_wait_child()`、`sched_reap()` |
| `kernel/proc/sched.h` | `sched_wait_child()`、`sched_reap()` 声明 |
| `kernel/proc/task.h` | `exit_status` 字段、`EVENT_CHILD_EXIT` |

## 测试

`-smp 2` 上先运行 `ls`，再运行 `pwd`，能返回 shell。回收 exec 包装进程与 `ls` 任务。启动时不再反复重启 shell。
