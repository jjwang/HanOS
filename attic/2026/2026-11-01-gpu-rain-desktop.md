# 2026-11-01: Skylake GPU 雨桌面

本文描述 HanOS 在 Intel Skylake（Gen9）上的显示通路。桌面分三层：primary 平面扫描烘焙好的雨纹理；sprite 平面放半透明终端；光标平面在最上层。雨纹理只烘焙一次。每帧 CPU 只写一次 plane offset 和 surface 地址，节拍来自管道帧计数器。无 Gen9 时回退 primary console。

## 1. 结果

- 模式：eDP（embedded DisplayPort）原生模式。参考机 1366×768。
- primary：1366×3850 纹理，周期 3080 px。
- sprite：1088×620 终端窗口，位于 139,74。
- 状态栏：20 px。显示版本、时间与 `rain:gpu`。
- 终端背景：80% 不透明。文字与光标不透明。
- 雨速：8 px/帧，480 px/s。
- 回退：无 GPU 或非 Gen9 时保留 primary console。

## 2. 显示通路

`kernel/device/display/skl_display.c` 配置 eDP 通路：pipe A、EDP transcoder、DDI-A（Digital Display Interface A）。它保留固件训练好的链路与 PLL（phase-locked loop），重写时序，再使能 plane 1。桌面沿用该模式的几何与 primary 平面。

显示引擎按 pipe 合成已使能的平面。Pipe A 的 Gen9 通用平面寄存器：

| 平面 | 控制 | 步长 | 位置 | 尺寸 | 表面 | 偏移 |
|------|------|------|------|------|------|------|
| 1 A | 0x70180 | 0x70188 | 0x7018C | 0x70190 | 0x7019C | 0x701A4 |
| 2 A | 0x70280 | 0x70288 | 0x7028C | 0x70290 | 0x7029C | 0x702A4 |

光标用 `CURACNTR`、`CURAPOS`、`CURABASE`。光标是 64×64 ARGB 表面。

平面控制字段：

- bit 31：使能。
- bit 27:24：格式。值 4 是 32bpp XRGB/BGRX。
- bit 5:4：Alpha 模式。值 2 是软件预乘（SW premultiply）。
- bit 12:10：平铺。值 0 是线性。

Gen9 的步长字段以 64 字节为单位。Surface 字段是 32 位 GPU 地址。偏移字段低 16 位是 X，高 16 位是 Y。写 Surface 会装配整个双缓冲平面状态。下一次 vblank 生效。

## 3. 显示数据缓冲

SKL 每 pipe 有 512 块显示数据缓冲（DDB）。每块 512 字节。桌面分配：

| 平面 | DDB 块 |
|------|--------|
| primary | 0..445 |
| cursor | 446..477 |
| console sprite | 478..511 |

约束：

- primary 不得小于 mode set 的 445 块。1366 宽扫描输出会欠载。管道与 vblank 停止。雨停在首帧。
- cursor 占 446..477。sprite 从 478 开始。sprite 从 446 开始会与 cursor 抢占同一 DDB。
- sprite 只有 34 块。水位 16 块。窗口行宽 4352 字节，合 8.5 块。水位加行宽不超过 34 块。
- primary 水位 32 块。cursor 水位 32 块。

欠载时屏幕出现花屏、右黑或半幅黑。日志显示 `GFX: Timeout waiting for vblank on pipe 0`。vblank 状态位不置位。

## 4. 雨纹理

纹理宽等于屏幕宽。高等于 `(period_rows + screen_rows) × font_h`。字体是内核 PSF1（PC Screen Font），参考机字高 14。

参考机参数：

| 项 | 值 |
|----|----|
| 屏幕行数 | 55 |
| 周期行数 | 4 × 55 = 220 |
| 纹理高 | (220 + 55) × 14 = 3850 |
| 周期像素 | 220 × 14 = 3080 |
| 纹理字节 | 1366 × 3850 × 4，21 MB |

列结构：每列有随机起点。列内循环放 streak。streak 长 3..16 行。间隔 10..59 行。间隔随机，画面断续。头部在更高行，尾部在头部上方。头部亮，尾部按距离衰减。字符取自 `0/1/2/.../#$%&` 集合。字符由列种子与行号散列得到。

