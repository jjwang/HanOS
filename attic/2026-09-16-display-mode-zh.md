# 2026-09-16：显示模式模型

## 概述

迈向自适应分辨率的第一步：从 EDID 首选详细时序（缺失时回退到当前帧缓冲几何）
推导出完整的显示时序模型并打印。本步尚不重新编程显示。

## 变更

- 新增 `kernel/device/display/display_mode.{c,h}`：`display_mode_t` 保存像素
  时钟、水平/垂直的 active/blank/sync（偏移与脉宽）、同步极性、隔行标志、
  行 pitch 与刷新率。`display_mode_from_edid()` 解析 EDID 详细时序描述符 1
  （含 MSB 半字节与 sync MSB 位），`display_mode_from_fb()` 提供仅几何的回退，
  `display_mode_log()` 打印结果。
- `kernel/kmain.c`：在 EDID/帧缓冲处理之后，构建并打印启动显示模式（优先
  EDID 首选时序，否则使用帧缓冲几何）。

## 变更文件

| 文件 | 变更 |
|------|------|
| `kernel/device/display/display_mode.{c,h}` | 显示时序模型 |
| `kernel/kmain.c` | 构建并打印启动显示模式 |

## 测试

- `make -C kernel clean && make -C kernel` 无警告，完整镜像可构建。
- QEMU 将其 EDID 解析为 `MODE: 1024x768 @ 74 Hz (source: EDID)`，像素时钟
  82290 kHz，pitch 4096 字节，H/V 同步值解析正确；启动仍到达
  `name "hansh"`，无 panic。
