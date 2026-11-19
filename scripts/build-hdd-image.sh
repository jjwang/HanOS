#!/bin/bash
# Build the 512 MiB HanOS disk image (release/hdd.img).
# GPT hybrid: p1 Limine BIOS stage 2, p2 ext2 data (toolchain + ports + games),
# p3 64 MiB FAT16 EFI System Partition. Boots on BIOS and UEFI.
set -e

ROOT=$(cd "$(dirname "$0")/.." && pwd)
IMG="$ROOT/release/hdd.img"
FAT="$ROOT/release/hdd.fat16"
INITRD="${1:-$ROOT/initrd.tar}"
BIOS_START=2048
EXT2_START=4096
EXT2_MB=445
ESP_START=915456
ESP_MB=64

rm -f "$IMG" "$FAT"
truncate -s 512M "$IMG"
sgdisk -Z "$IMG" >/dev/null 2>&1 || true
sgdisk -n 1:${BIOS_START}:+1M -t 1:8300 \
    -n 2:${EXT2_START}:+${EXT2_MB}M -t 2:8300 \
    -n 3:${ESP_START}:+${ESP_MB}M -t 3:ef00 "$IMG" >/dev/null
"$ROOT/limine/limine" bios-install "$IMG" 1 >/dev/null

dd if=/dev/zero of="$FAT" bs=1M count=${ESP_MB} status=none
mformat -i "$FAT" ::
mmd -i "$FAT" ::/EFI ::/EFI/BOOT
mcopy -i "$FAT" "$ROOT/kernel/hanos.elf" \
    "$ROOT/limine.conf" "$ROOT/limine/limine-bios.sys" ::/
mcopy -i "$FAT" "$INITRD" ::/initrd.tar
mcopy -i "$FAT" "$ROOT/limine/BOOTX64.EFI" \
    "$ROOT/limine/BOOTIA32.EFI" ::/EFI/BOOT
dd if="$FAT" of="$IMG" bs=512 seek="$ESP_START" conv=notrunc status=none
rm -f "$FAT"

ROOTFS="$ROOT/release/hdd.p2root"
SYS="$ROOT/3rdparty/build/sysroot"
CROSSLIB="$ROOT/3rdparty/build/toolchain/x86_64-hanos/lib/gcc/x86_64-hanos/10.2.0"
rm -rf "$ROOTFS"
mkdir -p "$ROOTFS/bin" "$ROOTFS/assets"
cp -a "$ROOT/3rdparty/build/toolchain/native-gcc/usr/." "$ROOTFS/"
find "$SYS/usr/include" -mindepth 1 -maxdepth 1 ! -name c++ -exec cp -a {} "$ROOTFS/include/" \;
cp -f "$ROOT/3rdparty/build/toolchain/native/bin/x86_64-hanos-as" "$ROOTFS/bin/as"
cp -f "$ROOT/3rdparty/build/toolchain/native/bin/x86_64-hanos-ld" "$ROOTFS/bin/ld"
mkdir -p "$ROOTFS/libexec/gcc/x86_64-hanos/10.2.0"
cp -f "$ROOT/3rdparty/build/toolchain/native/bin/x86_64-hanos-as" "$ROOTFS/libexec/gcc/x86_64-hanos/10.2.0/as"
cp -f "$ROOT/3rdparty/build/toolchain/native/bin/x86_64-hanos-ld" "$ROOTFS/libexec/gcc/x86_64-hanos/10.2.0/ld"
cp -f "$SYS/usr/lib/libc.a" "$SYS/usr/lib/libm.a" "$SYS/usr/lib/libc.so" "$ROOTFS/lib/"
cp -f "$SYS/usr/lib/crt1.o" "$SYS/usr/lib/crti.o" "$SYS/usr/lib/crtn.o" "$ROOTFS/lib/"
cp -f "$SYS/usr/lib/crt1.o" "$ROOTFS/lib/crt0.o"
cp -f "$CROSSLIB/crtbegin.o" "$CROSSLIB/crtend.o" "$ROOTFS/lib/"
mkdir -p "$ROOTFS/lib/gcc/x86_64-hanos/10.2.0"
cp -f "$CROSSLIB/crtbegin.o" "$CROSSLIB/crtend.o" "$ROOTFS/lib/gcc/x86_64-hanos/10.2.0/"
for app in $(sed -n 's/^DISK_APPS = //p' "$ROOT/GNUmakefile"); do
    cp -f "$ROOT/userspace/$app" "$ROOTFS/bin/"
done
if [ -d "$ROOT/3rdparty/build/ports/root/bin" ]; then
    cp -a "$ROOT/3rdparty/build/ports/root/bin/." "$ROOTFS/bin/"
fi
if [ -d "$ROOT/3rdparty/build/ports/root/lib" ]; then
    cp -a "$ROOT/3rdparty/build/ports/root/lib/." "$ROOTFS/lib/"
fi
printf 'HanOS ext2 data partition.\n' > "$ROOTFS/assets/readme.txt"
mke2fs -t ext2 -q -F -O ^dir_index,^resize_inode \
    -E offset=$((EXT2_START * 512)) -d "$ROOTFS" "$IMG" ${EXT2_MB}M
rm -rf "$ROOTFS"
echo "disk image: $IMG"
