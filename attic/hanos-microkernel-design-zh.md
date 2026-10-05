# HanOS：一种混合微内核的架构

*本文按当前实现描述。实现变化时同步修订。*

## 摘要

HanOS 是面向 x86-64、用 C 编写的操作系统。它对齐 Linux x86-64 系统调用接口。文件系统、块设备、终端、管道、文件描述符与网络迁到 ring 3 服务。用户态链接 musl。内核保留调度、虚拟内存、IPC、中断路由与系统调用入口。服务之间通过基于句柄的能力 IPC 通信。线程共享地址空间，在 futex 上阻塞。本文描述内核原语、IPC 机制、服务路由器、线程与各服务的实现。

## 1. 设计

单体内核把每个子系统放进 ring 0。文件系统的一个缺陷会破坏调度状态或页表。纯微内核把驱动与服务全移到 ring 3，代价是重做接口与设备访问。HanOS 走中间路线。

- 对齐 Linux x86-64 系统调用接口。POSIX 调用沿用 Linux 编号。HanOS 独有调用用 `0x400` 以上。出错时 `RAX` 返回负 errno。`O_*` 与 `MAP_*` 用 Linux 取值。musl 只需薄后端。
- 子系统接口有界、状态能放进 ring 3 时，才迁到服务。
- 内核只留调度、地址空间、IPC、中断与引导加载。

内核从启动 initrd 镜像拉起首批服务。内核不在 ring 0 建文件系统。VFS 服务注册后，所有文件路径都走服务。

最终内核负责地址空间、进程创建、IPC 与中断；一组协作式服务负责文件描述符、文件系统状态、管道、控制台、输入设备与网络。

## 2. 系统总览

系统分三层。

- 内核运行在 ring 0。它提供调度、虚拟内存与内核对象模型。它还提供 IPC endpoint、中断对象、服务路由器与系统调用表。
- 服务作为普通进程运行在 ring 3。每个服务负责一个资源域。域含帧缓冲、PS/2 与串口输入、终端。也含 ATA 磁盘、FAT32 卷、initrd 命名空间。还含管道、每进程文件描述符表与网络。
- 用户程序运行在 ring 3，链接 musl；由 musl 发起系统调用。

服务用服务 id 寻址。内核路由器把服务 id 映射到负责服务的 endpoint。

| 服务 | id | 负责方 |
|------|----|--------|
| SVC_MM | 0 | 内核（内存） |
| SVC_FS | 1 | `/bin/vfs` |
| SVC_PROC | 2 | `/bin/process` |
| SVC_NET | 3 | `/bin/net` |
| SVC_MISC | 4 | 预留 |
| SVC_PIPE | 5 | `/bin/pipe` |
| SVC_TTY | 6 | `/bin/tty` |
| SVC_FAT | 7 | `/bin/fat32` |

## 3. 内核原语

### 3.1 特权模型与启动

Limine 把内核 ELF、initrd 与配置读入内存，再跳到 `kmain`。内核依次拉起串口、ACPI 表、本地 APIC、HPET、SMP 核、页表与内存分配器。内核记录 initrd 镜像，按需读取 ACPI 表。内核不在 ring 0 建文件系统。随后内核创建 `kshell`，它启动各服务，最后 exec shell 程序。

服务由 `sched_execve` 创建，并收到一块 `bootinfo_t`。该结构列出服务所需 endpoint 的句柄、授予的中断线、I/O 端口范围，以及帧缓冲、initrd 或网卡映射。服务在做任何事之前先用 `bootinfo` 系统调用读取它。

### 3.2 内核对象与句柄

endpoint、内存对象与中断对象都是 `kernel_object_t`。头部保存类型、原子引用计数、实现指针与销毁函数。最后一个引用释放时回收对象。

用户态只能通过每进程句柄表里的句柄访问对象。句柄打包 16 位槽索引与 16 位代数。每次分配代数自增，回收的槽不会与过期句柄混淆。每个槽还保存访问权限。

- `handle_alloc` 找空槽、推进代数并加一个引用。
- `handle_get` 校验索引、代数与请求的权限。
- `handle_close` 清空槽并减一个引用。
- `handle_dup` 用相同权限复制句柄。

