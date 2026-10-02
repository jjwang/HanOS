#!/bin/bash
#
# Build musl for HanOS.
#
# The musl source lives in 3rdparty/musl (not tracked). The HanOS-specific
# overrides live in musl: a syscall backend that maps Linux syscall numbers to
# HanOS numbers and converts the return convention, and a __set_thread_area that
# uses SYSCALL_SET_FS_BASE.
#
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
MUSL="$ROOT/3rdparty/musl"

CC=${CC:-x86_64-elf-gcc}
AR=${AR:-x86_64-elf-ar}
RANLIB=${RANLIB:-x86_64-elf-ranlib}

if [ ! -d "$MUSL" ]; then
    echo "missing $MUSL (vendor musl 1.2.5 there)" >&2
    exit 1
fi

cp "$ROOT/musl/syscall_arch.h" "$MUSL/arch/x86_64/syscall_arch.h"
cp "$ROOT/musl/__set_thread_area.s" \
   "$MUSL/src/thread/x86_64/__set_thread_area.s"

cd "$MUSL"
make distclean >/dev/null 2>&1 || true
./configure --target=x86_64-hanos --disable-shared --enable-static \
    CC="$CC" CFLAGS='-mcmodel=large -mno-red-zone -Wno-return-local-addr'
make AR="$AR" RANLIB="$RANLIB" -j"$(nproc)"

echo "musl: $MUSL/lib/libc.a"
