# 2026-11-20: Port CPython 3.8

- Build CPython 3.8.18 against the HanOS musl sysroot as a static interpreter.
- The x86_64-elf toolchain cannot emit shared objects, so link the selected extensions from `Modules/Setup.local` as builtins.
- Leave out modules that need OpenSSL, libffi, expat, zlib or a database.
- Stage the stdlib under `lib/python3.8` on the ext2 partition and copy the ports `lib` tree into the image.
- Skip the interpreter in the ISO initrd; the live image carries no data partition.
- Verify `python3 -V` prints `3.8.18` and `python3 -c "print(40+2)"` prints `42`.
