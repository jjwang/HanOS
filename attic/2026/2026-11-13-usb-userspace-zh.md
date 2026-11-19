# 2026-11-13: USB 主控栈移入用户态

把 xHCI、EHCI、USB HID 解码与大容量存储传输移入 `/bin/usb`。

## 内核

- `kernel/srv/usb_srv.c` 查找 xHCI 与 EHCI，映射各自 BAR0，授予 512 KiB DMA 区与 PCI 配置 I/O 端口，并传入 tty/GPU 端点。
- 删除 `kernel/device/usb/xhci.c`、`ehci.c` 与 `kmain` 中的 USB 启动调用。
- USB block 系统调用改为通过内存对象转发到 `SVC_USB`。block server 接口不变。

## 用户态

- `/bin/usb` 负责控制器复位、端口枚举、HID 轮询与 Bulk-Only Transport。
- HID 按键发往 tty 端点；指针增量发往 GPU 端点。
- `USB_BLOCK_INFO/READ/WRITE` 用 `xfer[1]` 的内存对象传递数据。
- PCI 配置访问使用授予的 `0xCF8..0xCFF` I/O 端口。DMA 来自内核固定授权。

## 测试

- 内核与用户态构建无警告。
- 基础镜像与 ports 镜像都启动到 `hansh` 与 `bash`。
- QEMU xHCI：HID 键盘进入 shell，U 盘报告扇区数。
- QEMU EHCI：U 盘报告扇区数。
- 无 USB 主控的 CI 镜像走 AHCI 块路径启动。