消息在 `xfer[]` 中携带句柄时，句柄在进程间移动。内核从发送方句柄表取出对象，以受限权限移入接收方。第 4 节描述转移。

### 3.3 endpoint 与消息传递

endpoint 是保存 64 条消息 FIFO 的内核对象。消息含一个 tag、六个内联字与最多两个移入的句柄。内联字承载小负载；大块数据放进 `xfer[]` 的内存对象。

- `ipc_send` 入队一条消息并唤醒接收方。
- `ipc_recv` 阻塞到有消息。
- `ipc_recv_timeout` 带截止时间阻塞，超时返回。

唤醒是显式的。`sched_wake_key` 把睡眠进程置为就绪；进程在别的核上时，向该核发送重调度 IPI。没有该 IPI，唤醒后的进程要等目标核的定时器 tick。第 4 节给出完整机制。

### 3.4 服务路由器

路由器按服务 id 保存 endpoint 指针与所有者 pid。服务启动后注册自己的 endpoint。需要服务的系统调用调用 `router_forward`：创建应答 endpoint，放进请求，发出请求，并在应答上阻塞。服务 id 没有 endpoint 时，`router_lookup` 返回 NULL，调用方报告服务不可达。4.8 节描述路由器。

### 3.5 内存对象与用户拷贝安全

内存对象（`memobj_t`）是一组带引用计数的物理页。内核为大块传输创建它，把句柄放进 `xfer[1]`；服务用 `mem_map` 映射、拷入或拷出、再 `mem_unmap`。内核不把裸用户指针交给服务。

内核内部在进程地址空间与内核之间拷贝时用 uaccess 辅助函数。每条可能出错的读写指令在异常表（`__ex_table`）里对应一条记录。缺页处理程序查出出错指令并跳到它的修复标号，坏用户指针返回 `-EFAULT`，内核不致命。用户态异常不在 uaccess 修复表内，按信号（SIGSEGV、SIGILL、SIGFPE、SIGBUS）终止进程组。核心继续运行。内核态异常无修复，仍然 panic。

### 3.6 调度

每个核有自己的运行队列与当前进程。1 ms 的 APIC 定时器驱动时间片；每个核把上下文切换入口装到自己的定时器向量上。阻塞的进程带唤醒截止时间入队。切换代码先提升到期睡眠者为就绪，再选下一个进程。一直可运行的进程饿不死定时睡眠者。唤醒走 4.4 节的重调度 IPI。

### 3.7 内存布局

- 内核直接映射只覆盖 RAM。`vmm_init` 映射可用区、引导器与 ACPI 可回收区、内核镜像与帧缓冲，不映射 MMIO 空洞。
- ACPI 表放在保留内存。`acpi_init` 读表前先映射该表。
- 用户程序链接在 `0x0000400000000000`，内核与用户映射不再共用 HHDM 区间。
- process 服务保存每 pid 文件描述符表；内核按需把 fd 解析为服务描述符。

### 3.8 线程与 futex

线程是一个 `process_t`，与线程组共享地址空间。带 `CLONE_VM` 的 `clone` 创建它。内核给地址空间计数（`addrspace_t.refs`），映射表也存在地址空间里，最后一个线程负责释放。线程携带线程组 id `tgid`；组长的 `pid == tgid`。

`process_clone` 分配内核栈，并在其上构造子线程的用户态返回帧。系统调用入口跑在用户栈上，线程不能复用父栈。帧把 `rax` 置 0，`rip` 指向 `syscall` 之后，`rsp` 指向 `clone` 传入的栈。`CLONE_SETTLS` 设置子线程 `fs_base`。`CLONE_PARENT_SETTID` 与 `CLONE_CHILD_SETTID` 写入 tid。`CLONE_CHILD_CLEARTID` 记下退出时要清零并唤醒的地址。

`exit` 结束一个线程。`exit_group` 把本组其余线程标死，交给 idle 回收者释放，再退出调用者。

futex 阻塞在一个用户字上。`k_futex_wait` 武装等待键后再读该字，窗口内到达的唤醒不会被漏掉。`k_futex_wake` 最多唤醒 `nr` 个等待者。二者都建立在 IPC 等待键机制上，唤醒会向等待者所在核发重调度 IPI。

内核按 `tgid` 索引文件描述符调用，因此同组线程共享组长的 fd 表。

