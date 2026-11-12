# 2026-11-11: GPU 驱动移入用户态

把 Intel 显示驱动移出内核。内核保留 Limine 帧缓冲用于启动画面，并把显示 BAR 授予新服务。

## 内核

- 设备选择留在 `kernel/srv/gpu_srv.c`。该文件把 GTTMMADR 与 GMADR 映射给 `/bin/gpu`，传入固件模式并注册 `SVC_GPU`。
- 内核在 `boot_display.c` 保留最小 eDP 引导：在 HNK 启动画面前切到原生分辨率，随后由用户态服务接管运行期路径。
- 删除内核 `gfx.c`、VBT、EDID、桌面、测试与光标代码。引导不包含合成器与运行期 plane。
- `gpu_reg.h` 与 `gpu_types.h` 由内核和用户态共用，不再保留两份寄存器与类型定义。
- `input_srv` 与 `xhci` 用 `GPU_CURSOR_MOVE` 把指针增量转发到 GPU 端点。
- `console_srv` 映射固件帧缓冲作为回退，并把 GPU 端点交给 `/bin/console`。

## 用户态

- 新增 `/bin/gpu`：GTT 写入、内存对象分配、rain 桌面与硬件光标。`bootinfo.gpu_display_ready` 表示内核已完成模式设置时，服务跳过自己的 eDP 模式设置。
- 用户态源码不再包含模式设置与 EDID 解析；除 plane 与桌面外，只保留光标编程。
- 桌面在 Gen9 universal plane 与 Gen7.5/Gen8 传统 primary/sprite plane 上运行。Gen7.5/Gen8 primary 用 `DSPOFFSET` 的 X/Y 坐标滚动。两代 sprite 只有 XRGB 格式，console 窗口保持不透明；CPU 不做整屏混合。
- GPU 服务在接触 plane 前分配并烘焙 rain 纹理与 console sprite，再应答 `GPU_CONSOLE_SURFACE`，用内存对象交付表面。应答带宽度、高度、stride 与桌面标志。
- `console` 启动时请求该表面，以写合并方式映射到 `0x21000000`。请求失败时回退到固件帧缓冲。
- 内存对象映射新增 `PROT_WC`。显示引擎读 DRAM，表面映射必须绕过缓存。

## 测试

- 内核与用户态构建无警告。
- 基础镜像与 ports 镜像都启动到 `hansh` 与 `bash`。
- QEMU 无支持的 Intel 显示设备，日志显示回退路径，console 在固件帧缓冲上启动。
