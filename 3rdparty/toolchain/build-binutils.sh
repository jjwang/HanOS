#!/bin/bash
# Build x86_64-hanos binutils with the host compiler.
set -e

VERSION=2.42
ROOT=$(cd "$(dirname "$0")/../.." && pwd)
BUILD="$ROOT/3rdparty/build/toolchain"
DL="$BUILD/dl"
SRC="$BUILD/src"
PREFIX="$BUILD/x86_64-hanos"
TARBALL="binutils-$VERSION.tar.xz"
URL="https://ftp.gnu.org/gnu/binutils/$TARBALL"

mkdir -p "$DL" "$SRC"
if [ ! -f "$DL/$TARBALL" ]; then
    curl -fL --retry 5 --retry-delay 2 --retry-connrefused \
        -o "$DL/$TARBALL.part" "$URL"
    mv "$DL/$TARBALL.part" "$DL/$TARBALL"
fi
if [ ! -d "$SRC/binutils-$VERSION" ]; then
    tar -xf "$DL/$TARBALL" -C "$SRC"
fi
# config.sub does not know the hanos OS name.
sed -i 's/| fiwix\* | mlibc\* | cos\* | mbr\* )/| fiwix* | mlibc* | cos* | mbr* | hanos* )/' \
    "$SRC/binutils-$VERSION/config.sub"
sed -i 's/  x86_64-\*-elf\* | x86_64-\*-rtems\* | x86_64-\*-fuchsia | x86_64-\*-genode\*)/  x86_64-*-elf* | x86_64-*-rtems* | x86_64-*-fuchsia | x86_64-*-genode* | x86_64-*-hanos*)/' \
    "$SRC/binutils-$VERSION/bfd/config.bfd"
for f in gas/configure.tgt ld/configure.tgt; do
    sed -i 's/x86_64-\*-elf\* | x86_64-\*-rtems\* | x86_64-\*-fuchsia\* | x86_64-\*-genode\*)/x86_64-*-elf* | x86_64-*-rtems* | x86_64-*-fuchsia* | x86_64-*-genode* | x86_64-*-hanos*)/' \
        "$SRC/binutils-$VERSION/$f"
done
sed -i 's/  i386-\*-elf\*)\t\t\t\tfmt=elf ;;/  i386-*-elf* | i386-*-hanos*)\t\tfmt=elf ;;/' \
    "$SRC/binutils-$VERSION/gas/configure.tgt"

rm -rf "$BUILD/build-binutils"
mkdir -p "$BUILD/build-binutils"
cd "$BUILD/build-binutils"
"$SRC/binutils-$VERSION/configure" \
    --target=x86_64-hanos --prefix="$PREFIX" \
    --disable-nls --disable-werror --disable-shared --enable-static \
    --disable-gdb --disable-sim --disable-gprof --disable-gold \
    --disable-multilib --disable-werror
make -j"$(nproc)" MAKEINFO=true
make install MAKEINFO=true

echo "binutils: $PREFIX/bin"