### 3.9 信号

系统调用入口把用户寄存器帧保存在用户栈。处理函数返回后，内核检查当前进程的信号状态。若有未屏蔽的待处理信号且装了处理函数，内核在用户栈压入信号帧，把返回 RIP 指向处理函数，用第一个参数寄存器传出信号号，再经 `sysret` 返回。动作自带的恢复例程执行 `rt_sigreturn`；它读回该帧，恢复被打断的寄存器组，继续原系统调用。系统调用结果保存在帧里。

`kill`、`tkill`、`tgkill` 排入信号并唤醒目标。未装处理函数时内核执行默认动作：忽略，或终止进程。处理函数执行期间屏蔽自身信号，`SA_NODEFER` 除外，并叠加动作掩码。`SIGKILL` 绕过掩码与处理函数。

帧位于 `kernel/proc/signal.c`。`signal_deliver` 显式接收被打断的 RIP 与 RFLAGS。系统调用路径经 `kernel/proc/syscall.c` 的 `syscall_post` 传 `rcx`/`r11`。中断路径传 `rip`/`rflags`。APIC 定时抢占入口 `enter_context_switch` 在选下一个进程前调用同一投递。进程在用户态自旋时，信号在下一次定时 tick 落地。该路径上的默认动作把进程标记为死亡。调度器丢弃它，空闲回收线程记录退出，关中断时不能走进程服务往返。

内核保存 Linux 等待状态。正常退出把低字节左移存放。信号致死把信号号放低位，核心转储置 `0x80`。父进程的 `wait` 返回该状态，`WIFEXITED`/`WEXITSTATUS` 与 `WIFSIGNALED`/`WTERMSIG` 据此译码。

### 3.10 中断控制器

内核启用本地 APIC，并编程 I/O APIC，让设备线在物理机上到达 CPU。它解析 MADT 的中断源覆盖项，映射第一个 I/O APIC。每条启用的 ISA 线对应一个重定向表项，把与 PIC 兼容的向量（`0x20 + IRQ`）送往引导核。有 I/O APIC 时，`irq_clear_mask` 走 I/O APIC，同时把 8259 线屏蔽；没有 I/O APIC 的机器仍走 8259。级联线 IRQ2 不改路由，因为覆盖项可能把它的 GSI 映射到别的线。中断返回时，若中断由 I/O APIC 投递则发本地 APIC EOI，否则发 PIC EOI。

## 4. IPC 机制

客户端与服务之间的每个请求都以消息形式走在 endpoint 上。用户内存指针不跨边界。本节描述消息、endpoint、句柄转移、应答模式、阻塞、挂起应答、大块数据、路由器与中断投递。

### 4.1 endpoint

endpoint 是保存自旋锁与 64 条消息 FIFO 的内核对象。

```c
#define IPC_WORDS 6
#define IPC_QUEUE_LEN 64

struct endpoint {
    kernel_object_t obj;
    spinlock_t lock;
    ipc_queue_entry_t msgs[IPC_QUEUE_LEN];
    uint32_t head;
    uint32_t tail;
    uint32_t count;
};
```

队列是环形缓冲。`ipc_send` 在 `tail` 追加；`ipc_try_recv` 在 `head` 弹出。自旋锁跨核保护 head、tail 与 count。队列满时拒绝发送。

### 4.2 消息

```c
#define IPC_WORDS 6

struct ipc_msg {
    uint64_t tag;
    uint64_t words[IPC_WORDS];
    handle_t xfer[2];
    uint8_t  xfer_count;
};
```

- tag 命名请求。每个服务自定 tag 空间（`VFS_*`、`PIPE_*`、`PROC_*`、`NET_*`、`TTY_*`、`FAT_*`）。
- 六个内联字承载小输入与小回复。回复把状态或负 errno 放进 `words[0]`。
- `xfer[]` 随消息移动最多两个句柄。路径或大块缓冲走内存对象；应答 endpoint 走 endpoint 对象。
- `xfer_count` 指明发送方设置了几个 `xfer[]` 项。接收方在拷贝后读取。

### 4.3 句柄转移

句柄是进程对内核对象的引用。表打包 16 位槽与 16 位代数；回收后代数改变，过期句柄不会命中。

