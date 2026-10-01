# HanOS：一种混合微内核的架构

*本文按当前实现描述。实现变化时同步修订。*

## 摘要

HanOS 是面向 x86-64、用 C 编写的操作系统。它保留经典 POSIX 系统调用接口，同时把文件系统、块设备、终端、管道与文件描述符子系统迁到 ring 3 服务。内核保留调度、虚拟内存、IPC、中断路由与系统调用入口。服务之间通过一套基于句柄的能力 IPC 通信。本文描述内核原语、服务路由器，以及每个服务的实现。

## 1. 为什么采用混合微内核

单体内核把每个子系统放进 ring 0。文件系统的一个缺陷会破坏调度状态或页表。纯微内核把驱动与服务全移到 ring 3，代价是重做接口与设备访问。HanOS 走中间路线。

- 保留用户态已在用的系统调用编号与名称。
- 子系统接口有界、状态能放进 ring 3 时，才迁到服务。
- 同时保留同一接口的内核实现。服务已注册时走服务，否则走内核路径。迁移可以按服务推进，系统在任何时候都能启动。

最终内核仍负责地址空间、进程创建、IPC 与中断；一组协作式服务负责文件描述符、文件系统状态、管道、控制台与输入设备。

## 2. 系统总览

系统分三层。

- 内核运行在 ring 0。它提供调度、虚拟内存、内核对象与句柄模型、IPC endpoint、中断对象、服务路由器与系统调用表。
- 服务作为普通进程运行在 ring 3。每个服务负责一个资源域：帧缓冲、PS/2 与串口输入、终端、ATA 磁盘、FAT32 卷、initrd VFS、管道，或每进程文件描述符表。
- 用户程序运行在 ring 3，通过 libc 发起系统调用，并带范围检查。

服务用服务 id 寻址。内核路由器把服务 id 映射到负责服务的 endpoint。

| 服务 | id | 负责方 |
|------|----|--------|
| SVC_MM | 0 | 内核（内存） |
| SVC_FS | 1 | `/bin/vfs` |
| SVC_PROC | 2 | `/bin/process` |
| SVC_NET | 3 | 预留 |
| SVC_MISC | 4 | 预留 |
| SVC_PIPE | 5 | `/bin/pipe` |
| SVC_TTY | 6 | `/bin/tty` |
| SVC_FAT | 7 | `/bin/fat32` |

## 3. 内核原语

### 3.1 特权模型与启动

Limine 把内核 ELF、initrd 与配置读入内存，再跳到 `kmain`。内核依次拉起串口、ACPI 表、本地 APIC、HPET、SMP 核、页表与内存分配器。它在 initrd 上建起内核内 ramfs，然后创建 `kshell`。`kshell` 是内核进程，负责启动各服务，最后 exec shell 程序。

服务由 `sched_execve` 创建，并收到一块 `bootinfo_t`。该结构列出服务所需 endpoint 的句柄、授予的中断线、I/O 端口范围，以及帧缓冲或 initrd 映射。服务在做任何事之前先用 `bootinfo` 系统调用读取它。

### 3.2 内核对象与句柄

endpoint 与内存对象都是 `kernel_object_t`。头部保存类型、原子引用计数、实现指针与销毁函数。最后一个引用释放时回收对象。

用户态只能通过每进程句柄表里的句柄访问对象。句柄打包 16 位槽索引与 16 位代数。每次分配代数自增，回收的槽不会与过期句柄混淆。每个槽还保存访问权限。

- `handle_alloc` 找空槽、推进代数并加一个引用。
- `handle_get` 校验索引、代数与请求的权限。
- `handle_close` 清空槽并减一个引用。
- `handle_dup` 用相同权限复制句柄。

消息在 `xfer[]` 中携带句柄时，句柄在进程间移动。路由器从发送方句柄表取出对象，以受限权限移入接收方。

### 3.3 endpoint 与消息传递

endpoint 是保存 64 条消息 FIFO 的内核对象。消息含一个 tag、六个内联字与最多两个移入的句柄。内联字承载小负载；大块数据放进 `xfer[]` 的内存对象。

- `ipc_send` 入队一条消息并唤醒接收方。
- `ipc_recv` 阻塞到有消息。
- `ipc_recv_timeout` 带截止时间阻塞，超时返回。

唤醒是显式的。`sched_wake_key` 把睡眠进程置为就绪；进程在别的核上时，向该核发送重调度 IPI。没有该 IPI，唤醒后的进程要等目标核的定时器 tick。