周期尾端复制首屏。复制区覆盖 `period_rows..period_rows+screen_rows`。平面偏移在 0..3080 之间回绕。回绕点内容相同，接缝无跳变。

滚动方向：偏移递减。纹理内容向下移动。每帧减 8 px。周期 6.4 秒。

烘焙只做一次。之后每帧 CPU 只读帧计数器、写 offset 与 surface。显示引擎负责取数、合成与扫描输出。

## 5. GTT（图形地址转换表）内存

小缓冲用 `gfx_fb_alloc()` 分配连续物理页。参考尺寸：终端 sprite 1088×620，2.7 MB。

雨纹理 21 MB。开机后连续物理页不可靠。`gfx_fb_alloc_scatter()` 逐页分配：

- 每页调用 `pmm_get(1)`。
- 用 64 位 GTT entry 把物理页映射到连续 GPU 地址。
- 在 `gfx_fb_t.pages` 保存物理页表。
- CPU 经 direct map 逐页写入。`layer_pixel()` 按页拆分偏移。
- 写完 GTT 回读首末 entry。显示引擎随后取数。
- `gfx_fb_free()` 识别 `pages`，清 GTT，逐页 `pmm_free()`。

共享 GTT 范围从 64 MB 到 256 MB。参考机分配：

| 对象 | GPU 地址 |
|------|----------|
| console framebuffer | 0x04001000 |
| cursor | 0x04409000 |
| rain texture | 0x0440D000 |
| console sprite | 0x05843000 |

## 6. vblank 与节拍

显示中断走 GPU 的 PCI INTx（传统中断线）。内核读取 PCI 配置 0x3C 的中断线。参考机是线 11。

中断通路：

- `ioapic_route_gsi()` 写 I/O APIC 重定向项。
- `irq_route_vector()` 把线映射到显式向量。桌面用 `0x30`。
- IDT（interrupt descriptor table）为 PCI 向量 13..23 建立门。向量 0x30 对应 `irq16` 桩。
- 处理顺序：屏蔽 GSI。注册处理函数。路由与解掩码。使能显示中断。

Gen8+ 的 pipe vblank 在 per-pipe 寄存器：

| 寄存器 | 地址 | vblank 位 |
|--------|------|-----------|
| `GEN8_DE_PIPE_IIR(0)` | 0x44408 | bit 0 |
| `GEN8_DE_PIPE_IMR(0)` | 0x44404 | bit 0（0 为解掩码） |
| `GEN8_DE_PIPE_IER(0)` | 0x4440C | bit 0 |

IIR 是中断标识寄存器。IMR 是屏蔽寄存器。IER 是使能寄存器。

还要置 `GEN8_MASTER_IRQ`（0x44200）的 bit 31。旧的 `DEIER`/`DEIMR`/`DEIIR` 是 Haswell 兼容路径。其 pipe A vblank 是 bit 7。Ivy Bridge 的 bit 0 是 FIFO underrun。位选错时 `IIR` 永远为 0。

中断只留 pipe A vblank。GT（graphics）、misc、port 源都屏蔽。共享 INTx 不会因其他源一直拉高而死循环。

桌面雨不依赖 vblank 状态位。sprite 活跃时 per-pipe vblank IIR 可能一直为 0。雨线程改用 pipe 帧计数器 0x70040：

```c
last = gfx_pipe_frame();
for (;;) {
    sched_sleep(1);
    now = gfx_pipe_frame();
    if (now == last)
        continue;
    last = now;
    scroll -= 8;
    if (scroll < 0)
        scroll = period;
    write(PLANE_OFFSET_1_A, scroll << 16);
    write(PLANE_SURF_1_A, texture);
}
```

`gfx_wait_vblank_frame()` 仍供测试用。它先等中断计数。超时后关闭 vblank 中断，改轮询 IIR。轮询超时是 50 ms。PIT（programmable interval timer）tick 是 1 ms。旧代码把次数写成 50000，实为 50 秒。

## 7. sprite 与半透明

sprite 平面是终端窗口。窗口占屏幕五分之四。参考机 1088×620，位于 139,74。顶部 20 px 是状态栏。

console 服务把终端画进自己的 GTT 表面。内核把该表面配到 plane 2：