```c
handle_t handle_alloc(handle_table_t *ht, kernel_object_t *o, uint32_t rights);
kernel_object_t *handle_get(handle_table_t *ht, handle_t h, uint32_t rights);
int handle_close(handle_table_t *ht, handle_t h);
```

权限为 `READ`、`WRITE`、`SEND`、`RECV`、`MAP` 与 `TRANSFER`。句柄带 `TRANSFER` 时，发送方才交出对象。内核把对象移出发送方句柄表，放进消息，再以受限权限移入接收方：

- 内存对象 → `READ | WRITE | MAP`
- endpoint → `SEND | RECV`
- 中断对象 → `READ`

转移是移动语义：消息入队时，发送方句柄关闭。收到的句柄不带 `TRANSFER`，服务无法把它继续转发。

### 4.4 发送、接收与唤醒

```c
int ipc_send(endpoint_t *ep, const ipc_msg_t *msg);
int ipc_recv(endpoint_t *ep, ipc_msg_t *msg);
int ipc_recv_timeout(endpoint_t *ep, ipc_msg_t *msg, time_t ms);
int ipc_recv_nb(endpoint_t *ep, ipc_msg_t *msg);
```

- `ipc_send` 入队后调用 `sched_wake_key(ep)`。
- `ipc_recv` 经 `sched_wait_key_begin` 与 `sched_wait_key_commit` 阻塞。等待键是 endpoint 指针。
- `sched_wake_key` 把睡眠进程置为就绪。该进程在别的核上时，唤醒方向其核发送重调度 IPI，接收方不必等定时器 tick。

唤醒不丢失。`sched_wait_key_begin` 在检查队列前武装进程；`sched_wait_key_commit` 仅在队列为空时睡眠。窗口期内到达的唤醒把进程置为就绪，接收循环再观察到它。

### 4.5 应答模式

需要答案的调用方用 `router_forward`。内核创建临时应答 endpoint，放进请求。

1. 创建应答 endpoint。
2. 把应答 endpoint 放进 `xfer[0]`；请求自身句柄接在 `xfer[1..]`。
3. 把请求发到服务 endpoint。
4. 在应答 endpoint 上带截止时间阻塞。
5. 释放应答 endpoint 并返回应答消息。

服务从 `xfer[0]` 读应答 endpoint，处理请求，在它上面发送应答。服务随后关闭应答 endpoint；内核返回时释放自己的引用。应答路径单向：服务不需要调用方的 endpoint。

### 4.6 挂起应答

无法立刻答复的服务保留应答 endpoint，稍后答复。应答 endpoint 是句柄，持有它即可。系统据此阻塞，不轮询。

- tty 读无键：服务保存应答 endpoint 与长度，下一个键到达时答复。
- 管道读无数据，或写无空间：服务保存应答 endpoint 与内存对象，对端搬数据或关闭时答复。
- 数据报或流接收无数据：服务保存应答 endpoint 与缓冲，报文到达时答复。
- `accept` 无待处理连接：服务保存应答 endpoint，连接完成时答复。

每个服务至多持有少量挂起请求，内存开销有界。

### 4.7 大块数据

大块路径或文件缓冲不走六个内联字。调用方创建内存对象（`mem_alloc`）、映射、填充，把句柄放进 `xfer[1]`。服务用 `mem_map` 映射、拷入或拷出、再解除映射。内核不把用户指针跨边界传递。

VFS 服务用一个缓冲承载路径与 stat 结果：路径在偏移 0，结果在 `VFS_IO_DATA_OFF`。管道服务把 32 字节内的传输内联，超过则用内存对象。网络服务经内存对象搬运报文。

### 4.8 服务路由器

路由器是从服务 id 到 endpoint 的目录。

```c
typedef enum {
    SVC_MM, SVC_FS, SVC_PROC, SVC_NET, SVC_MISC, SVC_PIPE, SVC_TTY, SVC_FAT,
    SVC_COUNT
} service_id_t;

void router_register(service_id_t id, endpoint_t *ep, pid_t owner);
endpoint_t *router_lookup(service_id_t id);
bool router_forward(service_id_t id, const ipc_msg_t *req, ipc_msg_t *rep);
bool router_forward_timeout(service_id_t id, const ipc_msg_t *req,
                            ipc_msg_t *rep, time_t timeout_ms);
```

