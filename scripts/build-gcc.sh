#!/bin/bash
# Build a hosted x86_64-hanos GCC from source.
set -e

VERSION=10.2.0
ROOT=$(cd "$(dirname "$0")/.." && pwd)
TOOL="$ROOT/3rdparty/build/toolchain"
SRC="$TOOL/src/gcc-$VERSION"
BUILD="$TOOL/build-gcc"
PREFIX="$TOOL/x86_64-hanos"
SYSROOT="$ROOT/3rdparty/build/sysroot"
BIN="$PREFIX/bin"
TARBALL="gcc-$VERSION.tar.gz"

[ -d "$SRC" ] || {
    if [ -f "$HOME/src/$TARBALL" ]; then
        tar -xf "$HOME/src/$TARBALL" -C "$TOOL/src"
    else
        mkdir -p "$TOOL/dl"
        curl -fL --retry 3 -o "$TOOL/dl/$TARBALL" \
            "https://ftp.gnu.org/gnu/gcc/gcc-$VERSION/$TARBALL"
        tar -xf "$TOOL/dl/$TARBALL" -C "$TOOL/src"
    fi
}

# The host config.sub and config.gcc do not know the target OS.
python3 - "$SRC" <<'PY'
from pathlib import Path
import sys
src = Path(sys.argv[1])
p = src / "config.sub"
s = p.read_text()
s = s.replace("\tios)\n\t\t;;\n\tnone)\n\t\t;;",
              "\tios)\n\t\t;;\n\tnone | hanos*)\n\t\t;;", 1)
p.write_text(s)
p = src / "libgcc/config.host"
s = p.read_text()
s = s.replace("x86_64-*-elf* | x86_64-*-rtems*)",
              "x86_64-*-elf* | x86_64-*-rtems* | x86_64-*-hanos*)", 1)
p.write_text(s)
p = src / "gcc/config.gcc"
s = p.read_text()
old = """x86_64-*-elf*)
\ttm_file="${tm_file} i386/unix.h i386/att.h dbxelf.h elfos.h newlib-stdint.h i386/i386elf.h i386/x86-64.h"
\t;;"""
new = """x86_64-*-elf*)
\ttm_file="${tm_file} i386/unix.h i386/att.h dbxelf.h elfos.h newlib-stdint.h i386/i386elf.h i386/x86-64.h"
\t;;
x86_64-*-hanos*)
\ttm_file="${tm_file} i386/unix.h i386/att.h dbxelf.h elfos.h newlib-stdint.h i386/i386elf.h i386/x86-64.h"
\ttmake_file="${tmake_file} i386/t-crtstuff"
\t;;"""
if "x86_64-*-hanos*)" not in s:
    s = s.replace(old, new, 1)
p.write_text(s)
PY

rm -rf "$BUILD"
mkdir -p "$BUILD"
cd "$BUILD"
"$SRC/configure" \
    --target=x86_64-hanos --prefix="$PREFIX" \
    --with-sysroot="$SYSROOT" --with-build-sysroot="$SYSROOT" \
    --with-as="$BIN/x86_64-hanos-as" --with-ld="$BIN/x86_64-hanos-ld" \
    --disable-nls --disable-shared --enable-static --disable-multilib \
    --enable-languages=c --disable-bootstrap --disable-libssp \
    --disable-libquadmath --disable-libgomp --disable-libatomic \
    --disable-libstdcxx --disable-libvtv --disable-libsanitizer
make -j"$(nproc)" all-gcc all-target-libgcc MAKEINFO=true
make install-gcc install-target-libgcc MAKEINFO=true

echo "gcc: $PREFIX/bin/x86_64-hanos-gcc"
