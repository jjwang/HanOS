#!/bin/bash
# Build libffi (shared) against the HanOS musl sysroot.
#
# wayland links libffi for its closure API, so this runs before the wayland
# port. The exec static trampoline is disabled: it needs <linux/*.h> kernel
# headers that the musl sysroot does not ship, and the runtime table is not
# used by a normal ffi_call.
set -e
. "$(dirname "$0")/../common.sh"

NAME=libffi-3.4.4
URL=https://deb.debian.org/debian/pool/main/libf/libffi/libffi_3.4.4.orig.tar.gz

fetch_extract "$NAME" "$URL" libffi_3.4.4.orig.tar.gz
cd "$SRC/$NAME"

CC="$PORTS/cc-dynamic" CC_FOR_BUILD=gcc \
    ./configure --build=x86_64-pc-linux-gnu --host=x86_64-pc-linux-musl \
    --prefix=/usr --enable-shared --disable-static \
    --disable-builddir --disable-exec-static-tramp
make -j"$NPROC"

DEST="$BUILD/install-libffi"
rm -rf "$DEST"
make install DESTDIR="$DEST"

LIB="$DEST/usr/lib/libffi.so.8.1.2"
"$STRIP" --strip-unneeded "$LIB"

mkdir -p "$STAGE/lib/pkgconfig" "$STAGE/include"
# Copy real files under every soname: the ext2 loader does not follow a
# symlink on the guest, so libffi.so.8 must be a regular file.
cp -f "$LIB" "$STAGE/lib/libffi.so.8.1.2"
cp -f "$LIB" "$STAGE/lib/libffi.so.8"
cp -f "$LIB" "$STAGE/lib/libffi.so"
cp -f "$DEST/usr/include/ffi.h" "$DEST/usr/include/ffitarget.h" "$STAGE/include/"

# Point the pkg-config file at the staged tree so the cross build of wayland
# resolves the target headers and library by absolute host path.
cat > "$STAGE/lib/pkgconfig/libffi.pc" <<EOF
prefix=$STAGE
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: libffi
Description: Library supporting Foreign Function Interfaces
Version: 3.4.4
Libs: -L\${libdir} -lffi
Cflags: -I\${includedir}
EOF

rm -rf "$DEST"
echo "libffi: $STAGE/lib/libffi.so.8"
