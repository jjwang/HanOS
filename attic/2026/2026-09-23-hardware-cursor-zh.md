# 2026-09-23：硬件光标、PS/2 与 USB HID 指针

## 概述

用 GPU 驱动鼠标光标。Skylake 显示引擎的硬件光标平面把箭头合成到 console 之上。它不触碰扫描帧缓冲。指点设备负责移动光标。两条输入链路均已实现：PS/2 辅助端口与经 xHCI 控制器的 USB HID。

## 变更

- `gfx_reg.h`：新增硬件光标寄存器宏。
- 新增宏覆盖 FBC 控制、live surface、光标 watermark。
- 新增宏还覆盖显示数据缓冲分配与位置字段掩码。
- 新宏与已有 control/base/position 并列。
- `skl_display.c`：`skl_cursor_init()` 分配并绘制 64x64 ARGB GTT 表面。给它一块数据缓冲分配与 watermark。编程 `CURCNTR` / `CURPOS` / `CURBASE`。`skl_cursor_set()` / `skl_cursor_move()` 放置光标。`skl_cursor_selftest()` 驱动光标绕屏移动。
- `gfx.c` / `gfx.h`：模式设置成功后初始化光标，运行一次自检，并暴露 `gfx_cursor_move()`。
- 新增 `device/usb/xhci.{c,h}`：精简 xHCI 驱动。复位控制器，在任意根端口枚举 HID 指针。内核线程轮询其中断 IN 端点。报告喂给 `gfx_cursor_move()`。
- `keyboard.c`：解析 PS/2 鼠标三字节包，把位移喂给光标。指点设备挂在 PS/2 控制器后面时，此路径作为兜底保留。
- `kmain.c`：SMP 初始化后，在独立内核线程启动 USB 初始化（`usb_hid_start()`）。枚举慢或卡住不阻塞启动。
- `gfx.h`：让头文件自包含，包含 `stdint.h`、`stdbool.h`、`sys/pci.h`。

## 实现细节

### 光标表面与寄存器

- `skl_cursor_init()` 经主平面同一条 `gfx_alloc()` 路径分配 64x64 ARGB 表面（`CURSOR_SIZE * CURSOR_SIZE * 4` = 16 KiB）。
- `cursor_draw()` 把表面 `memset` 成透明，再把 24x24 位图拷到左上角。
- 表面启用前用 `wbinvd` 刷出写入。
- 光标控制与位置寄存器分别为 `CURACNTR`、`CURAPOS`。
- 表面地址寄存器为 `CURABASE`，FBC 寄存器为 `CUR_FBC_CTL_A`。
- watermark 寄存器为 `CUR_WM_0..7` 与 `CUR_WM_TRANS`。
- 显示数据缓冲分配寄存器为 `CUR_BUF_CFG`。
- `CUR_BUF_CFG` 取 block 446..477，紧挨主平面的 block 0..445。
- 所有光标 watermark 设为 `PLANE_WM_EN | PLANE_WM_IGNORE_LINES | PLANE_WM_BLOCKS(32)`，保证小块取数不饿死。
- 控制寄存器使用 `CURSOR_MODE_64_ARGB_AX`（`0x27`，方形 64x64 ARGB，带 alpha X 通道）。
- 写入顺序为 FBC、control、position，最后 base。写 `CURABASE` 武装整组双缓冲光标状态。
- `cursor_pos()` 把位置编码为符号-幅值（每轴 1 符号位 + 15 位幅值）。减去 `CURSOR_HOT_X`/`CURSOR_HOT_Y`，使 (7,4) 热点落在箭头尖端。
- `skl_cursor_set()` 夹取到模式范围，先写 `CURAPOS` 再写 `CURABASE`。
- 驱动用 `skl_cursor_move()` 做增量移动。
- `skl_cursor_selftest()` 依次访问四角与中心，使光标在指针驱动出现前可见。
- 位图取自 DMZ-White 的 `left_ptr`：24x24 ARGB 白箭头，带黑描边与抗锯齿边缘。

### PS/2 辅助指针

- `keyboard.c` 在键盘初始化时启用辅助端口。
- 用 `0xA8` 打开 aux。用 `0x20` 读回配置字节。置位 bit 1（aux 中断）。用 `0x60` 写回。
- 向鼠标发送 `0xF6`（默认设置）与 `0xF4`（启用）。把 `mouse_callback` 注册到 IRQ12。
- 回调每次中断从端口 `0x60` 读一字节，运行三字节状态机。
- 第一字节必须置有 bit 3，作为标志字节保存。
- 随后两字节是 X、Y 位移，分别按标志字节 bit 4（X）与 bit 5（Y）做符号扩展。
- X 位移直接使用。Y 位移取负，因为屏幕 Y 轴向下，而 PS/2 以上为正。
- 随后调用 `gfx_cursor_move()`。

