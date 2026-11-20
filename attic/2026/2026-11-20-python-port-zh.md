# 2026-11-20：移植 CPython 3.8 并使用共享模块

- 用 HanOS 的 musl sysroot 构建 CPython 3.8.18。配置开启 `--enable-shared`。
- 解释器动态链接 `libpython3.8.so` 与共享 musl libc。
- 扩展模块构建为共享对象。x86_64-elf 驱动丢弃 `-shared`。ports 包装器改为传 `-Wl,-shared`。
- 修正 `getrandom` 返回字节数。CPython 的哈希种子循环等待缓冲区填满。
- ldso 符号查找跳过没有哈希表的 DSO。vdso 既无 SysV 也无 GNU 哈希。
- 关闭 fd 或进程退出时丢弃内核 mmap 文件缓存。复用的 fd 会读到旧文件内容。
- 标准库与 `lib-dynload` 放在 ext2 的 `lib/python3.8`。
- 构建用 `MACHDEP=hanos`。`sys.platform` 与 `sysconfig` 报告 HanOS。Linux host 三元组保留给 configure 检测。
- 验证：`python3 -c "print(40+2)"` 输出 `42`。`import math` 加载共享模块。
- tty 把 Ctrl-D 当作 VEOF。交互解释器读到文件结束就退出。
- 规范模式只在一行结束时交付输入。退格与按键缓冲保持一致。
- Delete 键按退格处理，删除最后一个字符。
- 为 ext2 与运行时文件实现 chmod。C++ 编译输出带执行位。
- 把 `fchmodat` 接到 chmod。coreutils 的 `chmod` 命令能修改权限。
- 拒绝执行没有执行位的文件，返回 EACCES。文件缺失返回 ENOENT。
