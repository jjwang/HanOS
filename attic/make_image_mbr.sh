#!/bin/sh

echo "==> Making an image on $OSTYPE..."
# Create an empty zeroed out 48MiB image file
echo ""
echo "==> Creating an empty 48 MiB image..."
dd if=/dev/zero bs=1M count=0 seek=48 of=./release/hdd.img

# Create a MBR partition table
echo ""
echo "==> Creating a MBR partition table..."
parted -s ./release/hdd.img mklabel msdos

# Create an MBR partition that spans the whole disk
echo ""
echo "==> Creating an primary partition..."
parted -s ./release/hdd.img mkpart primary fat32 2048s 100%

# Install the Limine BIOS stages onto the image
echo ""
echo "==> Installing Limine..."
./limine/limine bios-install ./release/hdd.img

# Mount the loopback device
echo ""
echo "==> Mounting and formatting the image (might request your password)..."
USED_LOOPBACK=$(sudo losetup -Pf --show ./release/hdd.img)

# Format the MBR partition as FAT32
sudo mkfs.fat -F 32 "${USED_LOOPBACK}p1"

# Mount the partition itself
mkdir -p img_mount
sudo mount "${USED_LOOPBACK}p1" img_mount

# Copy the relevant files over
echo ""
echo "==> Copying necessary files..."
sudo rm -rf iso_root initrd.tar
#if [ -e "xbstrap-build/system-root" ]; then cp -rf xbstrap-build/system-root/* initrd 2>/dev/null; fi
mkdir -p initrd/etc initrd/usr initrd/root
#cp -rf sysroot/* initrd
tar -cvpf initrd.tar -C ./initrd bin assets etc usr root

sudo mkdir -p img_mount/EFI/BOOT/

LIMINE_TMP_CFG=$(mktemp)

sudo rm -rf "$LIMINE_TMP_CFG"
cat <<EOF | sudo tee "$LIMINE_TMP_CFG" > /dev/null
# Timeout in seconds that Limine will use before automatically booting.
TIMEOUT=5
EDITOR_ENABLED=no
GRAPHICS=yes
INTERFACE_RESOLUTION=1366x768

:HanOS
PROTOCOL=limine
KERNEL_PATH=boot:///hanos.elf
KERNEL_CMDLINE=Hi HanOS!
MODULE_PATH=boot:///initrd.tar
MODULE_CMDLINE=INITRD
RESOLUTION=1366x768
KASLR=no
EOF

sudo cp -v kernel/hanos.elf initrd.tar img_mount/
sudo cp -v "$LIMINE_TMP_CFG" img_mount/limine.cfg
sudo rm -rf "$LIMINE_TMP_CFG"

{
    sudo cp -v limine/limine-bios.sys img_mount/
    sudo cp -v limine/BOOTX64.EFI img_mount/EFI/BOOT/
} 2> /dev/null

# Sync system cache and unmount partition and loopback device
echo ""
echo "==> Finishing up..."
sync
sudo umount img_mount
sudo rm -rf img_mount
sudo losetup -d "${USED_LOOPBACK}"

echo "-----------"
echo "==> Done!"
echo "==> Now, you can launch using the following command."
echo "> qemu-system-x86_64 -enable-kvm -cpu host -serial stdio -m 2G -hda ./release/hdd.img"
echo "==> Good luck!"
