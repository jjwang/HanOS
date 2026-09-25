# 2026-09-22：Skylake eDP 模式设置

## 概述

在主 eDP 端口（DDI-A）上按启动模式编程 Skylake Gen9 显示管线，把扫描用的
帧缓冲交给终端服务，并修复在 HD520 目标机上发现的问题：面板出现移动的水波
纹，以及画面一直被锁在固件的 800x600 源区域。

## 变更

- `gfx_reg.h`：新增并修正 Gen9 模式设置所需寄存器——eDP 转码器时序、源尺寸
  副本与 DP M/N；eDP 转码器的移位 pipe 配置（`0x7F008`）；pipe A 的两个
  scaler；`DC_STATE_EN`；`DP_TP_CTL` 的链路训练与 enhanced framing 位；plane
  的 data buffer 与 watermark 字段；AUX 控制字段。
- `skl_display.c`：接管时按逆序拆一次旧管线，保留固件的 eDP M/N 与色彩深度，
  按规定顺序使能转码器/pipe，拆掉固件的面板 scaler，并在 vblank 处补写双缓冲
  的源尺寸/plane 状态。
- `gfx.c`：用 `gfx_attach_fb()` 把新帧缓冲交给终端（分配 backbuffer、更新终端
  几何），成功时提前返回，而不再继续使用固件 aperture 帧；并在模式设置前把
  当前正在扫描的那一帧清成黑色。
- `term.{c,h}`：新增 `term_update_size()`，交接后让字符网格跟随新帧缓冲。
- `fb.c`：用非临时存储把背缓冲刷到 scan-out，让内核不留下脏 cache line 给
  console 服务。
- `fb.h` / `kmain.c`：去掉写死的 `FB_WIDTH`/`FB_HEIGHT` 上限；改为从终端帧
  缓冲报告实际扫描几何，而不是固件的帧缓冲结构。
- `skl_display_dump()`：收成精简的关键寄存器概要（时钟、pipe/plane 几何、
  eDP 链路/M/N、背光）。删除了大范围的逐区域扫描及其辅助函数。

## 实现细节

### 原理

- 显示通路是一条链（`plane -> pipe -> transcoder -> DDI -> PLL`），因此从源头
  向外依次使能、逆序拆除。动时钟树之前必须先确认 pipe 真的停了——帧计数器不再
  前进——否则仍在扫描的 pipe 会被正在变化的时钟驱动。
- 分辨率与链路训练彼此独立：分辨率决定 pipe/transcoder 时序（pixel clock），
  链路训练决定 symbol clock / 链路速率。仅改分辨率因此无需重训，这正是 i915
  的 fastset 选择；只有链路速率或通道数变化时才需要重训。
- 源尺寸与 plane 窗口是双缓冲的，只有在 vblank 处武装后才生效，因此必须在 pipe
  运行之后再补写一次。
- 面板为 6bpc，固件的 data M/N 与色彩深度给链路定速，必须保留；按 24bpp 重算会
  让 sink 失步。
- 显示引擎从 DRAM 取数，因此要安排好 CPU 侧的写入者，不能留下缓存副本在稍后
  回写覆盖 scan-out。

### 操作

接管时（从固件画面起，仅此一次）：

1. 逆序拆旧管线：plane、pipe、transcoder、DDI；动任何时钟前确认 pipe 已停。
2. 只按本次改动需要配置时钟与链路：CDCLK、再按新链路速率配 PLL、再
   `DDI_BUF_TRANS`、再 AUX 训练（CR -> CE）。纯换分辨率都不需要，因此保留固件的
   CDCLK、PLL 与训练。
3. 正序开新管线：`DDI_BUF_CTL`、transcoder、pipe、plane；再在 vblank 处补写双
   缓冲的源尺寸与 plane 状态。
4. 把扫描帧缓冲交给 console，并停止内核终端对 scan-out 的写入。

下面各小节给出每一部分的寄存器级细节。

### 帧缓冲分配与几何

`skl_edp_set_mode()` 自己计算扫描几何，不复用启动帧缓冲：
`pitch = (hactive * 4 + 63) & ~63`（1366 像素 → 5464 → 5504，使每行起始都落在
`PLANE_STRIDE` 要求的 64 字节边界上），`fbsize = pitch * vactive`。随后
`gfx_alloc()` 从 `pmm_get` 取 `ceil(fbsize / 4096)` 个**物理连续**页，把共享
GPU 地址按对齐要求对齐，并通过 `gfx_gtt_map()` 逐页写入 GTT 表项——每个 4 KiB
GPU 页指向对应的物理页。它返回 plane 使用的 GPU 地址，以及内核使用的 CPU 地址
（`PHYS_TO_VIRT(phys)`）。模式设置通过可缓存直接映射清零新缓冲并 `wbinvd`，
之后才让 plane 指向它，保证显示引擎不会读到旧 DRAM 内容。

### 继承状态与拆除

`modeset_save()` 快照模式设置可能触碰的所有寄存器——六个转码器时序寄存器、
`PIPEACONF`、pipe/eDP 源尺寸与移位 pipe 配置、eDP M/N、`PIPE_MISC`、
`TRANS_DDI_FUNC_CTL`、`DP_TP_CTL`、两个 scaler、plane 的
control/stride/surf/offset/pos/size、背光与 watermark——以便后续某步失败时用
`modeset_restore()` 还原固件状态。拆除顺序是使能顺序的逆序：plane、pipe、
transcoder、DDI、PLL。`wait_pipe_off()` 先等两个 `PIPE_STATE` 位清零，再要求
`0x70040` 处的帧计数器在连续两次 40 ms 采样间都不再前进：pipe state 位在固件
仍在扫描时也会读回 0，所以帧计数器才是直接证据。确认失败则中止模式设置，而
不是在 pipe 还活着时去动时钟树。

