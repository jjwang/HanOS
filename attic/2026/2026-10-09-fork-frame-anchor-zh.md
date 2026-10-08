# 2026-10-09：fork 帧锚点重定位（用户栈踩踏）

## 现象

进程 fork 后返回用户态，第一条 ret 即崩溃。
缺页报告 RIP `0x400000001467`，即 sys_write 的 ret。
缺页地址就是栈指针，取值很小，如 0x202、0x206、0x246。
0x202 正是上下文切换桩压入的 RFLAGS 常量。

轻载下罕见。
`ls /bin` 循环不出现。
混合循环 `ls /`、`ls /bin`、`pwd`、`ls /bin | wc`、`cat /etc/version` 共 105 条，出现 8 次。

## 复现

- `-smp 4` 启动，输入混合循环，几十条内出现。
- `-smp 1` 同样复现。
- 这是逻辑错误，不是跨核竞争。

## 排查

逐一排除：

- 原地 exec。关掉它（`if (0 && ...)`）走旧 spawn 路径，故障照旧。
- 信号帧改写。无挂起时 `signal_deliver` 走快速路径返回。shell 不装处理器。
- 系统调用返回帧。`syscall_post` 加 `rsp < 1MiB` 检查，从未触发。
- 切换边界。`do_context_switch` 检查将恢复的帧，未见用户帧 rsp 非法。
- `process_fork` 重定位。旧代码重定位后，`context`、`rsp`、`rbp` 都落在子内核栈内，范围检查通过。

0x202 是 RFLAGS 常量，指向帧错位 8 字节。
`push_all` 先压 r15，最后压 rax，保存帧 rax 在前。
`process_regs_t` 声明 r15 在前。
两套顺序相反。

## 根因

fork 出的子进程不经系统调用入口进入内核。
它从 `fork_context_switch` 的 `.exit` 恢复。
恢复由 `exit_context_switch` 完成。
系统调用返回执行 `mov rsp, [r15 - 16]`。
r15 是帧锚点，进入内核时设为 `kstack_top - 8`。

`process_fork` 拷贝父上下文，把三处重定位到子内核栈：

- 它按 `process_regs_t` 读帧，通用寄存器顺序与实际帧相反。
- `tr->rbp` 改到的其实是 r8 槽，不是 rbp。
- rsp 恰好正确。rip、cs、rflags、rsp、ss 在两布局中偏移相同。
- 它从未重定位 r15。

子进程保留父进程的 r15 锚点。
`mov rsp, [r15 - 16]` 便从父内核栈读用户 rsp。
父进程后续系统调用复用该槽后，子进程读到残值。
槽位存放切换桩帧时，残值就是 RFLAGS 常量。
缺页地址便形如 0x202。

## 修复

按 `push_all` 布局用 `syscall_regs_t` 读帧。
对 `rsp`、`rbp`、`r15`，凡落在父内核栈范围内者，重定位到子内核栈。

## 同期其它修复

- `fix(serial)`：给日志发送等待设上界。日志在自旋锁内轮询 UART 发送寄存器，宿主停止排空时阻塞两秒。改为有限轮询，超限丢弃该字节。
- `perf(ext2)`：连续块一次读。缓存间接块，物理连续的块合并为一次块服务器读取。2.2MB 映像装载从 4s 降到 1s 以内。
- `fix(sched)`：原地 exec 复位入口寄存器与信号。清零通用寄存器，ABI 用 `%rdx` 传 atexit 指针。清除已捕获信号处置与挂起信号。

## 变更文件

| 文件 | 变更 |
|------|------|
| `kernel/proc/process.c` | 按 `syscall_regs_t` 重定位 fork 帧；一并重定位 r15 |
| `kernel/arch/x64/serial.c` | 发送等待设上界 |
| `userspace/servers/ext2.c` | 缓存间接块；合并连续读 |
| `kernel/proc/sched.c` | 复位 exec 入口寄存器与信号；原地栈不映射进旧地址空间 |

## 测试

- `make -C kernel clean && make -C kernel` 无告警。
- `-smp 2` 与 `-smp 4` 引导：`selftest PASS`，无故障。
- 混合循环 105 条，`-smp 1` 与 `-smp 4`：无故障。修复前 8 次。
- 循环 `tetris` 后按 `q` 10 次：每次都返回提示符。
- `musltest` 完成：`pipe-ok`、`nanosleep ok`、`mustest: done`。

## 尚未完成

- `process_regs_t` 的通用寄存器顺序仍与保存帧相反。
- `process_make` 用它构建初始帧。
- 目前 GPR 多为 0，无害。
- 后续统一两套布局。
