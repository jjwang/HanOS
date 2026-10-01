# 2026-09-15：GPU 驱动清理加固

## 概述

加固 Intel 显示驱动，目标机 Skylake HD520。

- 修正芯片组与设备识别。
- 修复损坏注释。
- 合并开机自检。
- 修正寄存器偏移与日志格式。

## 变更

- `gfx.c`：扫描已枚举 PCI 设备查找 PCH，即 Sunrise Point（0x9D00）。不再假设 ISA bridge 位于 00:1f.0。重命名误名的 `DEVICE_SUNRISE_PANTHERPOINT`。只匹配 HD520（0x1916）。移除 Broadwell HD5500，它永远无法通过 Skylake PCH 检查。
- `gfx.c`：把损坏的 “Claude Code” 文本改为 `cursor`。修复 4 处注释，以及日志中的函数名 `gfx_configure_cursor()`。
- `gfx.c`：把开机自检移出 `gfx_init()`，放入原先无人调用的 `gfx_test_advanced_features()`。由 `gfx_init()` 调用。所有测试共用一个 force-wake 会话。`gfx_init()` 保留探测、BAR/GTT/显存初始化与 aperture framebuffer 后备缓冲。
- `gfx.c` 与 `gfx_reg.h`：修正 `gfx_set_plane_fb()`，把 surface 地址写入 `DSPxSURF`（偏移 0x1C），不再写 `DSPxLINOFF`（0x04）。新增 `PIPE_*` 与 `DSP_SURF`/`DSP_STRIDE` 宏。在 timing、plane 辅助函数中使用。
- `gfx.c`：32 位寄存器与数值改用 `%u`/`%x`，不再用 `%ld`/`%lx`。

## 变更文件

| 文件 | 变更 |
|------|------|
| `kernel/device/display/gfx.c` | 芯片组/设备识别、自检合并、寄存器偏移、日志格式 |
| `kernel/device/display/gfx_reg.h` | pipe timing 与 plane surface 宏 |

## 测试

- `make -C kernel clean && make -C kernel` 无警告，可构建启动镜像。
- QEMU 启动行为不变。驱动不匹配 QEMU VGA 设备，不访问寄存器，无 `GFX:` 输出。启动仍到达 `name "hansh"`。
- Skylake/HD520 目标机：核对设备与芯片组识别、GTT/stolen 参数与自检结果。
