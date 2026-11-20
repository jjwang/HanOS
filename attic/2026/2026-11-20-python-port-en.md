# 2026-11-20: Port CPython 3.8 with shared modules

- Build CPython 3.8.18 against the HanOS musl sysroot with `--enable-shared`.
- Link the interpreter against `libpython3.8.so` and the shared musl libc.
- Build the extension modules as shared objects. The x86_64-elf driver drops `-shared`, so the ports wrapper passes `-Wl,-shared` to the linker.
- Fix `getrandom` to return the byte count; the CPython hash seed loops until the buffer fills.
- Skip a DSO without a hash table in the ldso symbol lookup; the vdso carries neither a SysV nor a GNU hash.
- Drop the kernel mmap file cache when an fd closes or a process exits; a reused fd returned stale file contents.
- Keep the stdlib and `lib-dynload` on ext2 under `lib/python3.8`.
- Build with `MACHDEP=hanos` so `sys.platform` and `sysconfig` report HanOS; the Linux host triple stays for the configure checks.
- Verify `python3 -c "print(40+2)"` prints `42` and `import math` loads the shared module.
- Treat Ctrl-D as VEOF in the tty so the interactive interpreter exits on end of file.
- Deliver canonical input only at end of line so backspace stays in sync with the key buffer.
- Map the Delete key to erase the last character.
- Implement chmod for ext2 and runtime files, so a C++ build marks its output executable.
- Route `fchmodat` to chmod, so the coreutils `chmod` command changes the mode.
- Refuse to execute a file without an execute bit, and report EACCES; a missing file reports ENOENT.
- Fill st_blksize and st_blocks for runtime and ext2 files, so ls -l reports a total.
- Add a free command that reports total, used and free memory from sys_mem_stats.
