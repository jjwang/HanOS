# 2026-09-17：Skylake eDP 模式设置

## 概述

迈向自适应分辨率的第二步：在主 eDP 端口（DDI-A）上按启动模式编程 Skylake
Gen9 显示管线，而不再只使用固件 aperture 帧缓冲。

## 变更

- `gfx_reg.h`：新增 Gen9 模式设置所需寄存器（显示核心电源井、CDCLK、DPLL
  状态、DDI buffer translation、DP 传输控制、transcoder DP 控制、通用平面
  字段与 `PIPE_MISC`）。
- 新增 `skl_display.{c,h}`：`skl_edp_set_mode()` 分配 GTT 帧缓冲，保留固件
  已建立的显示核心电源、CDCLK、DPLL0 与已训练的 eDP 链路，然后编程
  transcoder 时序、pipe 与 plane，打开面板与背光，并返回帧缓冲。
  `skl_display_dump()` 打印管线寄存器便于调试。所有硬件等待都有超时。
- `display_mode.{c,h}`：通过 `display_mode_get_boot()` 暴露启动模式。
- `gfx.c`：自检后打印显示状态并尝试模式设置；失败则保留固件 aperture 帧
  缓冲。

## 说明

- 本次为纯显示模式设置：保留 BIOS 已训练的 eDP 链路与显示核心时钟，避免
  重新做链路训练与改 CDCLK。
- 扫描用的帧缓冲是 GTT 缓冲区，填充灰色测试图案，便于确认模式设置成功；
  把它交给终端服务是下一步。

## 测试

- `make -C kernel clean && make -C kernel` 无警告，完整镜像可构建。
- QEMU 行为不变：驱动不匹配 QEMU 的 VGA 设备，不会走到模式设置（无 `GFX:` 输出），
  启动仍到达 `name "hansh"`。
- Skylake/HD520 目标机：核对 `skl_display_dump()` 的值、模式设置（屏幕显示
  灰色图案）以及某步超时后的回退。
