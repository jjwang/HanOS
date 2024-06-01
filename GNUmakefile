ISO_IMAGE = cdrom.iso
HDD_IMAGE = release/hdd.img
TARGET_ROOT = $(shell pwd)/initrd

.PHONY: clean all initrd kernel run run-hdd

all: $(ISO_IMAGE)

# Option for hyper-threading: -smp 4,sockets=1,cores=2
# Option for debug information: -d in_asm,out_asm,int,op
run: $(ISO_IMAGE)
	qemu-system-x86_64 -enable-kvm -cpu host -serial stdio -M q35,smm=off -m 2G -smp 2 -no-reboot -rtc base=localtime -cdrom $(ISO_IMAGE)

run-uefi: ovmf $(ISO_IMAGE)
	qemu-system-x86_64 -enable-kvm -cpu host -serial stdio -M q35 -m 4G -smp 2 -no-reboot -rtc base=localtime -bios ovmf/OVMF.fd -cdrom $(ISO_IMAGE)

run-hdd: $(HDD_IMAGE)
	qemu-system-x86_64 -enable-kvm -cpu host -serial stdio -M q35 -m 2G -smp 4 -no-reboot -rtc base=localtime -drive id=handisk,if=ide,format=raw,bus=0,unit=0,file=$(HDD_IMAGE)

run-hdd-uefi: $(HDD_IMAGE)
	qemu-system-x86_64 -enable-kvm -cpu host -serial stdio -M q35 -m 2G -smp 4 -no-reboot -rtc base=localtime -bios ovmf/OVMF.fd -drive id=handisk,if=ide,format=raw,bus=0,unit=0,file=$(HDD_IMAGE)

limine:
	git clone https://github.com/limine-bootloader/limine.git --branch=v7.x-binary --depth=1
	make -C limine

ovmf:
	mkdir -p ovmf
	cd ovmf && curl -Lo OVMF.fd https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd

kernel:
	$(MAKE) -C kernel

initrd:
	mkdir -p initrd
	cp -rf sysroot/* initrd
	$(MAKE) -C userspace

$(ISO_IMAGE): limine initrd kernel
	rm -rf iso_root initrd.tar
	@if [ -e "xbstrap-build/system-root" ]; then cp -rf xbstrap-build/system-root/* initrd 2>/dev/null; fi
	mkdir -p initrd/etc initrd/usr initrd/root
	cp -rf sysroot/* initrd
	tar -cvpf initrd.tar -C $(TARGET_ROOT) bin assets etc usr root
	mkdir -p iso_root
	cp kernel/hanos.elf initrd.tar \
		limine.cfg limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/
	xorriso -as mkisofs -b limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(ISO_IMAGE)
	limine/limine bios-install $(ISO_IMAGE)
	rm -rf iso_root

$(HDD_IMAGE): limine initrd kernel
	rm -rf initrd.tar
	@if [ -e "xbstrap-build/system-root" ]; then cp -rf xbstrap-build/system-root/* initrd 2>/dev/null; fi
	mkdir -p initrd/etc initrd/usr initrd/root
	cp -rf sysroot/* initrd
	tar -cvpf initrd.tar -C $(TARGET_ROOT) bin assets etc usr root
	rm -f $(HDD_IMAGE)
	dd if=/dev/zero bs=1M count=0 seek=64 of=$(HDD_IMAGE)
	sgdisk $(HDD_IMAGE) -n 1:2048 -t 1:ef00
	./limine/limine bios-install $(HDD_IMAGE)
	mformat -i $(HDD_IMAGE)@@1M
	mmd -i $(HDD_IMAGE)@@1M ::/EFI ::/EFI/BOOT
	mcopy -i $(HDD_IMAGE)@@1M kernel/hanos.elf initrd.tar limine.cfg limine/limine-bios.sys ::/
	mcopy -i $(HDD_IMAGE)@@1M limine/BOOTX64.EFI limine/BOOTIA32.EFI ::/EFI/BOOT

clean:
	rm -rf $(ISO_IMAGE) kernel/boot/stivale2.h kernel/wget-log initrd.tar \
        release/hdd.img initrd/etc initrd/usr
	rm -rf initrd
	$(MAKE) -C userspace clean
	$(MAKE) -C kernel clean