### 3.4 服务路由器

路由器按服务 id 保存 endpoint 指针与所有者 pid。服务启动后注册自己的 endpoint。需要服务的系统调用调用 `router_forward`：

1. 创建临时应答 endpoint。
2. 把应答 endpoint 放进 `xfer[0]`，请求自身句柄接在其后。
3. 把请求发到服务 endpoint。
4. 在应答 endpoint 上带超时阻塞。
5. 释放应答 endpoint 并返回应答。

服务方在应答 endpoint 上回复并关闭它。服务 id 还没有 endpoint 时，`router_lookup` 返回 NULL，调用方走内核实现。这条回退路径让系统在服务启动前、服务关闭时都能启动。

### 3.5 内存对象与用户拷贝安全

内存对象（`memobj_t`）是一组带引用计数的物理页。内核为大块传输创建它，把句柄放进 `xfer[1]`；服务用 `mem_map` 映射、拷入或拷出、再 `mem_unmap`。内核不把裸用户指针交给服务。

内核内部在进程地址空间与内核之间拷贝时用 uaccess 辅助函数。每条可能出错的读写指令在异常表（`__ex_table`）里对应一条记录。缺页处理程序查出出错指令并跳到它的修复标号，坏用户指针返回 `-EFAULT`，内核不致命。

### 3.6 调度

每个核有自己的运行队列与当前进程。1 ms 的 APIC 定时器驱动时间片；每个核把上下文切换入口装到自己的定时器向量上。阻塞的进程带唤醒截止时间入队。切换代码在选择下一个进程前先把到期的睡眠者提升为就绪，一直可运行的进程饿不死定时睡眠者。唤醒走 3.3 节的重调度 IPI。

## 4. 系统服务

每个服务是一个进程。它从 `service_ep` 收消息，处理请求，再在本内核移入 `xfer[0]` 的 endpoint 上回复。大块载荷走 `xfer[1]` 的内存对象。

### 4.1 console

console 服务负责帧缓冲。内核授予它带 write-combining 属性的 scan-out 映射，以及一个收控制台字节的 endpoint。服务保存一块后台缓冲，用共享的 gohufont 字形渲染文本。它记录脏行，只把这些行拷到帧缓冲。

内核与 tty 服务发送 `CONSOLE_WRITE_TAG`。服务解析少量 SGR 子集（颜色）与换行、回车、退格、制表符。队列空闲时它闪烁块状光标。服务从清屏开始，不继承启动画面。

### 4.2 input

input 服务负责 PS/2 控制器、其中断线与 COM1。内核授予它 IRQ1（键盘）、IRQ12（鼠标）、IRQ4（串口），PS/2 端口 `0x60` 与 `0x64`，以及 COM1 范围 `0x3F8` 到 `0x3FF`。端口访问统一经 `ioport_access` 系统调用，并带范围检查。

收到中断通知后，服务排空 PS/2 控制器。状态字节标明数据来自键盘还是鼠标。键盘扫描码经共享的 keycode 表解码；鼠标按三字节包取增量。服务同时排空 COM1 字节，没有 PS/2 键盘时串口控制台可用。解码后的按键与鼠标增量以 `INPUT_KEY_TAG` 与 `INPUT_MOUSE_TAG` 交给内核。

### 4.3 tty

tty 服务负责 `/dev/tty`。它缓存按键，并把按键回显到控制台。内核把每个解码按键以 `TTY_KEY` 转发给服务。

读是事件驱动的。`TTY_READ` 有时返回现成字节。`TTY_READ` 无键时挂起该请求：服务保存应答 endpoint，等下一个键到达再回复。内核在 `tty_server_read` 里阻塞等该应答，不再轮询。空闲 shell 不发请求，由应答的重调度 IPI 唤醒。

### 4.4 block

block 服务负责 ATA PIO 端口。它应答 `BLOCK_GET_INFO`、`BLOCK_READ` 与 `BLOCK_WRITE`。读写把扇区放进 `xfer[1]` 的内存对象，用 28 位 LBA 寻址。服务用 `IDENTIFY` 探测主盘、报告几何，也从不解引用客户端指针。有界轮询让设备缺失时快速失败。

### 4.5 fat32

FAT32 服务是 block 服务的只读客户端。启动时它读引导扇区、校验 FAT32 签名、记录几何。它按 8.3 名遍历目录，并跟随文件的簇链。

