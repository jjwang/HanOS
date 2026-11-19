# 2026-11-19: Self-host GCC and reorganise the toolchain scripts

- Move the toolchain build scripts to `3rdparty/toolchain/`.
- Build native GCC, GMP/MPFR/MPC and a shared `libstdc++.so`.
- Keep the initrd minimal and place the toolchain, ports and games on ext2.
- Build a GPT hybrid `release/hdd.img` that boots on BIOS and UEFI.
