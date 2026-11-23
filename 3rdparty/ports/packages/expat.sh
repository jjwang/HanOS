#!/bin/bash
# Build expat (static) against the HanOS musl sysroot.
#
# wayland-scanner parses protocol XML with expat. The target scanner links
# expat statically, so the guest needs no libexpat at runtime.
set -e
. "$(dirname "$0")/../common.sh"

NAME=libexpat-R_2_5_0
URL=https://deb.debian.org/debian/pool/main/e/expat/expat_2.5.0.orig.tar.gz

fetch_extract "$NAME" "$URL" expat_2.5.0.orig.tar.gz
cd "$SRC/$NAME/expat"
# The Debian source is a git export without the generated configure script.
./buildconf.sh
port_configure --without-xmlwf --without-examples --without-tests \
    --without-docbook
make -j"$NPROC"

DEST="$BUILD/install-expat"
rm -rf "$DEST"
make install DESTDIR="$DEST"
"$STRIP" --strip-unneeded "$DEST/usr/lib/libexpat.a"

mkdir -p "$STAGE/lib/pkgconfig" "$STAGE/include"
cp -f "$DEST/usr/lib/libexpat.a" "$STAGE/lib/"
cp -f "$DEST/usr/include/expat.h" "$DEST/usr/include/expat_external.h" \
    "$STAGE/include/"

cat > "$STAGE/lib/pkgconfig/expat.pc" <<EOF
prefix=$STAGE
exec_prefix=\${prefix}
libdir=\${exec_prefix}/lib
includedir=\${prefix}/include

Name: expat
Description: XML parser library
Version: 2.5.0
Libs: -L\${libdir} -lexpat
Cflags: -I\${includedir}
EOF

rm -rf "$DEST"
echo "expat: $STAGE/lib/libexpat.a"
