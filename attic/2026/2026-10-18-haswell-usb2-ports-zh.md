# 2026-10-18：Haswell USB 2.0 存储与 xHCI 路由

## 结果

Haswell-ULT 笔记本从 U 盘启动。系统挂载 ext2 数据分区。游戏可以运行。启动摘要、`U-Disk` 行与磁盘行打印完。shell 随后进入。

## 现象

- 固件从 U 盘启动。内核看不到这颗盘。
- `dmesg usb` 显示 EHCI 无存储设备。xHCI 的 13 个端口全空。
- EHCI 端口 1 报 `CCS=1`，速度为全速。首个控制传输失败。

## 根因

Haswell 把蓝色端口的 USB2 引脚接到 EHCI。这颗 EHCI 没有 companion（`N_CC=0`）。根 Hub TT 拒绝全速设备的 split 事务。xHCI 持有 USB3 引脚。xHCI 看不到设备。

EHCI 驱动还有三处故障：

- 64 位控制器（`HCCPARAMS` bit 0）会读附录 B 的高缓冲指针。驱动用 32 字节 qTD 与 48 字节 QH。控制器把下一个 qTD 当成高指针。控制器触发 Host System Error。
- 批量队列头写死高速。设备是全速。
- 状态阶段 qTD 的缓冲指针为空。驱动在控制器退出 Halted 前读端口寄存器。

## 修复

### 把 USB2 路由到 xHCI

- 读 xHCI PCI 配置 0xd0 的 `USB2PR` 与 0xd4 的 `USB2PRM`。
- 清低 15 位路由位。按掩码置位。
- 扫描控制器前写回 0xd0。
- 写入限定 Intel 9c31、8c31、9cb1、8cb1。xHCI 原生处理 USB2。全速设备不再走 EHCI TT。

### xHCI

- `USBCMD.RS` 后等 `USBSTS.HCH` 清零。
- 给未供电端口置 `PORTSC.PP`。等待 500 ms。
- USB2 端口用 `PORTSC.PR` 复位。USB3 端口用 RxDetect。
- Address Device 从 PORTSC 报的速度起试。失败后再试更低速度。
- 启动控制器前做 BIOS 交接。

### EHCI

- qTD 与 QH 增加五个高缓冲指针 dword。qTD 补到 64 字节。QH 补到 96 字节。
- 地址 0 先读 8 字节描述符。设置地址。再读设备描述符。
- 端口复位后等 20 ms，满足 USB 复位恢复时间。
- 批量 QH 使用枚举得到的速度。
- 停异步调度。等 `USBSTS.ASS` 清零。改写 QH。再置 `ASE`。
- 每次装 QH 前清 `USBSTS` bit 0..5。状态 qTD 给有效缓冲指针。
- QH multiplier 置 1。全速与低速填根 Hub TT 端口号。
- 给固件关掉的端口供电。等待链路状态。
- 做 BIOS 交接。扫描两个 EHCI 控制器。
- 延时改用 HPET。block 系统调用里 PIT 中断不推进。

### AHCI

- 清 `ST` 前先置 ICC Active。
- DET 为 0 且签名有效的端口照常复位。Skylake 的盘休眠时会报 DET 0。Haswell 报 DET 1 或 3。
- DET 为 0 且签名看似残留的端口也复位。固件下电的盘在复位后仍可连上。
- DET 0 等 1 秒。DET 1/2 等 2 秒。空端口同样等待复位。
- ATA 签名端口首次 COMRESET 失败后，再试一次 2 秒等待。
- 链路复位后，端口启动与 IDENTIFY 各重试一次。
- USB 优先生效前先识别 ATA 盘。数据来自 U 盘时，型号仍留在摘要里。

### 启动流程

