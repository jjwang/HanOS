#!/bin/bash
# Build CPython 3.8 against the HanOS musl sysroot.
#
# The x86_64-elf toolchain cannot emit shared objects, so the interpreter is
# static and the extensions are listed in Modules/Setup.local as builtins.
# The stdlib is staged under lib/python3.8 and mounted from the ext2 partition.
set -e

. "$(dirname "$0")/../common.sh"

NAME=Python-3.8.18
URL=https://www.python.org/ftp/python/$NAME/Python-3.8.18.tgz

fetch_extract "$NAME" "$URL" "$NAME.tgz"
cd "$SRC/$NAME"

cat > Modules/Setup.local <<'EOF'
# Static builtin modules for the HanOS port. Modules that need OpenSSL,
# libffi, expat, zlib or a database stay out.
*static*

_struct _struct.c
array arraymodule.c
cmath cmathmodule.c _math.c -lm
math mathmodule.c _math.c -lm
_contextvars _contextvarsmodule.c
_random _randommodule.c
_pickle _pickle.c
_datetime _datetimemodule.c
_bisect _bisectmodule.c
_heapq _heapqmodule.c
_asyncio _asynciomodule.c
_json -I$(srcdir)/Include/internal -DPy_BUILD_CORE_BUILTIN _json.c
unicodedata unicodedata.c
fcntl fcntlmodule.c
select selectmodule.c
mmap mmapmodule.c
_csv _csv.c
_socket socketmodule.c
termios termios.c
resource resource.c
_posixsubprocess _posixsubprocess.c
_md5 md5module.c
_sha1 sha1module.c
_sha256 sha256module.c
_sha512 sha512module.c
_sha3 _sha3/sha3module.c
_blake2 _blake2/blake2module.c _blake2/blake2b_impl.c _blake2/blake2s_impl.c
binascii binascii.c
EOF

cat > "$BUILD/python-config.site" <<'EOF'
ac_cv_file__dev_ptmx=no
ac_cv_file__dev_ptc=no
EOF

READELF=x86_64-elf-readelf CONFIG_SITE="$BUILD/python-config.site" \
    ./configure --build=x86_64-pc-linux-gnu --host=x86_64-pc-linux-musl \
    --prefix=/usr --with-ensurepip=no --disable-ipv6

make -j"$NPROC"

# Stage the binary and stdlib. Use real files, not symlinks: the ext2 path
# does not follow a symlink on exec.
PYLIB="$STAGE/lib/python3.8"

rm -rf "$PYLIB"
mkdir -p "$PYLIB" "$STAGE/bin"
cp -a Lib/. "$PYLIB/"
rm -rf "$PYLIB/test" "$PYLIB/idlelib" "$PYLIB/tkinter" "$PYLIB/turtledemo" \
    "$PYLIB/ensurepip" "$PYLIB/lib2to3" "$PYLIB/distutils/tests" \
    "$PYLIB/unittest/test"
cp -f build/lib.*/_sysconfigdata_*.py "$PYLIB/"
cp -f python "$STAGE/bin/python3.8"
cp -f python "$STAGE/bin/python3"
cp -f python "$STAGE/bin/python"
"$STRIP" --strip-unneeded "$STAGE/bin/python3.8" "$STAGE/bin/python3" \
    "$STAGE/bin/python"
