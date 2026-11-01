#!/bin/bash
# Build tar against the HanOS musl sysroot.
set -e

. "$(dirname "$0")/../common.sh"

NAME=tar-1.35
URL=https://ftp.gnu.org/gnu/tar/$NAME.tar.xz

fetch_extract "$NAME" "$URL" "$NAME.tar.xz"
cd "$SRC/$NAME"

port_configure         \
    --without-selinux  \
    --without-posix-acls \
    --without-xattrs

make -j"$NPROC"

install_bin src/tar tar
