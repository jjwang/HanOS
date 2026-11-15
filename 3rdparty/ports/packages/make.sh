#!/bin/bash
# Build GNU make against the HanOS musl sysroot.
set -e
. "$(dirname "$0")/../common.sh"
NAME=make-4.4.1
URL=https://ftp.gnu.org/gnu/make/$NAME.tar.gz
fetch_extract "$NAME" "$URL" "$NAME.tar.gz"
cd "$SRC/$NAME"
port_configure --without-guile
make -j"$NPROC"
install_bin make make
