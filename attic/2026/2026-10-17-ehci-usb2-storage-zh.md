# 2026-10-17：EHCI USB 2.0 存储

## 目标

在独立 EHCI 上驱动 USB 2.0 存储设备。部分机器把 USB 2.0 端口挂在独立 EHCI 上。xHCI 扫描找不到存储设备。

## 驱动

- 扫描 PCI 找 EHCI 控制器。映射 BAR0。复位控制器。
- 跑一条异步队列头。每次控制或批量传输都重装该队列头。
- 枚举首个存储设备。发送 GET_DESCRIPTOR、SET_ADDRESS、GET_CONFIGURATION、SET_CONFIGURATION。
- 遍历配置描述符。找到存储接口与两个批量端点。
- 跑 Bulk-Only Transport。发送 CBW。按方向发送数据阶段。读取 CSW。
- 经 `usb_msc_*` 提供 READ CAPACITY 与 READ/WRITE(10)。
- 填满 qTD 的缓冲指针表。跨页传输由此覆盖每一页。
- 传输超时改用 HPET。block 服务在系统调用里读盘。此时 PIT 中断不推进。

## 修掉的故障

- QH 端点号位于 bit 8..11。旧移位把端点 2 落进端点 1 的字段。
- 配置描述符字段：bConfigurationValue 在字节 5。bEndpointAddress 在字节 2。bmAttributes 在字节 3。
- CSW 签名是 "USBS"。字节 3 是 0x53。
- BOT 写数据阶段两个方向都用 IN 端点。
- qTD 只带一个缓冲指针。跨页传输停在页边界。

## Block 服务

- 拉起端口时跳过 DET 为 0 的 AHCI 端口。QEMU 空端口不再各等两秒复位。
- 端口匹配 ATA 盘后恢复 `port_start` 与 `ahci_identify`。

## 改动文件

| 文件 | 改动 |
|------|------|
| `kernel/device/usb/ehci.c` | EHCI 拉起、控制与批量传输、BOT 与 SCSI |
| `kernel/device/usb/xhci.h` | 导出 EHCI 存储入口 |
| `kernel/device/usb/xhci.c` | USB 块设备回落到 EHCI |
| `userspace/servers/block.c` | 跳过空 AHCI 端口；恢复磁盘探测 |
| `design/usb-live-and-games-en.md`、`design/usb-live-and-games-zh.md` | 记录 EHCI USB 2.0 路径 |

## 测试

- 内核与用户态编译无 `-Wall -Wextra` 警告。
- QEMU q35 挂 `ich9-usb-ehci1`。存储设备挂在 `usb-storage` 上。EHCI 读出 524288 个扇区。`ls /data/bin` 经 EHCI 读 ext2 分区。输出为 `2048`、`snake`、`tetris`。
- QEMU q35 挂 `qemu-xhci`。xHCI 仍能枚举存储设备。键盘同样可用。
- 无盘 `cdrom.iso` 启动：串口日志含 `name "hansh"`。日志无 panic、exception、corrupt 行。
