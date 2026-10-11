# 2026-10-11：USB 大容量存储（xHCI BOT）支撑 live USB

## 目标

live U 盘第一分区放内核与 initrd，第二分区放 ext2 游戏。块服务器经同一 USB 设备读该 ext2 分区。

## 现象

内核枚举到 QEMU 的 `usb-storage` 设备，跑通 TEST UNIT READY 与 READ CAPACITY。第一次读扇区 1 成功，第二次失败。设备对下一条 CBW 回 STALL，并一直停住。块服务器看不到盘。

## 排查

逐一排除：

- 系统调用上下文。内核线程探针走同一路径读扇区 1，同样失败。调用方无关。
- 中断使能。清掉 `USBCMD_INTE` 依旧失败。
- 缓冲大小与 DIR 位。缩小缓冲、去掉第 16 位，均无变化。
- 空闲间隔。故障看似与传输间的 sleep 相关，换个启动又通过。这是竞态，不是固定间隔。

事件日志暴露两处缺陷：

- BOT 驱动只对批量 OUT 端点按铃。CSW 走批量 IN，其环无人按铃。该传输只出两个事件，从不出三个。
- `READ CAPACITY` 的字段是大端。驱动按小端读，512 字节块校验失败。

真正缺陷在结构上。控制器从环取 TRB，取到一个传输描述符完成即停。CBW、数据段、CSW 只按一次端点门铃，CSW 取不到。停住后端点进入 Halted，设备侧 ClearFeature 不能重启它。

## 根因

一次 Bulk-Only Transport 传输需要三个分阶段门铃，落在两个端点上。每端点一次门铃取不齐三个描述符。设备 STALL 会挂起宿主端点。只有 xHCI `Reset Endpoint` 命令能清该挂起。只清设备侧挂起，管道仍死，重试继续停。

## 修复

- 两个批量端点都按铃。
- `READ CAPACITY` 字段按大端读。
- CBW、数据段、CSW 拆成独立阶段，各自门铃与等待。
- 增加 `msc_recover()`：Bulk-Only Mass Storage Reset、两个批量端点的 ClearFeature(ENDPOINT_HALT)、每端点的 xHCI `Reset Endpoint`（`TRB_RESET_EP`）命令。失败后恢复再重试。
- HID 线程与存储传输共用一个事件环。用 `evt_lock` 串行化，统一走 `xhci_drain()`。

## 接入

- 新增系统调用 `SYS_USB_BLOCK_INFO/READ/WRITE`（`0x434-0x436`）与 `sys_usb_block_*`。
- `usb_msc_wait()` 让首次查询等枚举完成。块服务器不与枚举抢跑。
- 块服务器找不到 AHCI 盘时回退到 USB 设备。`blk_read`、`blk_write` 按 `use_usb` 分派。

## 变更文件

| 文件 | 变更 |
|------|------|
| `kernel/device/usb/xhci.c` | USB 大容量存储 BOT 与 SCSI；共享 `xhci_drain`；`msc_recover` 含 Reset Endpoint |
| `kernel/device/usb/xhci.h` | 导出 `usb_msc_ready/sectors/read/write/wait` |
| `kernel/proc/syscall.c` | `k_usb_block_info/read/write` |
| `include/syscall_nr.h` | `SYSCALL_USB_BLOCK_INFO/READ/WRITE` |
| `userspace/runtime/sysfunc.c` | `sys_usb_block_*` |
| `userspace/include/sysfunc.h` | 声明 |
| `userspace/servers/block.c` | USB 回退与 `blk_read`、`blk_write` |

## 测试

- `make -C kernel clean && make -C kernel` 无告警。
- 用 `-device qemu-xhci -device usb-storage` 从 U 盘启动。shell 执行 `/data/bin/tetris`，5 次无故障、无读失败。
- 每次启动都有 `block: GET_INFO sector_size 512 count 524288` 与 `block: PART 1 start 198656 count 325599`。
- `cdrom.iso` 无盘启动：`name "hansh"`，无 panic、无 exception、无 corrupt。
- 同一映像走 AHCI 启动：`execute "/data/bin/tetris"` 仍正常。

## 尚未完成

- HID 设备在线时，与存储传输同时使用共享事件环的场景未测。
- 数据翻转位靠 STALL 后的 `Reset Endpoint` 复位，未做端口显式复位。
