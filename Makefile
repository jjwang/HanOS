ISO_IMAGE = cdrom.iso

.PHONY: clean all run

all: $(ISO_IMAGE)

run: $(ISO_IMAGE)
	qemu-system-x86_64 -M q35 -m 2G -smp 2 -debugcon stdio -k en-us -cdrom $(ISO_IMAGE)

limine:
	git clone https://github.com/limine-bootloader/limine.git --branch=v2.0-branch-binary --depth=1
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

