#!/bin/bash
#
# Build the shared musl loader/libc for HanOS.
#
# binutils 2.35 BFD asserts in elf_append_rela when linking large-model PIC
# shared objects. This build uses the small code model for the loader; normal
# user binaries keep the large model. The cross GCC driver drops -shared when
# calling collect2, so the wrapper hands the link to x86_64-elf-ld directly.
#
set -e

OVERLAY=$(cd "$(dirname "$0")" && pwd)
ROOT=$(cd "$OVERLAY/../.." && pwd)
SRC="$ROOT/3rdparty/build/musl"
BUILD="$ROOT/3rdparty/build/musl-shared"
CC=${CC:-x86_64-elf-gcc}
AR=${AR:-x86_64-elf-ar}
RANLIB=${RANLIB:-x86_64-elf-ranlib}

[ -f "$SRC/src/stdio/printf.c" ] || {
    echo "run 3rdparty/musl/build.sh first" >&2
    exit 1
}

rm -rf "$BUILD"
mkdir -p "$BUILD"
cp -a "$SRC"/. "$BUILD"/
cd "$BUILD"
make distclean >/dev/null 2>&1 || true
cp "$OVERLAY/syscall_arch.h" arch/x86_64/syscall_arch.h
cp "$OVERLAY/__set_thread_area.s" src/thread/x86_64/__set_thread_area.s

cat > cc-elf-link.py <<'LINK'
#!/usr/bin/env python3
import os
import subprocess
import sys

args = sys.argv[1:]
if any(a in ("-c", "-E", "-S") for a in args):
    os.execvp("x86_64-elf-gcc", ["x86_64-elf-gcc"] + args)

out = []
for a in args:
    if a.startswith("-Wl,"):
        parts = a[4:].split(",")
        i = 0
        while i < len(parts):
            p = parts[i]
            if p in ("-e", "-T", "--dynamic-list", "--sort-section"):
                out.append(p)
                i += 1
                if i < len(parts):
                    out.append(parts[i])
            else:
                out.append(p)
            i += 1
    elif (a.startswith("-m") or a.startswith("-f") or a.startswith("-O")
          or a.startswith("-W") or a.startswith("-D") or a.startswith("-I")
          or a.startswith("-std=") or a.startswith("-Wa,")
          or a.startswith("-Wp,")
          or a in ("-pipe", "-nostdlib", "-nostdinc", "-no-pie", "-static",
                   "-g", "-include")):
        pass
    elif a == "-lgcc":
        lib = subprocess.check_output(
            ["x86_64-elf-gcc", "-print-libgcc-file-name"], text=True).strip()
        out.extend(["-L" + os.path.dirname(lib), "-lgcc"])
    else:
        out.append(a)
os.execvp("x86_64-elf-ld", ["x86_64-elf-ld"] + out)
LINK
chmod +x cc-elf-link.py

./configure --target=x86_64-hanos --enable-shared --disable-static \
    CC="$CC" CFLAGS='-mcmodel=small -mno-red-zone -Wno-return-local-addr' \
    >/dev/null
make AR="$AR" RANLIB="$RANLIB" CC="$BUILD/cc-elf-link.py" -j"$(nproc)" >/dev/null

test -f "$BUILD/lib/libc.so"
