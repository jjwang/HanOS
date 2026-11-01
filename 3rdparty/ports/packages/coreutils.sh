#!/bin/bash
# Build coreutils against the HanOS musl sysroot.
set -e

. "$(dirname "$0")/../common.sh"

NAME=coreutils-9.5
URL=https://ftp.gnu.org/gnu/coreutils/$NAME.tar.xz

fetch_extract "$NAME" "$URL" "$NAME.tar.xz"
cd "$SRC/$NAME"

port_configure     \
    --without-gmp  \
    --without-selinux

make -j"$NPROC"

install_tree coreutils
