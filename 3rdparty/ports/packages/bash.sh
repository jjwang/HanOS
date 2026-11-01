#!/bin/bash
# Build bash against the HanOS musl sysroot.
set -e

. "$(dirname "$0")/../common.sh"

NAME=bash-5.2.37
URL=https://ftp.gnu.org/gnu/bash/$NAME.tar.gz

fetch_extract "$NAME" "$URL" "$NAME.tar.gz"
cd "$SRC/$NAME"

port_configure     \
    --without-bash-malloc \
    --without-curses      \
    --disable-readline    \
    --enable-static-link

make -j"$NPROC" LOCAL_LDFLAGS=

install_bin bash
