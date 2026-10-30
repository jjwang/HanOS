# 2026-10-30：GitHub Actions 启动测试与它暴露的两处故障

## 现象

GitHub Actions 启动任务反复失败。早期运行停在空串口日志。后期启动到 shell，但打印 `MK: selftest FAIL`。

## 背景

工作流跑在 `ubuntu-24.04`。它用宿主 GCC 包装器编译内核和用户态，生成 `cdrom.iso`，再用 QEMU 8.2 的 TCG 启动。启动脚本在串口日志里等待 `name "hansh"` 和 `MK: selftest PASS`。

## 根因

### 1. build-id 破坏 ELF 布局

Ubuntu 宿主 GCC 默认生成 `.note.gnu.build-id`。`kernel/linker.ld` 没有安放该段。`ld` 生成了与 text 段重叠的 data 段。Limine 拒绝加载内核，串口日志保持为空。裸机交叉 GCC 不生成该 note，开发机的故障被掩盖。

修复：链接内核时加 `-Wl,--build-id=none`（`c13d3ee`）。

### 2. 缺页入口改坏了 `%rax`

自检的容错拷贝检查失败。`copy_from_user` 在同一未映射地址上报 `EFAULT`。`copy_to_user` 却返回成功。插桩显示写确实缺页。处理器找到了 fixup。标志槽为 1。gdb 随后显示 fixup 处 `%rax = 0`，而写指令执行时 `%rax = len`。

`exc14` 入口在 `pusham` 保存现场前，借用 `%rax` 清 `%fs`。被打断的 `%rax` 变成 0。`copy_to_user` 把 `len` 留在 `%rax` 里跨过写指令。返回值算成 `0 + 8 - 8 = 0`。

修复：在 `isr_base.s` 里，改段寄存器前后保存并恢复 `%rax`（`9cf3a0a`）。

## 排查过程

- 宿主 GCC 包装器和 nolibc 交叉工具链都失败。nolibc GCC 16.2 的 `stdint.h` 转到宿主 glibc。内核 `-MM` 规则缺 `-ffreestanding`。
- 放弃交叉工具链。关掉 build-id 后，宿主 GCC 路径可用。
- 用宿主 GCC 10.5 在本机复现 CI 产物：ISO 9.85 MB，initrd 5.6 MB。修 build-id 前不启动，修后启动。
- 用 focal 工具链 PPA 的 GCC 13.1 在本机复现拷贝故障。该编译器能在开发机直接运行。gdb 观察写指令、fixup 和标志槽。

## 实施

- `c13d3ee fix(kernel): disable build-id so Limine loads the ELF`
- `956c795 ci: build and boot the ISO on GitHub Actions`
- `9cf3a0a fix(arch): preserve rax across the page-fault entry`
- `ac1cb94 test(kernel): simplify the uaccess self-test`
- `c873398 ci: print the serial log after a passing boot test`
- 自检探测 `0x00007ffff0000000`，避开 SMP 引导时恒等映射的低 1 MiB。
- 启动脚本在通过后打印串口日志。

## 验证

- GCC 13.1，TCG，`-smp 1`：`MK: selftest PASS`，shell 以 `hansh` 启动。
- 交叉 GCC 10.2，TCG，`-smp 1`：`MK: selftest PASS`，shell 以 `hansh` 启动。