### xHCI 控制器初始化

- `usb_hid_init()` 读取每个设备的 PCI class 寄存器查找控制器：`0x0C` 串行总线、子类 `0x03`、prog-if `0x30` 即 xHCI。
- 打开内存与总线主控访问。BAR0 经 `PHYS_TO_VIRT` 映射。
- `HCCPARAMS1` 报告 64 字节上下文时直接跳过，那需要不同上下文布局。
- 控制器用 `USBCMD.HCRST` 复位。代码等待 `USBCMD.HCRST` 清零与 `USBSTS.CNR` 清零。
- 共享结构均用 `dma_alloc()` 分配（物理连续页、清零、经直接映射访问）。
- 分配对象包括设备上下文基址数组、命令环与事件环。
- 事件环带 ERST 表项。
- 分配对象还包括设备与输入上下文、EP0 环、中断环。
- 分配对象还包括 64 字节中断缓冲与 512 字节控制缓冲。
- `OP_CONFIG` 写 slot 数。`OP_DCBAAP` 指向数组。`OP_CRCR` 指向命令环。
- 事件环由 `ERSTSZ`、`ERSTBA`、`ERDP` 描述。`IMAN` 使能中断。控制器用 `USBCMD_RS | USBCMD_INTE` 启动。
- TRB 携带 cycle 位。环在回绕处插入 Link TRB。`poll_event()` 越过回绕时翻转消费者 cycle 位。控制器与驱动对环所有权达成一致。

### 枚举与描述符解析

- 对每个根端口，`port_reset()` 要求 `PPORTSC.CCS`，置 `PR`，等 `PRC`，应答 `PRC`/`CSC`，读回端口速度。
- `ENABLE_SLOT` 给出 slot ID。
- `address_device()` 清空输入上下文，写入 slot 速度与根端口号。
- 它把设备地址设为 slot ID。
- 把 EP0 编程为控制端点，按速度选最大包长（8/64/512），指向 EP0 环并置 DCS。
- EP0 控制传输统一走 `control_xfer()`：构造 setup TRB、可选 data TRB、与数据段反向的 status TRB。data TRB 方向由请求 bit 7 决定。
- 驱动先读 18 字节设备描述符，再读配置描述符。
- 按可变长记录遍历，寻找 HID 类接口（class 3）。
- 驱动随后定位中断 IN 端点（地址 bit 7 置位、属性 `0b11`）。
- 记下端点 interval 与最大包长。发送 `SET_CONFIGURATION`。发送 HID `SET_PROTOCOL(boot)`。
- 为 DCI 3 调用 `configure_endpoint()`（`EP_TYPE_INT_IN`、interval 与最大包长取自描述符、置 DCS、指向中断环）。
- 第一个给出 HID 指针的端口即胜出。

### 报告轮询

- `usb_hid_init()` 派生 `usbmouse` 线程。
- 每轮在中断环入队普通 TRB（`TRB_NORMAL`，带完成中断位）。为 slot 与 DCI 3 敲 doorbell。等待对应 transfer 事件。短/零完成则重试。
- 成功后从 `int_buf` 取报告：byte 1、byte 2 是带符号 X、Y 位移，byte 0 是按键掩码。
- USB HID 的 Y 轴与屏幕同向，位移原样传给 `gfx_cursor_move()`。前八份报告会打印。
- 初始化运行在 `usb_hid_start()` 创建的 `usbinit` 线程。复位与控制传输超时不阻塞启动。结束后挂起。

## 说明

- 光标是独立平面。它合成在 console 之上，不干扰帧缓冲。
- 多数 Skylake 笔记本的触摸板与 USB 鼠标挂在 xHCI 控制器下。那里起作用的是 USB 路径。
- PS/2 路径只能看到 EC 仍以 PS/2 形式呈现的设备。
- USB 路径采用轮询，暂未做 xHCI 中断处理。按键与滚轮暂不处理。
- xHCI 枚举假定 32 字节设备上下文，暂不处理 hub、stream 与 USB 3.1 Link PM。

## 测试

- `make -C kernel clean && make -C kernel` 无警告，可构建启动镜像。
- QEMU 加 `-device qemu-xhci -device usb-mouse`：控制器拉起，鼠标枚举成功（`idVendor 0627 idProduct 0001`）。HID 中断 IN 端点配置成功。驱动正确解出注入的鼠标移动（`pointer 40,25`、`pointer -30,-20`）。启动仍到达 `name "hansh"`，无 panic。
- QEMU 不带 xHCI 控制器：驱动报告未找到控制器，启动行为不变。
- Skylake/HD520 目标机：箭头出现在面板中央。自检时访问四角，之后跟随触摸板或 USB 鼠标移动。
