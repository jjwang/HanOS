# 2026-10-31：非首领线程的 exit_group

## 现象

进程的非首领线程调用 `exit()` 后，父进程在 `wait` 上挂起，或取到状态 0，而不是该线程给出的状态。

## 根因

`k_exit_group` 只在调用者是组首领时记录进程退出（`!t->is_thread`）。非首领调用者经 `sched_kill_group` 杀掉首领，却没有保存状态。回收器随后用默认状态记录首领。

## 变更

- proc：任意调用者都按首领 pid 记录整组退出。`exit_group_record` 关闭组 fd 表。按首领 pid 通知 `PROC_EXIT`。标记首领已记录，回收器跳过。
- test：新增 `exitgrouptest`。子进程 fork 后，派生线程调用 `exit(42)`，父进程检查 `WEXITSTATUS`。

## 验证

- GCC 13.1 与交叉 GCC 10.2，TCG，`-smp 1`：`exitgrouptest PASS status=42`。
