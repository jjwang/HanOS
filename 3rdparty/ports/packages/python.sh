#!/bin/bash
# Build CPython 3.8 against the HanOS musl sysroot.
#
# The interpreter links dynamically against libpython3.8.so and the shared
# musl libc, and the extensions build as shared objects. libpython goes to
# /lib so the loader finds it on its default path; the stdlib and the
# extensions live on the ext2 data partition under lib/python3.8.
set -e

. "$(dirname "$0")/../common.sh"

NAME=Python-3.8.18
URL=https://www.python.org/ftp/python/$NAME/Python-3.8.18.tgz

fetch_extract "$NAME" "$URL" "$NAME.tgz"
cd "$SRC/$NAME"

echo '# Edit this file for local setup changes' > Modules/Setup.local

cat > "$BUILD/python-config.site" <<'EOF'
ac_cv_file__dev_ptmx=no
ac_cv_file__dev_ptc=no
EOF

READELF=x86_64-elf-readelf CONFIG_SITE="$BUILD/python-config.site" \
    CC="$PORTS/cc-dynamic" LDSHARED="$PORTS/cc-dynamic -shared" \
    ./configure --build=x86_64-pc-linux-gnu --host=x86_64-pc-linux-musl \
    --prefix=/usr --enable-shared --with-ensurepip=no --disable-ipv6

# Report HanOS as the platform. Keep the Linux host triple for the configure
# feature checks, and override MACHDEP only for the compiled-in platform name
# and the sysconfig data module.
rm -f Python/getplatform.o pybuilddir.txt
rm -f build/lib.*/_sysconfigdata_*.py
make -j"$NPROC" MACHDEP=hanos

# Stage the interpreter, the shared library and the stdlib. Use real files,
# not symlinks: the ext2 path does not follow a symlink on exec.
PYLIB="$STAGE/lib/python3.8"

rm -rf "$PYLIB" "$STAGE/lib/libpython3.8.so.1.0"
mkdir -p "$PYLIB/lib-dynload" "$STAGE/bin" "$STAGE/lib"
cp -a Lib/. "$PYLIB/"
rm -rf "$PYLIB/test" "$PYLIB/idlelib" "$PYLIB/tkinter" "$PYLIB/turtledemo" \
    "$PYLIB/ensurepip" "$PYLIB/lib2to3" "$PYLIB/distutils/tests" \
    "$PYLIB/unittest/test"
cp -f build/lib.*/_sysconfigdata_*.py "$PYLIB/"
cp -f build/lib.linux-x86_64-3.8/*.so "$PYLIB/lib-dynload/"

# Precompile the stdlib so imports load bytecode instead of parsing .py.
PYTHONHOME="$STAGE" ./python -m compileall -b -q "$PYLIB" >/dev/null 2>&1 || true
cp -f libpython3.8.so.1.0 "$STAGE/lib/"
cp -f python "$STAGE/bin/python3.8"
cp -f python "$STAGE/bin/python3"
cp -f python "$STAGE/bin/python"
"$STRIP" --strip-unneeded "$STAGE/lib/libpython3.8.so.1.0" \
    "$STAGE/bin/python3.8" "$STAGE/bin/python3" "$STAGE/bin/python"
