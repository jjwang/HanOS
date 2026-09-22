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
- `skl_display.c`：
  - 接管时按 `plane -> pipe -> transcoder -> DDI -> PLL` 逆序拆一次旧状态。
    `wait_pipe_off()` 确认 pipe 真的停了——pipe state 位作快路径，帧计数器作
    直接证据——确认失败则中止模式设置，而不是在 pipe 还活着时去动时钟树。
  - 保留固件的 eDP DP M/N 与 `PIPE_MISC`。面板是 6bpc（18bpp），固件的 data
    M/N 与色彩深度必须沿用；按 24bpp 重算 data M/N 会让 sink 失步，表现为
    横向移动的水波纹。
  - 在 `DP_TP_CTL` 中恢复 sink 协商好的 enhanced framing 位。
  - 编程 eDP 转码器的移位 pipe 配置（`0x7F008`）以及 pipe 和两份 eDP 源尺寸。
    源尺寸在 transcoder 时序之前写，并在 pipe 运行后于 vblank 处连同
    `PLANE_SIZE`/`PLANE_SURF` 一起补写，因为这些值是双缓冲、必须被武装
    （arm）后才生效。
  - 按 i915 顺序，把 `PLANE_CTL` 写在 `PLANE_SURF` 之前，让 surface 写入统一
    武装整组 plane 状态。
  - 对 1:1 原生模式，拆掉 pipe A 的两个 scaler，而不是保留固件用于放大的窗口。
- `gfx.c`：用 `gfx_attach_fb()` 把新帧缓冲交给终端（分配 backbuffer、更新终端
  几何），成功时提前返回，而不再继续使用固件 aperture 帧。
- `term.{c,h}`：新增 `term_update_size()`，交接后让字符网格跟随新帧缓冲。
- `fb.h` / `kmain.c`：去掉写死的 `FB_WIDTH`/`FB_HEIGHT` 上限；改为从终端帧
  缓冲报告实际扫描几何，而不是固件的帧缓冲结构。
- `skl_display_dump()`：收成精简的关键寄存器概要（时钟、pipe/plane 几何、
  eDP 链路/M/N、背光）。删除了大范围的逐区域扫描及其辅助函数。

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