- 控制：使能、XRGB、软件预乘（SW premultiply）、线性。
- 步长：sprite 步长 / 64。
- 位置：`(y << 16) | x`。
- 尺寸：`(h - 1) << 16 | (w - 1)`。
- DDB：478..511。水位 16。

``PLANE_CTL_ALPHA_SW_PREMULTIPLY`` 要求源像素已预乘。console 的 `premul(rgb, a)` 完成该步：

```c
r = ((rgb >> 16) & 0xff) * a / 255;
g = ((rgb >> 8) & 0xff) * a / 255;
b = (rgb & 0xff) * a / 255;
return (a << 24) | (r << 16) | (g << 8) | b;
```

Alpha 约定：

| 元素 | Alpha |
|------|-------|
| 终端背景 | 0xCC（80%） |
| 状态栏 | 0xD0（82%） |
| 文字 | 0xFF |
| 块光标 | 0xFF |
| 回退 console | 0xFF |

`render_cell()` 先处理 reverse 与 conceal，再加 alpha。前景固定不透明。背景用 `BG_ALPHA`。这样反显单元也保持文字清晰。

sprite 在 primary 之上。光标在 sprite 之上。雨从半透明背景与状态栏后面透出。

无桌面时 console 在 primary。primary 是 XRGB。Alpha 字节被忽略。回退保持不透明。

## 8. console 接入

内核 `gfx_desktop_start()` 负责桌面：

1. 计算终端几何。`cols = (screen_w / 8) * 4 / 5`，`rows = (screen_h / 15) * 4 / 5`。
2. 分配 scatter 雨纹理。烘焙。
3. 分配终端 sprite。尺寸 `cols × 8` 与 `rows × 15 + 20`。
4. 写 DDB：primary 0..445、cursor 446..477、sprite 478..511。
5. 配 primary：表面为雨纹理，偏移为周期像素。
6. 配 sprite：表面为终端缓冲，位置居中。
7. 启动 `gfxrain` 内核线程。
8. 标记桌面活跃。

`console_server_start()` 检测桌面。桌面活跃时，spawn hook 把 sprite 的物理页映射到 console 地址空间。映射带 WC（write-combining）与 USER。`bootinfo` 填：

- `fb_vaddr`：sprite 内核虚拟地址。
- `fb_width`、`fb_height`、`fb_pitch`：sprite 几何。
- `fb_size`：sprite 字节数。
- `desktop`：1。

console 读取 `bootinfo.desktop`。桌面分支：

- `cols = fb_w / 8`。
- `rows = (fb_h - 20) / 15`。
- `term_x = 0`，`term_y = 20`。
- 顶部画状态栏。内容为 `uname` 的 release、`clock_gettime` 的时间与 `rain:gpu`。
- 终端区喂 libvterm。单元带 alpha 渲染。
- 33 ms 循环。每秒刷新状态栏。光标每 500 ms 闪烁。
- 刷新脏行到 sprite。

回退分支保持旧 console：

- 网格占屏幕中间五分之四。
- 边距放 CPU 雨。
- 背景不透明。

console 重启由 svc monitor 负责。重启后 spawn hook 重新映射同一 sprite。plane 2 不变。雨线程继续。

内核与 tty 的字节仍走 `CONSOLE_WRITE_TAG`。console 把字节喂 vterm。内核日志环由 `conflush` 线程转发。

## 9. 启动顺序

1. `gfx_init()`：eDP mode set。primary 指向 console framebuffer。
2. `gfx_vblank_irq_start()`：路由 GPU 中断线，注册处理函数，使能 pipe A vblank。
3. `kshell` 调 `gfx_desktop_start()`：分配纹理与 sprite，配三层平面，启动雨线程。
4. `console_server_start()`：映射 sprite，启动 console。
5. `/bin/init` 启动 shell。

无 Gen9 或无 GPU：

- `gfx_desktop_start()` 返回 false。
- `console_server_start()` 走 primary。
- console 用旧边距雨。
- `gfx layers`、`gfx scroll`、`gfx solid`、`gfx flip` 在桌面活跃时被拒绝。

QEMU 无 Gen9。CI 启动测试走回退路径。shell 标记与内核自检保持不变。

## 10. 接口

内核显示接口（`kernel/device/display/gfx.h`）：