### 使能管线

使能步骤，按顺序：

1. pipe 与两份 eDP 源尺寸——`PIPEASRC`、`TRANS_EDP_SRC`、`TRANS_EDP_PIPE_SRC`
   均写 `((hactive-1) << 16) | (vactive-1)`。
2. 转码器时序，写转码器 0（`0x60000`）与 eDP 转码器（`0x6F000`，即转码器 15）。
   `transcoder_timing()` 把每个寄存器的低 16 位写第一个边沿（active/start）、
   高 16 位写第二个边沿（total/end），与 i915 一致：`HTOTAL`/`HBLANK`、
   `HSYNC`、`VTOTAL`/`VBLANK`、`VSYNC` 都由模式的 active/blank/sync 推出。
3. DP M/N 与色彩深度：把固件的 `TRANS_EDP_DATA_M1/N1`、
   `TRANS_EDP_LINK_M1/N1` 与 `PIPE_MISC` 原样写回。`LINK_N1` 最后写，因为它
   负责武装双缓冲的 M/N 更新。
4. `DP_TP_CTL` 带 sink 协商的 enhanced framing 位与正常链路训练重新使能，然后
   依次使能 `DDI_BUF_CTL_A`（端口宽度 x1）、`TRANS_DDI_FUNC_CTL_EDP`，并把
   eDP MSA misc 寄存器 `0x6F410` 设为 `0x01`（8bpc、sync clock）。
5. eDP 转码器自己的移位 pipe 配置 `TRANS_EDP_PIPE_CONF`（`0x7F008`）先写禁用、
   再带 `PIPE_ENABLE` 使能，让状态机干净地跳转；`PIPEACONF` 同样处理，随后
   有界轮询 `PIPE_STATE` 确认 pipe 已启动。
6. 清零 pipe A 的两个 scaler（pipe 1 与 pipe 2 的
   `PS_CTRL`/`PS_WIN_POS`/`PS_WIN_SZ`），因为固件留下一个把 800x600 启动模式
   放大，其缩放相位会把原生源再缩放一次。
7. 把 plane 数据缓冲扩到整个显示缓冲
   （`PLANE_BUF_END(445) | PLANE_BUF_START(0)`），并把八个 watermark 与转换
   watermark 全设为 `PLANE_WM_EN | IGNORE_LINES | BLOCKS(32)`，避免全分辨率
   取数被饿死。

### Plane 编程

`plane_configure()` 先禁用 plane、清零 offset/pos，写
`PLANE_SIZE = ((vactive-1) << 16) | (hactive-1)` 与
`PLANE_STRIDE = pitch / 64`，再写
`PLANE_CTL = ENABLE | FORMAT_XRGB8888 | TILED_LINEAR`，最后写 `PLANE_SURF`
（GPU 地址）。control 写在 surface 之前，因为 surface 写入会在下一个 vblank 处
统一武装整组双缓冲 plane 状态（i915 的 `skl_plane_update_arm`）。pipe 与 plane
运行后，pipe 源尺寸、两份 eDP 源尺寸以及 `PLANE_SIZE`/`PLANE_SURF` 会在一个
vblank 处再写一遍：使能 eDP pipe 会把源尺寸重置回固件的小启动区域，使能前的
那次写入不会生效。

### Console 交接与扫描一致性

`gfx_attach_fb()` 让终端指向 GTT 对象（`addr`、`width`、`height`、`pitch`），
分配一块 `stride * height` 字节的私有并清零的 backbuffer，并调用
`term_update_size()` 按新几何重算字符网格。console 服务随后以
`pitch * height` 作为映射长度，用 write-combining 映射进自己的地址空间，并在
服务被创建之前就占据屏幕，使任何过期的内核刷新都无法覆盖它的首帧。由于内核
经可缓存直接映射访问 scan-out、而服务用 write-combining 映射同一块内存，
`fb_refresh()` 改用非临时存储（`movnti` + `sfence`）写 scan-out：非临时存储
从不分配 cache line，因此不会有过期脏行日后被回写、覆盖服务的画面。

### 模式设置前清屏

在 `skl_edp_set_mode()` 运行前，`gfx_init()` 把显示引擎当前正在扫描的帧缓冲
清成黑色并刷新显示 50 ms。对于“在新区块覆盖之前会一直送出上次收到的区域”的
面板，此时屏幕上已是空帧，没有固件内容可残留。

## 说明

- 本次为纯显示模式设置：保留 BIOS 已训练的 eDP 链路与显示核心时钟
  （CDCLK/LCPLL），因此换分辨率无需重训链路、也无需改 CDCLK。
- “内容只出现在左上 800x600”的原因是 pipe source 与 plane 窗口属于双缓冲且
  没有被武装，显示引擎于是继续沿用固件的小启动区域，尽管寄存器读回已是新尺寸。
- “移动的水波纹”是因为把固件的 18bpp data M/N 与 6bpc `PIPE_MISC` 覆盖成了
  24bpp 的值。
- 扫描用的帧缓冲是 GTT 缓冲区；模式设置成功后交给终端服务，失败则保留固件
  aperture 帧。

## 测试

- `make -C kernel clean && make -C kernel` 无警告，完整镜像可构建。
- QEMU 行为不变：驱动不匹配 QEMU 的 VGA 设备，不会走到模式设置，启动仍到达
  `name "hansh"`。
- Skylake/HD520 目标机：终端铺满原生 1366x768 面板、无水波纹；
  `skl_display_dump()` 可确认 pipe、plane 与 eDP 状态；某步失败则回退到固件
  帧缓冲。