服务启动后注册 endpoint。系统调用查服务并转发。服务未注册时 `router_lookup` 返回 NULL，调用方报告服务不可达。ring 0 只留 initrd 读取器与内核设备驱动。

### 4.9 中断即消息

内核为一条中断线创建中断对象（`OBJ_IRQ`）。`irq_bind` 绑定 endpoint。线触发时，`irq_deliver` 向该 endpoint 发 `IRQ_NOTIFY_TAG` 消息；ISR 路径对已绑定的线跳过内核处理函数。驱动用 `irq_ack` 应答。硬件事件与系统调用走同一条消息路径。

### 4.10 一次文件读的全程

1. musl 以 `(fd, buf, len)` 发起 Linux `read` 系统调用（0）。
2. 内核经 process 服务把 `fd` 解析为 `(svc, server_fd)`。
3. 内核为 `buf` 创建内存对象，组装 `VFS_READ`。
4. `router_forward` 移入应答 endpoint 与内存对象，发到 FS endpoint。
5. VFS 服务映射对象、拷贝字节、回复计数，并关闭应答 endpoint。
6. 内核把对象拷回 `buf`，返回计数。

## 5. 系统服务

每个服务是一个进程。它从 `service_ep` 收消息，处理请求，再在内核移入 `xfer[0]` 的 endpoint 上回复。大块载荷走 `xfer[1]` 的内存对象。

### 5.1 console

console 服务负责帧缓冲。内核授予它带 write-combining 属性的 scan-out 映射，以及一个收控制台字节的 endpoint。服务保存一块后台缓冲，用共享的 gohufont 字形渲染文本。它记录脏行，只把这些行拷到帧缓冲。

内核与 tty 服务发送 `CONSOLE_WRITE_TAG`。服务把字节流喂给一个 libvterm `VTerm`,把 `VTermScreen` 单元渲染进后台缓冲,于是完整 VT 集合可用:光标定位、擦除、滚动、SGR 颜色、备用屏、光标隐藏与显示。网格占屏幕中间五分之四,边距放静态的绿色字符雨:二进制与十六进制数字,头部亮、尾部暗,多数列留空。GPU 版会把字符流做成动画,这版是静态的。它把裸 LF 映射为 CR-NL,因为 tty 只发 NL,而终端遇 LF 只下移不回首列。空闲时光标块闪烁,只有受损行拷进帧缓冲。服务从清屏开始,不继承启动画面。

### 5.2 input

input 服务负责 PS/2 控制器、其中断线与 COM1。内核授予它 IRQ1（键盘）、IRQ12（鼠标）、IRQ4（串口），PS/2 端口 `0x60` 与 `0x64`，以及 COM1 范围 `0x3F8` 到 `0x3FF`。端口访问统一经 `ioport_access` 系统调用，并带范围检查。

收到中断通知后，服务排空 PS/2 控制器。状态字节标明数据来自键盘还是鼠标。键盘扫描码经共享的 keycode 表解码；鼠标按三字节包取增量。服务同时排空 COM1 字节，没有 PS/2 键盘时串口控制台可用。解码后的按键与鼠标增量以 `INPUT_KEY_TAG` 与 `INPUT_MOUSE_TAG` 交给内核。

内核用精简 xHCI 驱动枚举 USB，代码在 `kernel/device/usb/xhci.c`。启动时它复位控制器与每个端口，分配地址，读取设备与配置描述符。它扫描所有端口，把每个设备按 厂家:产品 与接口类打印。USB 网卡或无线网卡即便没有驱动，也出现在启动日志里。HID 开机键盘或指针注册中断端点，解码后并入同一条按键通路。

### 5.3 tty

tty 服务负责 `/dev/tty`。它缓存按键，并把按键回显到控制台。内核把每个解码按键以 `TTY_KEY` 转发给服务。

读是事件驱动的，按行缓冲。`TTY_READ` 有时返回现成字节。`TTY_READ` 无键时挂起：服务保存应答 endpoint，在整行或请求长度到齐时答复。内核在 `tty_server_read` 里阻塞等该应答，不轮询。请求字节少的读保持非缓冲。