| 接口 | 作用 |
|------|------|
| `gfx_desktop_start()` | 建立常驻雨桌面 |
| `gfx_desktop_active()` | 查询桌面状态 |
| `gfx_desktop_console_fb()` | 返回 console sprite 表面 |
| `gfx_fb_alloc()` / `gfx_fb_free()` | 连续 GTT 表面 |
| `gfx_fb_alloc_scatter()` | 分散页雨纹理 |
| `gfx_flip_to()` | 写 plane 1 表面 |
| `gfx_wait_vblank_frame()` | 等一帧 |
| `gfx_pipe_frame()` | 读 pipe A 帧计数器 |
| `gfx_status()` 命令 | 打印 vblank、IIR、IER、IMR、master、帧计数 |

`bootinfo` 的 `desktop` 字段告诉 console 走桌面还是回退。

console 常量（`userspace/servers/console.c`）：

| 常量 | 作用 |
|------|------|
| `STATUS_H` | 状态栏高度，20 px |
| `BG_ALPHA` | 终端背景 alpha，0xCC |
| `STATUS_ALPHA` | 状态栏 alpha，0xD0 |

shell 命令：

- `gfx status`：读取显示状态。
- `gfx flip`、`gfx layers`、`gfx scroll`、`gfx solid`：调试用例。桌面活跃时返回 `desktop is active`。

## 11. 已知限制

- 雨是烘焙纹理滚动。所有列同一速度。无实时随机。
- 纹理周期 4 屏。1366×768 下 6.4 秒重复一次。
- 单 sprite 平面。终端窗口占屏幕五分之四。
- sprite DDB 只有 34 块。窗口越大，行宽越大，欠载风险越高。
- primary DDB 固定 445 块。缩小会停管道与 vblank。
- vblank 的 per-pipe IIR 在 sprite 活跃时可能不置位。桌面雨靠帧计数器。
- vblank 中断仍供 `gfx status` 与调试用例使用。
- 无 GPU render engine。雨不是 shader 生成。
- 半透明用软件预乘（SW premultiply）。终端背景 80%。状态栏 82%。回退不透明。
- 状态栏版本与时间来自 misc 服务。misc 起来前显示 `00:00:00`。
- 雨纹理 21 MB。1920×1080 下 42 MB。
- 全屏程序在 sprite 内运行。雨仍从半透明背景后面透出。

## 12. 验证

QEMU：

- `make` 生成 `cdrom.iso` 与 `release/hdd.img`。
- `scripts/ci-boot-test.sh` 走回退路径。
- 检查 `name "hansh"`、四个 `MK:` 标记、无 `panic`、`exception`、`corrupt`。

Skylake：

- 启动后应见全屏雨、居中半透明终端、顶部状态栏、硬件光标。
- shell 可执行 `ls`、`dmesg`、`crashtest`。
- 游戏在终端内运行，雨从背景透出。
- `dmesg GFX` 应见：

```text
GFX: desktop: rain 1366x768 period 3080, console 1088x620 at 139,74
GFX: desktop: rain thread running
GFX: desktop: rain offset ...
console: attached fb ... 1088x620 ... sprite
```

- `gfx status` 读取 vblank/IIR/IER/IMR/master 与帧计数。
- 雨速、密度与透明度可调：
  - `LAYER_SCROLL_SPEED`：4、8、12 对应 240、480、720 px/s。
  - 纹理 streak 间隔：密度。
  - `BG_ALPHA`、`STATUS_ALPHA`：透明度。

## 13. 文件

- `kernel/device/display/gfx.c`、`gfx.h`、`gfx_reg.h`：显示驱动、平面、GTT、桌面。
- `kernel/device/display/skl_display.c`：eDP mode set、光标。
- `kernel/device/display/fb.c`、`fb.h`：启动帧缓冲与字体。
- `kernel/srv/console_srv.c`：sprite 映射、console 启动。
- `kernel/kmain.c`：启动顺序。
- `kernel/proc/syscall.c`：`gfx` 命令与限制。
- `userspace/servers/console.c`：状态栏、终端、alpha、回退。
- `include/bootinfo.h`：`desktop` 字段。
- `kernel/arch/x64/{isr_base.s,idt.c,ioapic.c}`：PCI 中断向量与 I/O APIC。
