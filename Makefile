ISO_IMAGE = cdrom.iso
HDD_IMAGE = hdd.img
TARGET_ROOT = $(shell pwd)/tgroot
QEMU_HDA_FLAGS = -drive id=handisk,file=$(HDD_IMAGE),if=ide,bus=0,unit=0

.PHONY: clean all run

all: $(ISO_IMAGE)

# Option for hyper-threading: -smp 4,sockets=1,cores=2
# Option for CDROM: -cdrom $(ISO_IMAGE)
# Option for debug information: -d in_asm,out_asm,int,op
# Option for UEFI: -bios ./bios64.bin
# Option for debug: -d int
run: $(ISO_IMAGE)
	qemu-system-x86_64 -M q35 -m 4G -smp 2 -no-reboot -k en-us -cdrom $(ISO_IMAGE) -rtc base=localtime -monitor stdio

test: $(HDD_IMAGE)
	qemu-system-x86_64 $(QEMU_HDA_FLAGS) -no-reboot -m 4G -smp 2 -rtc base=localtime

limine:
	git clone https://github.com/limine-bootloader/limine.git --branch=v3.0-branch-binary --depth=1
	make -C limine

kernel/hanos.elf:
	$(MAKE) -C userspace
	$(MAKE) -C kernel

initrd:
	tar -cvf initrd.tar -C $(TARGET_ROOT) bin

$(ISO_IMAGE): limine kernel/hanos.elf initrd
	rm -rf iso_root
	mkdir -p iso_root
	cp kernel/hanos.elf initrd.tar \
		limine.cfg limine/limine.sys limine/limine-cd.bin limine/limine-cd-efi.bin iso_root/
	xorriso -as mkisofs -b limine-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine-cd-efi.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(ISO_IMAGE)
	limine/limine-deploy $(ISO_IMAGE)
	rm -rf iso_root

clean:
	rm -f $(ISO_IMAGE) kernel/boot/stivale2.h kernel/wget-log initrd.tar
	$(MAKE) -C userspace clean
	$(MAKE) -C kernel clean

