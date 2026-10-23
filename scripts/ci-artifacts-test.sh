#!/bin/sh
# Check the built ISO and initrd without booting a machine.
set -eu

test -s cdrom.iso
test -s initrd.tar

xorriso -indev cdrom.iso -ls / >/tmp/hanos-iso-list 2>/dev/null
grep -q 'hanos.elf' /tmp/hanos-iso-list
grep -q 'initrd.tar' /tmp/hanos-iso-list
grep -q 'limine.conf' /tmp/hanos-iso-list

tar -tf initrd.tar >/tmp/hanos-initrd-list
for name in hansh init vfs fat32 ext2 block net console; do
    grep -qx "bin/$name" /tmp/hanos-initrd-list
done

version=$(scripts/version.sh)
echo "$version" | grep -Eq '^[0-9]+\.[0-9]+\.[0-9]+\.[0-9]{6}$'

echo "artifact test PASS"
