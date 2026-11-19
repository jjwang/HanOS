#!/bin/bash
# Build the target GMP, MPFR and MPC libraries required by native GCC.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
TOOL="$ROOT/3rdparty/build/toolchain"
CROSS="$TOOL/x86_64-hanos/bin"
SYS="$ROOT/3rdparty/build/sysroot"
DL="$TOOL/dl"
SRC="$TOOL/src"
BUILD="$TOOL/build-math"
CC="$CROSS/x86_64-hanos-gcc"

mkdir -p "$DL" "$SRC"
common='--build=x86_64-pc-linux-gnu --host=x86_64-hanos --prefix=/usr --disable-shared --enable-static'

build() {
    name=$1 url=$2 file=$3
    shift 3
    [ -f "$DL/$file" ] || curl -fL --retry 5 --retry-delay 2 \
        -o "$DL/$file" "$url"
    [ -d "$SRC/$name" ] || tar -xf "$DL/$file" -C "$SRC"
    for f in "$SRC/$name/config.sub" "$SRC/$name/configfsf.sub" "$SRC/$name/build-aux/config.sub"; do
        [ -f "$f" ] || continue
        python3 - "$f" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1]); s = p.read_text()
s = s.replace("\tnone)\n\t\t;;", "\tnone | hanos*)\n\t\t;;", 1)
p.write_text(s)
PY
    done
    rm -rf "$BUILD/$name"; mkdir -p "$BUILD/$name"
    cd "$BUILD/$name"
    CC="$CC" CFLAGS='-O2 -mcmodel=large' \
    CPPFLAGS="-I$SYS/usr/include" LDFLAGS="-L$SYS/usr/lib" \
        "$SRC/$name/configure" $common "$@"
    make -j"$(nproc)"
    make install DESTDIR="$SYS"
    for la in "$SYS/usr/lib/libgmp.la" "$SYS/usr/lib/libmpfr.la"; do
        [ -f "$la" ] || continue
        python3 - "$la" "$SYS" <<'PY'
from pathlib import Path
import sys
p = Path(sys.argv[1]); root = sys.argv[2]
s = p.read_text()
s = s.replace(root + "/" + root, root)
s = s.replace("libdir='/usr/lib'", "libdir='" + root + "/usr/lib'")
s = s.replace(" /usr/lib/libgmp.la", " " + root + "/usr/lib/libgmp.la")
p.write_text(s)
PY
    done
    cd - >/dev/null
}

build gmp-6.3.0 https://ftp.gnu.org/gnu/gmp/gmp-6.3.0.tar.xz gmp-6.3.0.tar.xz
build mpfr-4.2.1 https://ftp.gnu.org/gnu/mpfr/mpfr-4.2.1.tar.xz mpfr-4.2.1.tar.xz \
    --with-gmp="$SYS/usr"
build mpc-1.3.1 https://ftp.gnu.org/gnu/mpc/mpc-1.3.1.tar.gz mpc-1.3.1.tar.gz \
    --with-gmp="$SYS/usr" --with-mpfr="$SYS/usr"

echo "math libs: $SYS/usr/lib"