对 fd 0 的 `poll` 经 `TTY_POLL` 问服务待读键数。无键时它阻塞在一个内核等待键上，tty 中转收到键时唤醒它，于是轮询者睡眠而非自旋。`select` 与 `pselect6` 共用就绪判定。管道与套接字的就绪查询见 5.7、5.9。

对标准描述符的 `TCGETS`/`TCSETS` ioctl 到达 tty 服务。清掉 `ECHO` 与 `ICANON` 进入 raw 模式：不再回显按键，有待读字节时立即答复挂起的读，全屏程序不必等换行即可逐键读取。行模式下它把退格回显为「退格-空格-退格」，被删单元随之消失。shell 退出时恢复保存的模式。

控制台服务从帧缓冲算出网格，用 `TIOCSWINSZ` ioctl 存下行数与列数。`TIOCGWINSZ` ioctl 返回该尺寸，全屏程序按真实网格摆放并居中。服务设置之前尺寸为 25x80。

### 5.4 block

block 服务负责 AHCI（SATA）控制器。内核像给网卡一样授予它 ABAR MMIO 窗口与一段物理连续 DMA 区。它应答 `BLOCK_GET_INFO`、`BLOCK_READ` 与 `BLOCK_WRITE`。读写把扇区放进 `xfer[1]` 的内存对象，用 48 位 LBA 寻址。服务在 DMA 区里建命令列表、命令表与接收 FIS 区，发 `IDENTIFY DEVICE` 与 `READ`/`WRITE DMA EXT`，并轮询端口。它经自己的 DMA 缓冲拷贝，因为只有那段内存有已知物理地址，也不解引用客户端指针。有界轮询让设备缺失时快速失败。

服务启动时解析 GPT，记录每个已用分区的 LBA 范围。`BLOCK_GET_PART` 按索引返回分区的起始与扇区数，文件系统服务据此挂载分区，不必自己解析分区表。

### 5.5 fat32

FAT32 服务是 block 服务的只读客户端。启动时它读引导扇区、校验 FAT32 签名、记录几何。它按 8.3 名遍历目录，并跟随文件的簇链。

描述符由服务保存。`FAT_OPEN` 解析路径并返回描述符；`FAT_READ` 推进偏移；`FAT_SEEK` 重定位；`FAT_FSTAT` 报告大小与目录标志；`FAT_READDIR` 列一个条目。`FAT_STAT` 不打开就报告路径。路径放在缓冲内存对象的偏移 0，文件数据放在 `VFS_IO_DATA_OFF`。

### 5.6 vfs

VFS 服务提供 initrd 命名空间。启动时它解析内核只读映射的 ustar 归档，建起文件、目录、大小、模式与修改时间的索引。它还保存少量运行时创建的 RAM 文件。

服务负责路径解析与 FAT 挂载。路径请求带进程工作目录与路径，服务拼接并归一。结果落在 `/fat` 下时，服务写回挂载内相对路径，回复 `VFS_REDIRECT_FAT`；内核随后把请求发给 FAT 服务。内核不拼接路径。

协议覆盖 `VFS_OPENAT`、`VFS_READ`、`VFS_WRITE`、`VFS_SEEK`、`VFS_CLOSE`、`VFS_READDIR`、`VFS_FSTAT`、`VFS_FSTATAT`、`VFS_FACCESSAT`、`VFS_UNLINK`、`VFS_MKDIRAT`、`VFS_SYMLINKAT`、`VFS_RENAMEAT`、`VFS_READLINK` 与 `VFS_FD_FORK`。带路径或数据缓冲的请求把它放进 `xfer[1]` 的内存对象；服务映射、读取或填充、再解除映射。`VFS_FD_FORK` 增加一个引用，fork 的子进程共享打开文件描述。

`VFS_MKDIRAT` 建运行时目录。`VFS_SYMLINKAT` 建运行时符号链接并保存目标。`VFS_READLINK` 返回该目标。`VFS_RENAMEAT` 重命名运行时条目，并改写被重命名目录子项的路径前缀。三者遇到只读 FAT 挂载下的路径时回应 `VFS_REDIRECT_FAT`，内核报 `EROFS`。

### 5.7 pipe

