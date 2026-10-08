# 2026-10-08：原地 execve 与系统调用内核栈切换

## 现象

交互 shell 在游戏退出后挂起。
复现：玩 tetris 约 15 秒，按 q。
长会话下提示符不再返回。
shell 阻塞在 wait。

## 背景

execve 原先把调用者换成一个新进程。
父进程 fork 子进程。
子进程在孙进程里运行映像，随后退出。
父进程经进程服务器等待孙进程。
服务器的影子状态 procs[] 记录退出与子进程计数。
计数与内核不一致时，等待无法完成。

修复让 execve 在调用进程内完成。
映像原地替换，pid 不变。
父进程等待的正是它 fork 的进程。

## 根因

三处缺陷拦住原地路径。
每处造成不同的失败。

### 1. 系统调用处理运行在用户栈

探针显示处理运行在调用者的用户栈。
中途切地址空间会解除内核自身栈的映射。
随后触发三重故障。
syscall_handler 现将用户 RSP 存入 gs:16。
RSP 切到 gs:24，即内核栈顶。
帧在内核栈上构建。

### 2. FS 基址（TLS）残留

sched.c 仅在 next->fs_base != 0 时重载 FS。
原地 exec 保留旧映像的 TLS 基址。
新映像线程指针错误，跳到低地址 0x267。
exec 现将 FS 基址清零。

### 3. 每 CPU 系统调用帧被改写

cpu->syscall_frame 每 CPU 一个槽。
elf_load 经文件系统服务读映像并阻塞。
同 CPU 的其他系统调用改写该槽。
阻塞后再取帧会写到别的进程。
故障地址与 pid 每次不同。
原地路径在阻塞前先取帧。
帧位于调用者内核栈，跨阻塞仍有效。

## 实现

### 内核栈切换（f4d2e85）

- syscall_handler.asm 将用户 RSP 存入 gs:16，RSP 切到 gs:24。
- cpu_t 新增 syscall_user_rsp（偏移 16）与 syscall_kstack（偏移 24）。
- do_context_switch 设置 cpu->syscall_kstack = next->kstack_top。

### 原地 exec（8700a06）

- sched_execve_prep 构建新进程，含地址空间、已装载映像与用户栈。
- 它接收 fork_fds 标志。
- 原地路径传 false，跳过 fd 克隆。
- 进程服务器不再多持一份 tty 端点。
- sched_execve_inplace 把新进程的用户态移入当前进程。
- 移入地址空间、上下文与用户栈。
- 接着切 CR3，释放旧地址空间。
- 清零 FS 基址，重写系统调用返回帧。
- k_execve 对用户进程调用 sched_execve_inplace。
- spawn 路径保留为回退。
- process_free_addrspace 释放地址空间及其映射。
- process_free 与原地路径共用它。

## 变更文件

| 文件 | 变更 |
|------|------|
| `kernel/proc/syscall_handler.asm` | 系统调用入口切内核栈 |
| `kernel/arch/x64/smp.h` | 新增 `syscall_user_rsp`、`syscall_kstack` |
| `kernel/proc/sched.c` | 设置 `syscall_kstack`；新增 `sched_execve_prep`、`sched_execve_inplace` |
| `kernel/proc/sched.h` | 声明 `sched_execve_inplace` |
| `kernel/proc/syscall.c` | `k_execve` 调用原地路径 |
| `kernel/proc/process.c` | 新增 `process_free_addrspace` |
| `kernel/proc/process.h` | 声明 `process_free_addrspace` |

## 测试

- `make -C kernel clean && make -C kernel` 无告警。
- `-smp 2` 与 `-smp 4` 引导：`selftest PASS`，无故障。
- 交互 `ls /`、`pwd`、`ls /bin`、管道：正确，无故障。
- 循环 `tetris` 后按 `q` 20 次：每次都返回提示符。
- `musltest` 完成：`pipe-ok`、`nanosleep ok`、`mustest: done`。

## 尚未完成

- 原地路径每次 exec 分配一个临时进程。
- 后续改为直接复位当前进程。
- 进程服务器仍保留影子状态。
- 后续把 fork/exec/exit/wait 权威移入内核。
