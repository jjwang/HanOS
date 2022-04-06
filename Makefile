ISO_IMAGE = cdrom.iso
HDD_IMAGE = hdd.img
QEMU_HDA_FLAGS = -drive id=handisk,file=$(HDD_IMAGE),if=ide,bus=0,unit=0

.PHONY: clean all run

all: $(ISO_IMAGE)

# Option for hyper-threading: -smp 4,sockets=1,cores=2
# Option for CDROM: -cdrom $(ISO_IMAGE)
run: $(ISO_IMAGE)
	qemu-system-x86_64 -M q35 -m 2G -smp 2 -no-reboot -k en-us -cdrom $(ISO_IMAGE) -rtc base=localtime

test: $(HDD_IMAGE)
	qemu-system-x86_64 $(QEMU_HDA_FLAGS) -m 2G -smp 2 -rtc base=localtime

limine:
	git clone https://github.com/limine-bootloader/limine.git --branch=latest-binary --depth=1
	make -C limine

kernel/hanos.elf:
	$(MAKE) -C kernel

$(ISO_IMAGE): limine kernel/hanos.elf
	rm -rf iso_root
	mkdir -p iso_root
	cp kernel/hanos.elf \
		limine.cfg limine/limine.sys limine/limine-cd.bin limine/limine-eltorito-efi.bin iso_root/
	xorriso -as mkisofs -b limine-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine-eltorito-efi.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(ISO_IMAGE)
	limine/limine-install $(ISO_IMAGE)
	rm -rf iso_root

clean:
	rm -f $(ISO_IMAGE) kernel/boot/stivale2.h kernel/wget-log
	$(MAKE) -C kernel clean

