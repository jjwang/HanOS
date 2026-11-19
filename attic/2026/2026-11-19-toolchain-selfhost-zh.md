# 2026-11-19：自举 GCC 并重整工具链脚本

- 把工具链构建脚本移到 `3rdparty/toolchain/`。
- 构建 native GCC、GMP/MPFR/MPC 与共享 `libstdc++.so`。
- initrd 保持精简，工具链、ports 与游戏放入 ext2。
- 生成 GPT 混合引导的 `release/hdd.img`，BIOS 与 UEFI 都可启动。
