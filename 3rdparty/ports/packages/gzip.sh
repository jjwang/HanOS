#!/bin/bash
# Build gzip against the HanOS musl sysroot.
set -e
. "$(dirname "$0")/../common.sh"
NAME=gzip-1.13
URL=https://ftp.gnu.org/gnu/gzip/$NAME.tar.xz
fetch_extract "$NAME" "$URL" "$NAME.tar.xz"
cd "$SRC/$NAME"
port_configure
make -j"$NPROC"
install_bin gzip gzip
