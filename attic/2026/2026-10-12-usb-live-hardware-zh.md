# 2026-10-12：live USB 在物理机上的 xHCI 引导

## 目标

在 Skylake 笔记本上从 live U 盘启动并运行 ext2 游戏。QEMU 通过，物理机不行。

## 现象

依次三处失败：

- `USB: address device port 16 failed cc 4`：SET_ADDRESS 报 USB 事务错误。
- `USB: config descriptor on port 16 failed`：255 字节控制读不完成。
- `USB: msc DATA timeout`：4096 字节批量读在设备侧成功（CSW 状态 0），但传输事件不到，`tetris` 只跑一次就报 `ELF(...): File header error`。

## 根因

对照 Linux（`xhci.c`、`xhci-mem.c`、`xhci-ring.c`、`xhci.h`）发现多处偏差。QEMU 容忍，真实 Sunrise Point-LP 不容忍。

端口引导：

- scratchpad 数量：`HCSPARAMS2` 的两段位域写反了，应为 `((p>>16)&0x3e0)|((p>>27)&0x1f)`。
- 端口速度在 PORTSC 的 bit 10-13，不是 bit 0-3。
- PORTSC 写出回写了读值，重新置起 `PORT_RESET`（bit4，固件常置位），把 USB3 口打成 Disabled，且跨重启保留。写只保留端口供电、唤醒使能与可移除位。
- U 盘是启动盘，固件已配置过它；Address Device 需要设备回到 Default。USB3 口在 U0 时用 RxDetect（PLS 5 + Link State Write Strobe）拉回，不能用 hot reset。
- 固件持有 USB Legacy Support；要置 OS Owned Semaphore 并等 BIOS Owned Semaphore 清零，否则固件用 SMI 占着端口。

控制传输：

- 控制 IN 的短包以 `COMP_SHORT_PACKET`（13）完成；判为失败才让配置描述符读失败。接受 13。
- 配置描述符分两段读：先 9 字节头取 `wTotalLength`，再按该长度读。
- Setup 与 Data 阶段 TRB 不设 CHAIN（控制器按类型串联阶段）；IN 的 Data TRB 设 ISP。
- `Reset Endpoint` 的 TRB 类型是 14；17 是 `Reset Device`。

批量传输：

- 端点上下文要设 SuperSpeed 伴随描述符的 `MaxBurst`，原来留 0。
- `ring_enqueue` 未写 LINK TRB：`*enq` 一直 `% RING_NUM`，回绕分支不执行。在段尾写 LINK TRB 并在那里翻转 cycle，与 Linux `inc_enq` 一致。
- 批量数据阶段用单个 NORMAL TRB 承载全长，带 IOC 与 ISP。

输入 Slot Context 的 Device Address 保持 0，地址由控制器按 slot ID 分配。

## 修复

在 `kernel/device/usb/xhci.c` 落实以上各点。

## 同期外设修复

- `userspace/servers/block.c`：优先 USB 设备；数据走 U 盘时也识别 ATA 盘取型号；等 USB 更久。
- `kernel/kmain.c`、`kernel/srv/block_srv.c`、`kernel/device/usb/xhci.c`：启动摘要打印磁盘而非控制器。`Hard Disk` 取 ATA IDENTIFY 的型号，`U-Disk` 取 SCSI INQUIRY 的厂商与型号，都上屏。
- `kernel/lib/klog.c` 与 `userspace/bin/hansh.c`：`usb` 打印 USB 块状态，`dmesg` 转储内核日志环。
- `userspace/servers/tty.c`：串口镜像按行缓冲，逐键 echo 不再与内核日志交错。
- `userspace/servers/input.c`：去掉 Pause 序列消耗（它会吞掉后续 break 码）；重复改由键盘 typematic 驱动并加确认窗口；启用串口输入前探测 UART scratch 寄存器。

## 变更文件

| 文件 | 变更 |
|------|------|
| `kernel/device/usb/xhci.c` | 端口引导、所有权交接、控制与批量 TRB、端点上下文、环 LINK TRB |
| `userspace/servers/block.c` | 优先 USB、ATA 型号、等待 USB 更久 |
| `kernel/srv/block_srv.c` | 打印 ATA 型号 |
| `kernel/kmain.c` | 去掉 PCI 控制器磁盘行 |
| `kernel/lib/klog.c`、`kernel/lib/klog.h` | `klog_read` |
| `kernel/proc/syscall.c` | `SYSCALL_KLOG`、`k_usb_block_info/read/write` |
| `userspace/servers/tty.c` | 串口镜像按行缓冲 |
| `userspace/servers/input.c` | 丢 break 处理、typematic 重复 |
| `userspace/bin/hansh.c` | `usb` 与 `dmesg` 命令 |

## 测试

- `make -C kernel clean && make -C kernel` 无告警。
- QEMU live USB（`-device qemu-xhci -device usb-storage`）：`block: PART 0 start`、`ext2: read /assets/readme.txt`、`execute "/data/bin/tetris"`，无 msc 失败。
- 物理 Skylake 笔记本：shell 反复从 `/data/bin` 运行 `tetris`、`snake`、`2048`；`U-Disk      : Kingston DataTraveler 3.0`。
- `cdrom.iso` 无盘启动：`name "hansh"`，无 panic、无 exception、无 corrupt。
