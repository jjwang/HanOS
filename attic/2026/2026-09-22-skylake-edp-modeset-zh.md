# 2026-09-22：Skylake eDP 模式设置

## 概述

在 DDI-A 主 eDP 端口按启动模式编程 Skylake Gen9 显示管线。把扫描帧缓冲交给 console 服务。修复 HD520 目标机的两个问题：

- 面板出现移动水波纹。
- 画面锁在固件 800x600 源区域。

## 变更

- `gfx_reg.h`：新增并修正 Gen9 模式设置寄存器。含 eDP 转码器时序、源尺寸副本与 DP M/N；eDP 转码器移位 pipe 配置（`0x7F008`）；pipe A 两个 scaler；`DC_STATE_EN`；`DP_TP_CTL` 的链路训练与 enhanced framing 位；plane 的 data buffer 与 watermark 字段；AUX 控制字段。
- `skl_display.c`：接管时按逆序拆一次旧管线。保留固件的 eDP M/N 与色彩深度。按规定顺序使能转码器与 pipe。拆掉固件的面板 scaler。在 vblank 处补写双缓冲的源尺寸与 plane 状态。
- `gfx.c`：用 `gfx_attach_fb()` 把新帧缓冲交给终端。分配 backbuffer，更新终端几何。成功时提前返回，不再沿用固件 aperture 帧。模式设置前清黑当前扫描帧。
- `term.{c,h}`：新增 `term_update_size()`。交接后字符网格跟随新帧缓冲。
- `fb.c`：用非临时存储刷 scan-out。内核不留脏 cache line 给 console 服务。
- `fb.h` / `kmain.c`：去掉写死的 `FB_WIDTH`/`FB_HEIGHT` 上限。从终端帧缓冲报告扫描几何，不用固件帧缓冲结构。
- `skl_display_dump()`：收成精简的关键寄存器概要，含时钟、pipe/plane 几何、eDP 链路 M/N、背光。删除逐区域大范围扫描及其辅助函数。

## 实现细节

### 原理

- 显示通路是一条链（`plane -> pipe -> transcoder -> DDI -> PLL`）。从源头向外使能，逆序拆除。动时钟树前确认 pipe 已停，帧计数器不再前进。否则变化时钟会驱动仍在扫描的 pipe。
- 分辨率与链路训练彼此独立。分辨率决定 pipe/transcoder 时序，即 pixel clock。链路训练决定 symbol clock 与链路速率。仅改分辨率无需重训，i915 fastset 也这样选择。只有链路速率或通道数变化才需重训。
- 源尺寸与 plane 窗口为双缓冲。仅在 vblank 武装后生效。pipe 运行后须补写一次。
- 面板为 6bpc。固件的 data M/N 与色彩深度给链路定速，必须保留。按 24bpp 重算会让 sink 失步。
- 显示引擎从 DRAM 取数。安排 CPU 侧写入者，不留缓存副本日后回写覆盖 scan-out。

### 操作

接管时执行一次，从固件画面起：

1. 逆序拆旧管线：plane、pipe、transcoder、DDI。动任何时钟前确认 pipe 已停。
2. 按本次改动配置时钟与链路：CDCLK、按新链路速率的 PLL、`DDI_BUF_TRANS`、AUX 训练（CR -> CE）。换分辨率无需这些，保留固件 CDCLK、PLL 与训练。
3. 正序开新管线：`DDI_BUF_CTL`、transcoder、pipe、plane。在 vblank 处补写双缓冲源尺寸与 plane 状态。
4. 把扫描帧缓冲交给 console。停止内核终端对 scan-out 的写入。

下面各节给出各部分的寄存器级细节。

### 帧缓冲分配与几何

- `skl_edp_set_mode()` 自行计算扫描几何，不复用启动帧缓冲。
- 计算 `pitch = (hactive * 4 + 63) & ~63`。1366 像素 → 5464 → 5504，每行起始落在 64 字节边界，满足 `PLANE_STRIDE` 要求。
- 计算 `fbsize = pitch * vactive`。
- `gfx_alloc()` 从 `pmm_get` 取 `ceil(fbsize / 4096)` 个物理连续页。
- 把共享 GPU 地址对齐到要求值。
- 通过 `gfx_gtt_map()` 逐页写 GTT 表项，每个 4 KiB GPU 页指向对应物理页。
- 返回 plane 使用的 GPU 地址与内核使用的 CPU 地址（`PHYS_TO_VIRT(phys)`）。
- 模式设置经可缓存直接映射清零新缓冲。
- plane 指向它之前用 `wbinvd` 刷出。显示引擎不会读到旧 DRAM 内容。

### 继承状态与拆除

`modeset_save()` 快照模式设置可触碰的寄存器。

- 快照覆盖六个转码器时序寄存器与 `PIPEACONF`。
- 快照覆盖 pipe/eDP 源尺寸、移位 pipe 配置、eDP M/N 与 `PIPE_MISC`。
- 快照覆盖 `TRANS_DDI_FUNC_CTL`、`DP_TP_CTL` 与两个 scaler。
- 快照捕获 plane 的 control/stride/surf/offset/pos/size、背光与 watermark。

某步失败时，`modeset_restore()` 还原固件状态。拆除顺序为 enable 逆序：plane、pipe、transcoder、DDI、PLL。`wait_pipe_off()` 先等两个 `PIPE_STATE` 位清零。再要求 `0x70040` 帧计数器在连续两次 40 ms 采样间不前进。固件仍在扫描时 state 位也读回 0，帧计数器是直接证据。确认失败则中止模式设置，不在 pipe 存活时动时钟树。