pipe 服务保存少量字节流管道。`PIPE_CREATE` 返回读端与写端。读写以最多 32 字节内联传输，或经 `xfer[1]` 的内存对象。读端无数据而写端未关，或写端无空间，服务挂起请求：保存应答 endpoint 与对象，对端搬数据或关闭时答复。写端关闭后读返回 0。`PIPE_POLL` 报告端是否可读（有数据或 EOF）与可写。`VFS_FD_FORK` 递增端引用计数，fork 的子进程共享管道。

### 5.8 process

process 服务保存每进程文件描述符表与进程树。每个 fd 请求给出 pid 与 fd。

- `PROC_FD_OPEN` 注册指向某服务与其服务 fd 的描述符；`PROC_FD_GET` 解析它；`PROC_FD_CLOSE` 删除并返回服务 fd；`PROC_FD_DUP` 复制它；`PROC_FD_FORK` 为子进程克隆父表；`PROC_FD_EXIT` 关闭退出进程的描述符。
- `PROC_FD_FCNTL` 读写 close-on-exec 标志。exec 关闭子进程里标记的描述符。
- `PROC_EXEC` 加载 ELF：内核把路径、工作目录、argv 与镜像打包进一个内存对象；服务映射各段、建栈、启动子进程。
- `PROC_EXIT` 记录退出状态；`PROC_WAIT` 返回已死子进程，子进程存活时阻塞，否则报 `ECHILD`。进程退出且其子进程全部退出后，才算已退出。

内核经 process 服务把 fd 解析为每 CPU 临时描述符。内核按 `tgid` 索引 fd 调用，同组线程共享组长那张表。`dup3` 复制 fd 0、1、2 时用 tty 服务做后端，因为标准描述符留内核；副本上的读写直达 tty。套接字也注册进程 fd，`poll`、`select`、`epoll` 经同一解析到达它。`eventfd` 与 `epoll` 对象留内核，用内核服务 id 注册进程 fd；对它们的 `read`、`write`、`close` 留在 ring 0。

`poll` 或 `epoll_wait` 无法立刻返回时装备进程轮询键。它把键注册到每个被轮询的管道端、内核 eventfd 或套接字。`PIPE_POLL_WAIT` 与 `NET_POLL_WAIT` 带上该键与请求事件位。只有描述符满足时服务才唤醒该键。内核在装备后复查就绪，消除竞态。tty 保留自己的键。`SYSCALL_POLL_WAKE` 供服务唤醒已停靠的轮询者。

### 5.9 net

net 服务负责套接字层与网卡。它提供 AF_INET 数据报与流套接字。

- 数据报：发往回环地址的 `sendto` 在服务内投递；真实地址先解析 ARP，再发 UDP/IP/以太网帧。收到的数据报按绑定套接字匹配。
- 流：`connect` 走 SYN/SYN-ACK/ACK 握手；`send` 发 PSH 段；`recv` 缓冲到达数据；`close` 发 FIN。`listen` 与 `accept` 接受入连接。
- 流可靠性：每连接一个发送队列，按超时退避重传最旧段。乱序段缓存到缺口填上。通告窗口跟随接收空闲空间。FIN/ACK 握手关闭连接，CLOSE_WAIT 下已缓冲数据仍可读。
- 收包校验：服务校验 IPv4、TCP 与 UDP 校验和，坏帧丢弃。
- 套接字选项：`setsockopt`/`getsockopt` 支持 `SO_REUSEADDR`、`SO_RCVTIMEO`、`SOCK_NONBLOCK`；超时或非阻塞读返回 EAGAIN。
- 地址配置：DHCP 客户端取地址、网关与 DNS 服务器；解析器向该服务器发 A 记录查询，HanOS resolve 系统调用返回地址。
- `NET_POLL` 报告套接字是否有待读数据报、待接受连接或已关闭的流（可读），以及流是否已建立（可写）。套接字注册进程 fd，`poll` 与 `epoll` 到达它；状态匹配时服务唤醒已注册的轮询键。
- 服务驱动 e1000e：内核授予 MMIO（内存映射 I/O）基址寄存器（BAR）与一段物理连续 DMA（直接内存访问）区；服务配置环形队列、读取 MAC。内核把网卡的 PCI 中断线绑到服务 endpoint，经 I/O APIC 路由（低有效、边沿）。服务收到通知后排空环形队列，轮询作为兜底。
- ARP 缓存放地址查询。RX 分发器处理 ARP、ICMP 回显、UDP 与 TCP。服务启动时 ping 网关。

