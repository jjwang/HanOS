# 2026-09-13：跨 CPU Core 任务调度

## 概述

- 改动前，调度器为每个核心维护一条运行队列（`tasks_active_table[cpu_id]`）。
- `sched_add()` 使用 `smp_get_current_cpu_id()`。
- 新建任务总是进入创建者核心的队列。
- 结果是一个核心运行所有负载，其余核心只运行 idle 任务。

- 本改动把新建任务投放到任意在线核心。
- 它保证所有跨核心调度操作安全。

## 设计

### 1. 核心选择

- `sched_pick_cpu()` 遍历 SMP 信息表（`smp_get_info()`），取得真实 APIC id。
- 它用原子计数器在在线核心之间轮询。
- 使用 APIC id，而非稠密核心下标。
- APIC id 不连续时，运行队列下标仍正确。
- `sched_add()` 把任务投放到选中的核心。
- `sched_execve()` 改为调用 `sched_add()`。
- 用户进程分散到各核心，不再停留在调用者核心。

### 2. 唤醒模型

- 每个核心运行周期性的 APIC 定时器。
- 该核心在下一个 tick 取走远程队列中的任务。
- 无需重新调度 IPI（reschedule IPI）。
- 无需新增中断向量，也避免锁序风险。

### 3. 跨核心查找与回收

父任务与子任务现在可能位于不同核心。以下操作改为感知所有核心：

- `sched_get_task_status()` 依次获取各核心的 `tasks_lock`。
- 它扫描所有核心的运行任务与运行队列。此前只扫描本地队列。
- `sched_cleanup()`（由 `sched_cleanup_local()` 重命名）在死亡任务所在的核心上定位并释放它。
- `task_idle_proc()` 通过 `sched_find_task()` 在所有核心中查找父任务。
- 它把回收的任务从父任务 `child_list` 移除。
- 不再局限于本地队列。

### 4. `child_list` 保护

- `task_t` 新增每任务的 `child_lock`。
- 该锁串行化所有对 `child_list` 的修改（fork、exec、waitpid、idle 回收）。
- 它是叶子锁。
- 持有它时，调用者不再获取运行队列锁。
- 它不会与 `tasks_lock` 形成 ABBA 死锁。
- `waitpid()` 在锁内对子任务列表做快照。
- 释放锁后再查询任务状态。

### 5. 原子任务号分配

- `task.c` 中的 `curr_tid` 通过 `__atomic_fetch_add()` 递增。
- 两个核心并发创建任务时，不会分配到相同 tid。

## 顺带修复的潜在缺陷：延迟睡眠任务饥饿

- 测试任务投放改动时，某核心出现活锁。
- 根因：`do_context_switch()` 在同一次扫描中查找 `TASK_READY`。
- 它同时选取睡眠已到期的任务。
- `kupdateui` 使用 `sched_sleep(0)` 作为主动让出。
- 该让出会立刻唤醒。
- 调度器可能先选中它。
- 后面入队的就绪任务得不到调度。
- 例如其他核心把进程投放到本核。

现在扫描分两趟：

- 先查找 `TASK_READY` 任务。
- 没有就绪任务时，才回退到睡眠已到期的任务。

## 后续修复：跨核心 `waitpid` 可能永久阻塞

- 加入任务投放后，在 shell 中运行命令可能使 shell 卡在 `waitpid`。
- 命令为 `fork` + `execve`。调度器把新进程投放到另一个核心。
- 两个问题叠加：

1. `sched_exit()` 先把任务置为 `TASK_DYING`，随后释放 `child_lock`。
- 释放锁会重新打开中断。
- 若此时发生定时器中断，定时器切出该任务。
- 调度器不会再调度 `TASK_DYING` 任务。
- 任务永久停留在 `DYING`。
- 其父任务一直认为子任务存活。

2. `sched_get_task_status()` 把 `TASK_DYING` 映射为 `TASK_UNKNOWN`。
- 子任务卡在 `DYING` 时，父任务无法观测到它已结束。

修复：

- `sched_exit()` 在进入 `DYING -> DEAD` 转换前关闭中断。
- 中断保持关闭直到最终上下文切换。
- 该转换对定时器中断原子。
- `sched_get_task_status()` 把没有存活子任务的 `TASK_DYING` 上报为 `TASK_DEAD`。
- 所属核心的 idle 任务可能长期得不到调度。
- `waitpid()` 也能通过跨核心 `sched_cleanup()` 回收它。

## 后续修复：控制台输出节流使用了每 CPU 独立的时钟

- `k_write()` 对任务的首次写入限速。
- 条件：上一个写者是别的任务。
- 它等待 `sched_get_ticks()` 增加 250。
- `sched_get_ticks()` 返回 `tasks_coordinate[cpu_id]`，即每 CPU 独立的计数。
- 任务分散到不同核心。
- 一个核心的计数与另一个核心的计数比较。
- 等待可能长达数秒。
- 运行 `ls`（投放到核心 A）。
- 回到 shell（运行在核心 B）。
- shell 需要约 10 秒才打印下一个提示符。

修复：

- 改用全局 HPET 时钟（`hpet_get_nanos()`），阈值设为 250ms。
- 节流在所有核心上行为一致。

## 变更文件

| 文件 | 变更 |
|------|------|
| `kernel/proc/sched.c` | 新增 `sched_pick_cpu()`、`sched_find_task()`；跨核心的 `sched_add()`/`sched_execve()`、`sched_get_task_status()`、`sched_cleanup()`；`task_idle_proc()` 全局查找父任务；使用 `child_lock`；`sched_resume_event()` 加锁；`do_context_switch()` 两趟选择就绪/睡眠任务 |
| `kernel/proc/sched.h` | `sched_cleanup_local()` 改为 `sched_cleanup()` |
| `kernel/proc/task.c` | `curr_tid` 原子分配；fork 时重置 `child_lock`；加锁修改 `child_list` |
| `kernel/proc/task.h` | `task_t` 新增 `lock_t child_lock` |
| `kernel/proc/syscall.c` | `waitpid()` 在 `child_lock` 下对 `child_list` 做快照并使用跨核心 `sched_cleanup()`；新增 `base/kmalloc.h` 头文件；`k_write()` 输出节流改用全局 HPET 时钟而非每 CPU 独立的 tick 计数 |
| `kernel/kconfig.h` | 保持 `ENABLE_KLOG_DEBUG` 关闭 |

## 测试

- 在 QEMU/KVM 下以 `-smp 2` 和 `-smp 4` 启动数十次。
- 调度日志显示任务、子任务、子任务的子任务。
- 调度器把它们投放到不同核心。例如：

```
SCHED: CPU 0 dispatches tid 2 to CPU 0
SCHED: CPU 0 dispatches tid 3 to CPU 1
SCHED: child tid 5 and parent tid 3
SCHED: CPU 1 dispatches tid 5 to CPU 0
SCHED: CPU 0 dispatches tid 7 to CPU 1
```

- 在 shell 中运行 `help`/`ls`，跨核心 `fork` + `execve` + `exit` 流程正常。
- 没有缺页异常、通用保护异常或死锁。
