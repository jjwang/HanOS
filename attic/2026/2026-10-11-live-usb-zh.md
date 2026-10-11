# 2026-10-11：xHCI 上的 live USB 存储

## 目标

live U 盘第一分区放内核与 initrd，第二分区放 ext2 游戏。内核经同一 xHCI 设备读该分区，QEMU 与 Skylake 笔记本都要能跑。

## QEMU 引导

内核枚举到 QEMU 的 `usb-storage`，跑通 TEST UNIT READY 与 READ CAPACITY，读扇区 1 一次。第二次失败：设备对下一条 CBW 回 STALL 并一直停住。逐一排除：系统调用上下文、`USBCMD_INTE`、缓冲大小与 DIR 位、空闲间隔。剩三处缺陷。

- BOT 驱动只对批量 OUT 端点按铃。CSW 走批量 IN，因此取不到。
- `READ CAPACITY` 字段是大端，按小端读，块校验失败。
- 控制器从环取 TRB，取到一个传输描述符完成即停。CBW、数据段、CSW 只按一次端点门铃，CSW 取不到。

设备 STALL 还会挂起宿主端点。只有 xHCI `Reset Endpoint` 命令（`TRB_RESET_EP`，类型 14）能清该挂起；只做设备侧 ClearFeature 管道仍死，重试继续停。

修复：两个批量端点都按铃；字段按大端读；CBW、数据段、CSW 拆成独立门铃阶段；重试前先 `msc_recover()`（Bulk-Only Mass Storage Reset、两个批量端点的 ClearFeature、每端点一条 `Reset Endpoint`）。HID 线程与存储传输共用一个事件环，用 `evt_lock` 与单一 `xhci_drain()` 串行化。

## 物理机

在 Skylake 笔记本（Sunrise Point-LP xHCI）上，同一份代码依次三处失败，而 QEMU 每步都过。对照 Linux（`xhci.c`、`xhci-mem.c`、`xhci-ring.c`、`xhci.h`）发现多处偏差。

端口引导：

- 端口速度在 PORTSC 的 bit 10-13，不是 bit 0-3。
- PORTSC 写出只保留端口供电、唤醒使能与可移除位。回写读值会重新置起 `PORT_RESET`（bit4，固件常置位），把 USB3 口打成 Disabled，且跨重启保留。
- U 盘是启动盘，固件已配置过它。Address Device 需要设备回到 Default，USB3 口在 U0 时用 RxDetect（PLS 5 + Link State Write Strobe）拉回，不能用 hot reset。空端口无需处理；对它强制 RxDetect 会让枚举停顿约十秒。
- 固件持有 USB Legacy Support。要置 OS Owned Semaphore 并等 BIOS Owned Semaphore 清零，否则固件用 SMI 占着端口。
- scratchpad 数量是 `HCSPARAMS2` 的 `((p>>16)&0x3e0)|((p>>27)&0x1f)`，两段位域写反了。
- 输入 Slot Context 的 Device Address 保持 0，地址由控制器按 slot ID 分配。

控制传输：

- 控制 IN 的短包以 `COMP_SHORT_PACKET`（13）完成；判为失败才让配置描述符读失败。接受 13。
- 配置描述符分两段读：先 9 字节头取 `wTotalLength`，再按该长度读。
- Setup 与 Data 阶段 TRB 不设 CHAIN（控制器按类型串联阶段）；IN 的 Data TRB 设 ISP。

批量传输：

- 端点上下文要设 SuperSpeed 伴随描述符的 `MaxBurst`，原来留 0。
- `ring_enqueue` 未写 LINK TRB：`*enq` 一直 `% RING_NUM`，回绕分支不执行。在段尾写 LINK TRB 并在那里翻转 cycle，与 Linux `inc_enq` 一致。
- 批量数据用单个 NORMAL TRB 承载全长，带 IOC 与 ISP。

## 外设

- 块服务器优先 USB 设备；数据走 U 盘时也识别 ATA 盘取型号；等 USB 更久。
- 启动摘要打印磁盘而非控制器。`Hard Disk` 取 ATA IDENTIFY 的型号，`U-Disk` 取 SCSI INQUIRY 的厂商与型号，都上屏。
- shell 增加 `usb`（USB 块状态）与 `dmesg`（内核日志环）。
- tty 串口镜像按行缓冲，逐键 echo 不再与内核日志交错。
- 输入路径去掉 Pause 序列消耗（它会吞掉后续 break 码）；重复改由键盘 typematic 驱动并加确认窗口；启用串口输入前探测 UART scratch 寄存器。

## 性能

- `msc_bot` 每阶段只轮询一次事件环就 `sched_sleep(1)`。USB 传输是微秒级，导致每个命令至少 3ms，读一个游戏 ELF 要几百毫秒。改为先自旋轮询事件环（2000 次）再让出，快传输不再睡眠。
- `port_reset` 对每个端口都强制 RxDetect 并等 U0。空端口到不了 U0，每个耗尽超时，整段枚举约十秒。只在有设备或端口 Disabled 时才拉链路，枚举降到一秒以内。

## 变更文件

| 文件 | 变更 |
|------|------|
| `kernel/device/usb/xhci.c` | BOT 与 SCSI、共享 `xhci_drain`、`msc_recover`、端口引导、所有权交接、控制与批量 TRB、端点上下文、环 LINK TRB、scratchpad 缓冲 |
| `kernel/device/usb/xhci.h` | 导出 `usb_msc_ready/sectors/read/write/wait` |
| `kernel/proc/syscall.c` | `k_usb_block_info/read/write`、`SYSCALL_KLOG` |
| `include/syscall_nr.h` | `SYSCALL_USB_BLOCK_INFO/READ/WRITE`、`SYSCALL_KLOG` |
| `kernel/lib/klog.c`、`kernel/lib/klog.h` | `klog_read` |
| `kernel/srv/block_srv.c` | 打印 ATA 型号 |
| `kernel/kmain.c` | 去掉 PCI 控制器磁盘行 |
| `userspace/runtime/sysfunc.c`、`userspace/include/sysfunc.h` | `sys_usb_block_*`、`sys_klog` |
| `userspace/servers/block.c` | USB 回退、ATA 型号、等待 USB 更久 |
| `userspace/bin/hansh.c` | `usb` 与 `dmesg` 命令 |
| `userspace/servers/tty.c` | 串口镜像按行缓冲 |
| `userspace/servers/input.c` | 丢 break 处理、typematic 重复 |

## 测试

- `make -C kernel clean && make -C kernel` 无告警。
- QEMU live USB（`-device qemu-xhci -device usb-storage`）：`block: PART 0 start`、`ext2: read /assets/readme.txt`、`execute "/data/bin/tetris"`，无读失败。
- 物理 Skylake 笔记本：shell 反复从 `/data/bin` 运行 `tetris`、`snake`、`2048`；`U-Disk      : Kingston DataTraveler 3.0`。
- `cdrom.iso` 无盘启动：`name "hansh"`，无 panic、无 exception、无 corrupt。

## 尚未完成

- HID 设备在线时，与存储传输同时使用共享事件环的场景未测。
- 数据翻转位靠 STALL 后的 `Reset Endpoint` 复位，未做端口显式复位。
