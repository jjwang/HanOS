ISO_IMAGE = cdrom.iso
HDD_IMAGE = release/hdd.img
TARGET_ROOT = $(shell pwd)/initrd

.PHONY: clean all initrd kernel run test

all: $(ISO_IMAGE)

# Option for hyper-threading: -smp 4,sockets=1,cores=2
# Option for CDROM: -cdrom $(ISO_IMAGE)
# Option for debug information: -d in_asm,out_asm,int,op
# Option for UEFI: -bios ./bios64.bin
# Option for debug: -d int
run: $(ISO_IMAGE)
	qemu-system-x86_64 -serial stdio -M q35,smm=off -m 2G -smp 2 -no-reboot -rtc base=localtime -cdrom $(ISO_IMAGE)

# The below "test" is used for debuging bootable disk image
.PHONY: test
test:
	qemu-system-x86_64 -serial stdio -M q35 -m 1G -smp 2 -no-reboot -rtc base=localtime -drive id=handisk,if=ide,format=raw,bus=0,unit=0,file=$(HDD_IMAGE)

# The below "debug" is used for gdb debug
.PHONY: debug
debug: $(ISO_IMAGE)
	qemu-system-x86_64 -s -S -serial stdio -M q35 -m 1G -smp 2 -no-reboot -rtc base=localtime -cdrom $(ISO_IMAGE)

# The below "distro" is for future distributable disk image build
.PHONY: distro
distro:
	mkdir -p xbstrap-build
	cd xbstrap-build && xbstrap init .. && xbstrap install -u --all
	cp -rf xbstrap-build/system-root/* initrd

limine:
	git clone https://github.com/limine-bootloader/limine.git --branch=v4.x-branch-binary --depth=1
	make -C limine

kernel:
	$(MAKE) -C kernel

initrd:
	$(MAKE) -C userspace
	$(MAKE) -C server

$(ISO_IMAGE): limine initrd kernel
	rm -rf iso_root initrd.tar
	@if [ -e "xbstrap-build/system-root" ]; then cp -rf xbstrap-build/system-root/* initrd 2>/dev/null; fi
	mkdir -p initrd/etc initrd/server initrd/usr initrd/root
	tar -cvf initrd.tar -C $(TARGET_ROOT) bin server assets etc usr root
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
	rm -rf $(ISO_IMAGE) kernel/boot/stivale2.h kernel/wget-log initrd.tar \
        release/hdd.img initrd/etc initrd/server initrd/usr
	$(MAKE) -C userspace clean
	$(MAKE) -C kernel clean
	$(MAKE) -C server clean

