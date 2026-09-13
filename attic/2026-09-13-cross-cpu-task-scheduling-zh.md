# 2026-09-13：跨 CPU Core 任务调度

## 概述

在此改动之前，调度器为每个核心维护一条运行队列
（`tasks_active_table[cpu_id]`），但新创建的任务总是被压入“创建它的那个
核心”的队列（`sched_add()` 使用 `smp_get_current_cpu_id()`）。其结果是
一个核心承担了全部工作负载，其余核心只运行各自的 idle 任务。

本改动让调度器可以把新建任务投放到任意在线核心上运行，并使所有跨核心的
调度操作变得安全。

## 设计

### 1. 核心选择

`sched_pick_cpu()` 遍历 SMP 信息表（`smp_get_info()`）取得真实的 APIC id，
再用一个原子计数器在所有在线核心之间轮询。使用 APIC id 而不是稠密的核心
下标，可以在 APIC id 不连续时依然保证运行队列下标正确。

`sched_add()` 现在会把任务投放到选中的核心；`sched_execve()` 也改为调用
`sched_add()`，因此用户进程会被分散到各核心，而不再总是停留在调用者所在
的核心上。

### 2. 唤醒模型

每个核心本来就运行周期性的 APIC 定时器，因此被压入远程队列的任务会在该核心
的下一个 tick 被取走，无需引入重新调度 IPI（reschedule IPI），从而避免了
额外的中断向量及其锁序风险。

### 3. 跨核心查找与回收

由于父任务与其子任务现在可能位于不同核心，以下操作被改为感知全部核心：

- `sched_get_task_status()` 会依次获取各核心的 `tasks_lock`，扫描所有核心的
  运行任务与运行队列，而不再只扫描本地队列。
- `sched_cleanup()`（由 `sched_cleanup_local()` 重命名）可以在拥有该死亡任务
  的核心上定位并释放它。
- `task_idle_proc()` 通过 `sched_find_task()` 在所有核心中查找父任务，从而把被
  回收的任务从其父任务的 `child_list` 中移除，而不再局限于本地队列。

### 4. `child_list` 保护

`task_t` 新增了每任务的 `child_lock`。所有对 `child_list` 的修改（fork、
exec、waitpid、idle 回收）都由该锁串行化。该锁是叶子锁：持有它时绝不会再去
获取运行队列锁，因此不会与 `tasks_lock` 形成 ABBA 死锁。`waitpid()` 会在锁内
对子任务列表做快照，释放锁后再查询任务状态。

### 5. 原子任务号分配

`task.c` 中的 `curr_tid` 现在通过 `__atomic_fetch_add()` 递增，因此两个核心
并发创建任务时不会分配到相同的 tid。

## 顺带修复的潜在缺陷：延迟睡眠任务饥饿

在测试任务投放改动时，某个核心会出现活锁。根因是：`do_context_switch()` 在
同一次扫描中既查找 `TASK_READY` 任务，也选取 `sched_sleep()` 已到期的任务。
`kupdateui` 使用 `sched_sleep(0)` 作为主动让出，它会被立刻唤醒，因此可能排在
它后面入队的就绪任务（例如被其他核心投放到本核的进程）之前被选中，导致后者
永远得不到调度。

现在扫描分为两趟：先查找 `TASK_READY` 任务；仅当不存在就绪任务时，才回退到
选取睡眠已到期的任务。

## 后续修复：跨核心 `waitpid` 可能永久阻塞

在加入任务投放后，在 shell 中运行命令（`fork` + `execve`，新进程被投放到
另一个核心）可能导致 shell 永远卡在 `waitpid`。这是两个问题叠加的结果：

1. `sched_exit()` 先把任务置为 `TASK_DYING`，随后释放 `child_lock` 时会重新
   打开中断。若此时恰好发生定时器中断，该任务会被切换出去；而 `TASK_DYING`
   状态永远不会再被调度，于是它永久停留在 `DYING`，其父任务一直认为该子任务
   仍然存活。
2. `sched_get_task_status()` 把 `TASK_DYING` 映射为 `TASK_UNKNOWN`，因此即使
   子任务卡在 `DYING`，父任务也无法观测到它已结束。

修复：

- `sched_exit()` 在进入 `DYING -> DEAD` 转换前关闭中断，并保持到最终上下文
  切换，使该转换相对定时器中断而言是原子的。
- `sched_get_task_status()` 将没有任何存活子任务的 `TASK_DYING` 任务上报为
  `TASK_DEAD`。这样即使所属核心的 idle 任务长期得不到调度，`waitpid()` 也能
  通过跨核心 `sched_cleanup()` 回收它。

## 后续修复：控制台输出节流使用了每个 CPU 独立的时钟

`k_write()` 在“上一个写者是别的任务”时会限速：等待 `sched_get_ticks()` 增加
250 后才允许新的写者输出。但 `sched_get_ticks()` 返回的是
`tasks_coordinate[cpu_id]`，即**每个 CPU 各自独立**的计数。一旦任务分散到不同
核心，一个核心上记录的计数会和另一个核心的计数比较，于是等待可能长达数秒。
运行 `ls`（被投放到核心 A）后回到 shell（运行在核心 B）时，shell 需要约 10 秒
才能打印出下一个提示符。

修复：改用全局 HPET 时钟（`hpet_get_nanos()`），阈值设为 250ms，使节流在所有
核心上行为一致。

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

在 QEMU/KVM 下以 `-smp 2` 和 `-smp 4` 启动数十次。调度日志显示任务、其子任务
以及子任务的子任务被投放到不同核心上，例如：

```
SCHED: CPU 0 dispatches tid 2 to CPU 0
SCHED: CPU 0 dispatches tid 3 to CPU 1
SCHED: child tid 5 and parent tid 3
SCHED: CPU 1 dispatches tid 5 to CPU 0
SCHED: CPU 0 dispatches tid 7 to CPU 1
```

在 shell 中运行 `help`/`ls` 时，跨核心的 `fork` + `execve` + `exit` 流程没有
出现缺页异常、通用保护异常或死锁。