- shell 前等 USB 线程最多 5 秒。`U-Disk` 行与控制器行归入启动摘要。
- shell 前探测块服务，超时 12 秒。超时后交给 `blkprobe` 线程。
- 块服务先回 ATA 型号，随后接入 USB。`Hard Disk` 行先于 init 打印。
- ext2 挂载重试 10 次。块服务首次请求时可能还在等 USB。
- 内核日志环改为 48 KiB。64 KiB 会让 Haswell 在串口初始化前复位。
- 新增 `dmesg [关键词...]`。每个关键词都出现才打印该行。匹配不分大小写。

### 启动耗时

- USB2 路由读回确认后，跳过 EHCI 复位与其等待。
- xHCI 控制器复位后等链路 200 ms，不再等 500 ms。
- 设备描述符只试两次，不再试五次。无响应的端口不再让 shell 多等 2.5 秒。

## 改动文件

| 文件 | 改动 |
|------|------|
| `kernel/device/usb/ehci.c` | EHCI 控制器、64 位结构、BIOS 交接、端口供电、BOT |
| `kernel/device/usb/xhci.c` | xHCI USB2 路由、USB2 复位、端口供电、HCH 等待、速度 |
| `kernel/device/usb/xhci.h` | EHCI 与拉起入口 |
| `kernel/kmain.c` | USB 拉起等待与块探测时序 |
| `kernel/srv/block_srv.c`、`kernel/srv/block_srv.h` | 有界块探测与后台重试 |
| `kernel/lib/klog.h` | 48 KiB 日志环 |
| `userspace/bin/hansh.c` | `dmesg [关键词...]`；帮助项 |
| `userspace/servers/ext2.c` | 挂载重试 |
| `userspace/servers/block.c` | AHCI 端口拉起；USB 优先 |
| `attic/2026/2026-10-18-haswell-usb2-ports-en.md`、`attic/2026/2026-10-18-haswell-usb2-ports-zh.md` | 本说明 |

## 测试

- 内核与用户态编译无 `-Wall -Wextra` 警告。
- QEMU EHCI：存储与 ext2 读取通过。
- QEMU xHCI：SuperSpeed 存储与高速键盘枚举。
- 无盘 `cdrom.iso`：日志含 `name "hansh"`。无 panic、exception、corrupt 行。
- Haswell：路由报告打印。U 盘报 30277632 个扇区。`/data` 挂载。`tetris`、`2048`、`snake` 运行。
- Skylake：`U-Disk : Kingston DataTraveler 3.0` 与 `Hard Disk : SanDisk SD8TB8U256G1` 先于 shell 打印。
- Haswell 与 Skylake 启动顺序：`U-Disk`、`Hard Disk` 与 USB 行先于 shell 打印。

## TODO

- 热插拔：开机后重扫。处理 connect change。加 `usbrescan` 命令。
- EHCI 全速：TT 路径未在无 xHCI 路由的机器上验证。补 companion/UHCI 驱动。硬件矩阵完成后决定保留或删除。
- EHCI BOT 恢复：端点 halt 时发 ClearFeature。重装 QH 后重试。xHCI 已有 `msc_recover`。
- EHCI 传输长度：单个 qTD 覆盖约 8 KiB。加 qTD 链。bounce buffer 提到 64 KiB。
- 块大小：检查 READ CAPACITY 的块长。支持 4Kn 盘。
- 分区发现：解析 MBR。去掉 ext2 分区索引 1 的硬编码。按类型或 UUID 选分区。
- Hub：枚举下游设备。当前只扫根端口。
- 多控制器、多设备、多 LUN：当前停在首个存储设备。
- UAS：xHCI 只跑 BOT。补 UAS。
- xHCI 路由：PCH ID 改成表驱动。补其他 Intel PCH 与 quirk。
- 电源：挂起、恢复、过流、端口供电恢复。
- 性能：完成改为事件驱动。合并批量传输。减少忙轮询。
- 启动：用就绪信号替代 5 秒 USB 等待与 12 秒块探测。无 USB 时提前进 shell。
- DMA 与 IOMMU：记录 EHCI 32 位限制。固件开启 IOMMU 时补映射。
- 测试矩阵：Haswell、Skylake、QEMU EHCI 与 xHCI、USB2 与 USB3 盘、Hub、热插拔。