描述符由服务保存。`FAT_OPEN` 解析路径并返回描述符；`FAT_READ` 推进偏移；`FAT_SEEK` 重定位；`FAT_FSTAT` 报告大小与目录标志；`FAT_READDIR` 列一个条目。`FAT_STAT` 不打开就报告路径。路径放在缓冲内存对象的偏移 0，文件数据放在 `VFS_IO_DATA_OFF`。

### 4.6 vfs

VFS 服务提供 initrd。启动时它解析内核只读映射的 ustar 归档，建起文件、目录与大小的索引。它还保存少量运行时创建的 RAM 文件。

协议覆盖 `VFS_OPENAT`、`VFS_READ`、`VFS_WRITE`、`VFS_SEEK`、`VFS_CLOSE`、`VFS_READDIR`、`VFS_FSTAT`、`VFS_FSTATAT`、`VFS_FACCESSAT`、`VFS_UNLINK` 与 `VFS_FD_FORK`。带路径或数据缓冲的请求把它放进 `xfer[1]` 的内存对象；服务映射、读取或填充、再解除映射。`VFS_FD_FORK` 增加一个引用，fork 的子进程共享打开文件描述。

内核 VFS 层只保存命名服务文件的每 CPU 临时描述符；描述符由进程服务按 fd 解析。

### 4.7 pipe

pipe 服务保存少量字节流管道。`PIPE_CREATE` 返回读端与写端。读写以最多 32 字节内联传输，或经 `xfer[1]` 的内存对象。读端无数据而写端未关，或写端无空间，返回 `PIPE_EAGAIN`，内核重试。写端关闭后读返回 0。`VFS_FD_FORK` 递增端引用计数，fork 的子进程共享管道。

### 4.8 process

process 服务保存每进程文件描述符表。每个请求给出 pid 与 fd。`PROC_FD_OPEN` 注册一个指向某服务与其服务 fd 的描述符；`PROC_FD_GET` 解析它；`PROC_FD_CLOSE` 删除它并返回服务 fd，调用方据此关闭服务侧；`PROC_FD_DUP` 复制描述符；`PROC_FD_SEEK` 更新偏移；`PROC_FD_FORK` 为子进程克隆父表；`PROC_FD_EXIT` 关闭退出进程的描述符。

内核不再为每进程保存文件表。对服务支撑的 fd 做读、写、定位、关闭或取状态时，内核经进程服务解析 fd，落到每 CPU 临时描述符。标准输入输出重定向用同一张表：`dup3` 记录别名，fd 0、1、2 的读写有别名时用别名，否则用 tty。

## 5. 启动顺序与回退

`kshell` 按依赖顺序启动服务。

1. console，让内核输出有去处。
2. input 与 tty，让终端可用。
3. block，其后 fat32 作为其客户端。
4. vfs，提供 initrd。
5. pipe。
6. process，负责文件描述符。

随后 exec `/bin/init`，init 再 exec shell。每次启动把服务 endpoint 注册到路由器，并用 ping 探测服务。

服务注册前，或在 `kconfig.h` 中关闭服务时，路由器返回 NULL，系统调用走内核实现。内核保留 ramfs、块设备、管道与终端的内核路径，没有服务时系统也能启动并运行。

## 6. 局限

- 每个服务只有一个实例；服务崩溃会让该域停止。内核回退覆盖服务未启动的情况，不覆盖服务中途退出的情况。
- FAT32 服务只读，且只处理 8.3 名。
- block 服务用 ATA PIO 轮询，不用 DMA。
- NET 与 MISC 服务 id 预留未用。
- 设备服务用超时轮询队列；只有 tty 读路径是事件驱动的。

## 7. 结论

HanOS 把地址空间、进程、IPC 与中断留在内核，把文件系统、块设备、终端、管道与文件描述符状态移进 ring 3 服务。基于句柄的能力模型、带句柄转移的 endpoint IPC、带回退的服务路由器，以及承载大块数据的内存对象，共同组成一个可用的混合微内核，同时保持 POSIX 系统调用接口不变。

## 参考资料

- 源码：`kernel/`、`libc/`、`userspace/servers/`。
- 接口头文件：`libc/include/protocol.h`、`libc/include/bootinfo.h`、`libc/include/sysfunc.h`。
- 内核 IPC 与对象：`kernel/ipc/{ipc.c,object.c,irq.c}`。
- 路由器：`kernel/router/router.c`。
- 服务：`userspace/servers/`。
