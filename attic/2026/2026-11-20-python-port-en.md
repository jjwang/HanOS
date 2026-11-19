# 2026-11-20: Port CPython 3.8 with shared modules

- Build CPython 3.8.18 against the HanOS musl sysroot with `--enable-shared`.
- Link the interpreter against `libpython3.8.so` and the shared musl libc.
- Build the extension modules as shared objects. The x86_64-elf driver drops `-shared`, so the ports wrapper passes `-Wl,-shared` to the linker.
- Fix `getrandom` to return the byte count; the CPython hash seed loops until the buffer fills.
- Place `libpython3.8.so` in the ports initrd. Keep the stdlib and `lib-dynload` on ext2 under `lib/python3.8`.
- Verify `python3 -c "print(40+2)"` prints `42`.
