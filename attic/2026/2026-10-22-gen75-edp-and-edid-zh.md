# 2026-10-22：Haswell、Broadwell、Skylake 原生 eDP

## 结果

Haswell-ULT、Broadwell-U、Skylake 笔记本按面板原生模式启动。终端、游戏、硬件光标正常。内核按顺序选模式。顺序：Limine EDID、OpRegion EDID、OpRegion VBT、AUX EDID、固件 eDP 时序。

## 现象

- Haswell 报 `EDID size: 0`，停在 800x600。
- `dmesg edid` 报无可用来源。
- 首次原生尝试闪屏，随后花屏。
- 加入 Haswell 路径时，Skylake 回落到 800x600。

## 根因

### Haswell 没有 EDID

- Limine 未交 EDID。
- OpRegion ASLE 邮箱 #5 无面板 EDID。
- eDP AUX DDC 在地址 0x50 返回 I2C NACK。
- PCH SMBus 也读不到 EDID。

### 固件把原生时序留在 eDP 转码器

GOP 把 800x600 pipe 放大到面板。pipe 源保持 800x600。eDP 转码器保留原生时序。读转码器时序拿到面板模式。

### Haswell plane 寄存器与 Gen9 不同

- Gen7.5 的 `DSPSTRIDE` 单位是字节。Gen9 的 `PLANE_STRIDE` 单位是 64 字节。
- Gen7.5 主 plane 格式字段在 bit 29:26。Gen9 在 bit 27:24。

首次原生设置按 64 字节单位写 stride。格式字段也被截断。plane 按 85 字节行宽取数。面板显示花屏。

### AUX 时钟字段按代不同

- Gen7.5 要写 `PRECHARGE_2US(3)` 与 `BIT_CLOCK_2X(cdclk/2000)`。
- Gen9 由 CDCLK 推导时钟。Gen9 保留固件 sync 字段。

首版把 Gen7.5 字段写到两个平台。Skylake AUX 失效。模式设置失败。

### 光标寄存器

- `CURABASE` 是 0x70084。`CURAPOS` 是 0x70088。
- 光标调试时两寄存器写反。光标消失。
- `MCURSOR_GAMMA_ENABLE` 没有 gamma LUT。光标偏红。去掉后正常。

### EDID 选择

原逻辑只读 DTD0。新逻辑优先 EDID preferred 位。该位指向 DTD0。没有该位时扫描 4 个 DTD。取最高分辨率的非 interlaced 时序。此策略来自 `howto.txt` 与 i915。

## 死路

这些路径在 Haswell 笔记本上读不到面板时序：

- OpRegion 0x400 的 VBT。固件写 `$VBT`，BDB 是桩，块从 4 开始。
- 0xc0b60 的 VGA shadow VBT。同一份桩。
- PCI ROM。ROM 可映射可扫描，内部无 `$VBT`。
- GPU SPI 与 PCH SPI。PCH FREG 五个区都读 0x7fff7fff。
- 低内存与顶层 ROM 的缓存 EDID 扫描。
- PCH SMBus 0x50 的 byte 与 block 读。

这些路径的代码已删除。VBT 模块只留 i915 集成显卡路径。该路径用 OpRegion 与 ASLE.RVDA。

## 修复

### 模式选择

- 新增 `edid.c`、`edid_read.h`、`vbt.c`、`vbt.h`。
- 按 i915 顺序选模式。顺序：Limine EDID、OpRegion EDID、OpRegion VBT、AUX EDID、eDP 时序。
- AUX 读前给面板 VDD 上电。写 `DPCD 0x600 = D0`。
- 最后从 eDP 转码器读原生时序。

### 平台分支

- `gfx_generation()` 按设备 ID 分 Gen7.5、Gen8、Gen9。
- 列出 Haswell、Broadwell、Skylake 的 GT 设备 ID。
- Gen7.5 用 2 MB GTT 与 32 位表项。Gen8/Gen9 用 8 MB 与 64 位表项。
- Gen9 保留 AUX sync 字段。Gen7.5 写 bit clock 与 precharge。
- Gen7.5 plane stride 用字节。Gen9 用 64 字节单位。
- Gen7.5 plane 格式字段留在 bit 29:26。
- Gen7.5 跳过 Gen9 光标 watermark。也跳过 data buffer 寄存器。
- 恢复 `CURABASE` 与 `CURAPOS`。

## 改动文件

| 文件 | 改动 |
|------|------|
| `kernel/device/display/gfx.c` | 代际表、GTT 大小、模式来源顺序、eDP 时序 |
| `kernel/device/display/edid.c`、`edid_read.h` | EDID 校验、OpRegion 与 AUX 读取 |
| `kernel/device/display/vbt.c`、`vbt.h` | OpRegion/RVDA 的 VBT 面板时序 |
| `kernel/device/display/display_mode.c`、`.h` | 多 DTD EDID 选择 |
| `kernel/device/display/skl_display.c`、`.h` | Gen9/Gen7.5 分支、AUX 控制、光标 |
| `kernel/device/display/gfx_reg.h` | Gen7.5 时钟、AUX 字段、plane stride 注释 |
| `kernel/device/display/gfx.h` | 平台字段 |
| `kernel/kmain.c` | 接受扩展 EDID 长度 |
| `attic/2026/2026-10-22-gen75-edp-and-edid-en.md`、`-zh.md` | 本说明 |

## 测试

- 内核与用户态 clean 构建无 `-Wall -Wextra` 警告。
- QEMU 到 `hansh`，无 panic、exception、corrupt 行。
- Haswell-ULT：原生模式，终端与游戏正常。硬件光标可见。
- Broadwell-U：同一 Gen7.5 路径，原生模式。
- Skylake：EDID 给出原生模式。光标可见。
- `dmesg edid` 用一行报告所选来源。

## TODO

- 无 EDID 面板：eDP 时序取决于 GOP 模式。补 VBIOS/VBT 主体读取。覆盖不放大 pipe 的固件。
- 标准 timing：补 `howto.txt` 的 EDID 标准 timing 回退。
- 共享文件继续变大时，把 Gen7.5 modeset 拆到 `hsw_display.c`。
- 硬件测试后补其余 Skylake GT ID。
- 光标：在 Gen7.5 上核对 ARGB 通道顺序。