### 使能管线

使能步骤按顺序：

1. 写 pipe 与两份 eDP 源尺寸。`PIPEASRC`、`TRANS_EDP_SRC`、`TRANS_EDP_PIPE_SRC` 均写 `((hactive-1) << 16) | (vactive-1)`。
2. 写转码器 0（`0x60000`）与 eDP 转码器（`0x6F000`，转码器 15）时序。`transcoder_timing()` 低 16 位写第一个边沿（active/start），高 16 位写第二个边沿（total/end），与 i915 一致。`HTOTAL`/`HBLANK`、`HSYNC`、`VTOTAL`/`VBLANK`、`VSYNC` 均由模式 active/blank/sync 推出。
3. 原样写回固件的 `TRANS_EDP_DATA_M1/N1`、`TRANS_EDP_LINK_M1/N1` 与 `PIPE_MISC`，恢复 DP M/N 与色彩深度。`LINK_N1` 最后写，它负责武装双缓冲 M/N 更新。
4. 带 sink 协商的 enhanced framing 位与正常链路训练重新使能 `DP_TP_CTL`。再使能 `DDI_BUF_CTL_A`（端口宽度 x1），再 `TRANS_DDI_FUNC_CTL_EDP`。最后把 eDP MSA misc 寄存器 `0x6F410` 设为 `0x01`（8bpc、sync clock）。
5. 先禁用再使能 `TRANS_EDP_PIPE_CONF`（`0x7F008`）。第二次带 `PIPE_ENABLE`，状态机干净跳转。`PIPEACONF` 也先禁用再使能。有界轮询 `PIPE_STATE` 确认 pipe 已启动。
6. 清零 pipe A 两个 scaler（pipe 1 与 pipe 2 的 `PS_CTRL`/`PS_WIN_POS`/`PS_WIN_SZ`）。固件留下一个 scaler 放大 800x600 启动模式。否则会再缩放原生源一次。
7. 把 plane 数据缓冲扩到整个显示缓冲（`PLANE_BUF_END(445) | PLANE_BUF_START(0)`）。八个 watermark 与转换 watermark 均设为 `PLANE_WM_EN | IGNORE_LINES | BLOCKS(32)`，避免全分辨率取数饿死。

### Plane 编程

`plane_configure()` 先禁用 plane，清零 offset/pos。写 `PLANE_SIZE = ((vactive-1) << 16) | (hactive-1)` 与 `PLANE_STRIDE = pitch / 64`。再写 `PLANE_CTL = ENABLE | FORMAT_XRGB8888 | TILED_LINEAR`。最后写 `PLANE_SURF`（GPU 地址）。control 写在 surface 前。surface 写入在下一个 vblank 统一武装整组双缓冲 plane 状态（i915 的 `skl_plane_update_arm`）。pipe 与 plane 运行后，pipe 源尺寸、两份 eDP 源尺寸与 `PLANE_SIZE`/`PLANE_SURF` 在一个 vblank 再写一遍。使能 eDP pipe 会把源尺寸重置回固件小启动区域。使能前的写入不生效。

### Console 交接与扫描一致性

`gfx_attach_fb()` 让终端指向 GTT 对象（`addr`、`width`、`height`、`pitch`）。分配一块 `stride * height` 字节私有并清零的 backbuffer。调用 `term_update_size()` 按新几何重算字符网格。console 服务以 `pitch * height` 作为映射长度。用 write-combining 映射进自身地址空间。服务创建前占据屏幕。过期内核刷新无法覆盖它的首帧。内核经可缓存直接映射访问 scan-out，服务用 write-combining 映射同一内存。`fb_refresh()` 改用非临时存储（`movnti` + `sfence`）写 scan-out。非临时存储从不分配 cache line。不会有过期脏行日后回写覆盖服务画面。

### 模式设置前清屏

`skl_edp_set_mode()` 运行前，`gfx_init()` 把显示引擎当前扫描的帧缓冲清黑，并刷新 50 ms。面板在新区块覆盖前一直送出上次区域。此时屏幕已是空帧，无固件内容残留。

## 说明

- 本次为纯显示模式设置。保留 BIOS 已训练的 eDP 链路与显示核心时钟（CDCLK/LCPLL）。换分辨率无需重训链路，也无需改 CDCLK。
- 内容只出现在左上 800x600，原因是 pipe source 与 plane 窗口为双缓冲且未武装。显示引擎沿用固件小启动区域。寄存器读回已是新尺寸。
- 移动水波纹的原因是覆盖了固件 18bpp data M/N 与 6bpc `PIPE_MISC`，改成 24bpp 值。
- 扫描帧缓冲是 GTT 缓冲区。模式设置成功后交给终端服务，失败则保留固件 aperture 帧。

## 测试

- `make -C kernel clean && make -C kernel` 无警告，可构建启动镜像。
- QEMU 行为不变。驱动不匹配 QEMU VGA 设备，不走到模式设置，启动仍到达 `name "hansh"`。
- Skylake/HD520 目标机：终端铺满原生 1366x768 面板、无水波纹。`skl_display_dump()` 确认 pipe、plane 与 eDP 状态。某步失败则回退到固件帧缓冲。
