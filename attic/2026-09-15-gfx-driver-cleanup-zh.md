# 2026-09-15：GPU 驱动清理加固

## 概述

针对 Skylake HD520 目标机加固 Intel 显示驱动：修正芯片组与设备识别、修复被
破坏的注释、合并开机自检，并修正寄存器偏移与日志格式。

## 变更

- `gfx.c`：改为扫描已枚举的 PCI 设备查找 Sunrise Point（0x9D00）来识别 PCH，
  不再假设 ISA bridge 位于 00:1f.0；重命名误名的
  `DEVICE_SUNRISE_PANTHERPOINT`；只匹配 HD520（0x1916），移除永远无法通过
  Skylake PCH 检查的 Broadwell HD5500。
- `gfx.c`：把被破坏的 “Claude Code” 文本改回 `cursor`（4 处注释，以及日志里
  的函数名 `gfx_configure_cursor()`）。
- `gfx.c`：把开机自检从 `gfx_init()` 移入原本从未被调用的
  `gfx_test_advanced_features()`，并由 `gfx_init()` 调用；所有测试共用一次
  force-wake 会话。`gfx_init()` 保留探测、BAR/GTT/显存初始化以及 aperture
  framebuffer 后备缓冲区建立。
- `gfx.c` 与 `gfx_reg.h`：修正 `gfx_set_plane_fb()`，将 surface 地址写入
  `DSPxSURF`（偏移 0x1C）而不是 `DSPxLINOFF`（0x04）；新增 `PIPE_*` 与
  `DSP_SURF`/`DSP_STRIDE` 宏，并在 timing、plane 辅助函数中使用。
- `gfx.c`：32 位寄存器与数值改用 `%u`/`%x`，而非 `%ld`/`%lx`。

## 变更文件

| 文件 | 变更 |
|------|------|
| `kernel/device/display/gfx.c` | 芯片组/设备识别、自检合并、寄存器偏移、日志格式 |
| `kernel/device/display/gfx_reg.h` | pipe timing 与 plane surface 宏 |

## 测试

- `make -C kernel clean && make -C kernel` 无警告，完整镜像可构建。
- QEMU 启动行为不变：驱动不匹配 QEMU 的 VGA 设备，因此不做任何寄存器访问
  （无 `GFX:` 输出），仍可启动到 `name "hansh"`。
- Skylake/HD520 目标机：核对设备与芯片组识别、GTT/stolen 参数及自检结果。