### 5.10 ext2

ext2 服务是 block 服务的只读客户端。它经 `BLOCK_GET_PART` 挂载索引 1 的分区，读字节偏移 1024 处的超级块、块组描述符与 inode 表，解析直接、单级、双级与三级间接块映射。它应答 `EXT2_OPEN`、`EXT2_READ`、`EXT2_READDIR`、`EXT2_STAT`、`EXT2_CLOSE` 与 `EXT2_SEEK`。名字区分大小写，每个 inode 带模式与属主，`STAT` 返回模式、属主、链接数与修改时间，`ls -l` 显示真实值。VFS 挂载表把 `/data` 指到该卷，`/data` 下的路径转发给 ext2 服务，`/fat` 到 FAT 服务同理。分区放应用与游戏，它们从磁盘装入，不占 initrd；shell 先在 `/bin` 找命令，再到 `/data/bin`。`exec` 经同一转发从该卷装入镜像，`vfs_load_file` 按大块读取。

## 6. 启动顺序

`kshell` 按依赖顺序启动服务。

1. console，让内核输出有去处。
2. input 与 tty，让终端可用。
3. block，其后 fat32 作为其客户端。
4. vfs，提供 initrd 命名空间。
5. pipe。
6. process，负责文件描述符与进程树。
7. net，负责网卡。

随后 exec `/bin/init`，init 再 exec shell。每次启动把服务 endpoint 注册到路由器，并用 ping 探测服务。

内核用 `vfs_load_file` 从 initrd 镜像加载首批服务。VFS 服务注册后，所有文件路径都走服务。内核不留文件系统，只留 initrd 读取器。

## 7. 局限

- 每个服务只有一个实例；服务崩溃会让该域停止。
- FAT32 服务只读，且只处理 8.3 名。
- block 服务用 AHCI（SATA）加 DMA。不再用传统 ATA PIO 端口。
- SVC_MM 与 SVC_MISC 预留未用。
- TCP 无拥塞控制；发送队列容纳四段，接收窗口跟随空闲缓冲。
- 网卡经 PCI 中断线唤醒服务；有界轮询作为兜底。
- VFS 运行时文件放在 RAM，不持久。
- xHCI 驱动扫描所有端口，但只驱动 HID 开机键盘与指针。它只用一个控制器，跳过 64 字节上下文的控制器。USB 网卡或无线网卡按 厂家:产品 与类打印，不驱动。
- 定时路径进入的信号处理函数经 `rt_sigreturn` 与 `sysret` 返回，被打断的 `rcx` 不恢复。系统调用 ABI 本就破坏 `rcx`；只有用户代码在被中断时把活值放在 `rcx` 才会察觉。
- `mkdirat`、`symlinkat`、`renameat` 只建运行时条目；initrd 与 FAT 挂载保持只读。
- 一批系统调用仍返回 `ENOSYS`，例如 `signalfd`、`getppid`、`chmod`，以及 FAT 挂载上的 `symlinkat`。

## 8. 结论

HanOS 把地址空间、进程、IPC 与中断留在内核，把文件系统、块设备、终端、管道、文件描述符与网络状态移进 ring 3 服务。基于句柄的能力模型、带句柄转移的 endpoint IPC、服务路由器、挂起应答，以及承载大块数据的内存对象，共同组成一个可用的混合微内核，同时保持 POSIX 系统调用接口不变。

## 参考资料

- 源码：`kernel/`、`userspace/`、`musl/`。
- 内核 C 库：`kernel/lib/`。
- 共享线协议头：`include/protocol.h`、`include/bootinfo.h`、`include/syscall_nr.h`。
- 用户态运行时：`userspace/runtime/`、`userspace/include/`。
- musl 移植：`musl/syscall_arch.h`、`musl/__set_thread_area.s`、`musl/linker.ld`、`musl/build.sh`。
- 内核 IPC 与对象：`kernel/ipc/{ipc.c,object.c,irq.c}`。
- 路由器：`kernel/router/router.c`。
- 服务：`userspace/servers/`。
