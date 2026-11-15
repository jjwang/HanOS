#!/bin/bash
# Build gawk against the HanOS musl sysroot.
set -e
. "$(dirname "$0")/../common.sh"
NAME=gawk-5.3.0
URL=https://ftp.gnu.org/gnu/gawk/$NAME.tar.xz
fetch_extract "$NAME" "$URL" "$NAME.tar.xz"
cd "$SRC/$NAME"
ac_cv_func_getopt=yes port_configure --disable-extensions --without-readline
make -j"$NPROC" CFLAGS="-O0 -g" LDFLAGS="-Wl,--allow-multiple-definition"
install_bin gawk gawk
