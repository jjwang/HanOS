ISO_IMAGE = cdrom.iso
HDD_IMAGE = release/hdd.img
TARGET_ROOT = $(shell pwd)/initrd

.PHONY: clean all initrd kernel run run-hdd run-uefi run-hdd-uefi

all: $(ISO_IMAGE)

# Option for hyper-threading: -smp 4,sockets=1,cores=2
# Option for debug information: -d in_asm,out_asm,int,op
run: $(ISO_IMAGE)
	qemu-system-x86_64 -enable-kvm -cpu host -serial stdio -M q35,smm=off -m 2G -smp 2 -no-reboot -rtc base=localtime -cdrom $(ISO_IMAGE)

run-uefi: ovmf $(ISO_IMAGE)
	qemu-system-x86_64 -enable-kvm -cpu host -serial stdio -M q35 -m 4G -smp 2 -no-reboot -rtc base=localtime -bios ovmf/OVMF.fd -cdrom $(ISO_IMAGE)

run-hdd: $(HDD_IMAGE)
	qemu-system-x86_64 -enable-kvm -cpu host -serial stdio -M q35 -m 2G -smp 4 -no-reboot -rtc base=localtime -drive id=handisk,if=none,format=raw,file=$(HDD_IMAGE) -device ide-hd,drive=handisk,bus=ide.0

run-hdd-uefi: ovmf $(HDD_IMAGE)
	qemu-system-x86_64 -enable-kvm -cpu host -serial stdio -M q35 -m 2G -smp 4 -no-reboot -rtc base=localtime -bios ovmf/OVMF.fd -drive id=handisk,if=none,format=raw,file=$(HDD_IMAGE) -device ide-hd,drive=handisk,bus=ide.0

limine:
	git clone https://github.com/limine-bootloader/limine.git --branch=v8.x-binary --depth=1
	make -C limine

ovmf:
	mkdir -p ovmf
	cd ovmf && curl -Lo OVMF.fd https://retrage.github.io/edk2-nightly/bin/RELEASEX64_OVMF.fd

kernel:
	$(MAKE) -C kernel

indent:
	$(MAKE) indent -C kernel
	$(MAKE) indent -C userspace

initrd:
	mkdir -p initrd
	cp -rf sysroot/* initrd
	$(MAKE) -C userspace

$(ISO_IMAGE): limine initrd kernel
	rm -rf iso_root initrd.tar
	mkdir -p initrd/etc initrd/usr initrd/root
	cp -rf sysroot/* initrd
	tar -cvpf initrd.tar -C $(TARGET_ROOT) bin assets etc usr root
	mkdir -p iso_root
	cp kernel/hanos.elf initrd.tar \
		limine.conf limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/
	xorriso -as mkisofs -b limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(ISO_IMAGE)
	limine/limine bios-install $(ISO_IMAGE)
	rm -rf iso_root

$(HDD_IMAGE): limine initrd kernel
	rm -rf initrd.tar
	mkdir -p initrd/etc initrd/usr initrd/root
	tar -cvpf initrd.tar -C $(TARGET_ROOT) bin assets etc usr root
	rm -f $(HDD_IMAGE) $(HDD_IMAGE).esp
	rm -rf $(HDD_IMAGE).p2root
	mkdir -p $(dir $(HDD_IMAGE))
	dd if=/dev/zero bs=1M count=0 seek=256 of=$(HDD_IMAGE)
	sgdisk $(HDD_IMAGE) -n 1:2048:+96M -t 1:ef00 -n 2:198656:0 -t 2:8300
	./limine/limine bios-install $(HDD_IMAGE)
	dd if=/dev/zero of=$(HDD_IMAGE).esp bs=1M count=96
	mformat -i $(HDD_IMAGE).esp ::
	mmd -i $(HDD_IMAGE).esp ::/EFI ::/EFI/BOOT
	mcopy -i $(HDD_IMAGE).esp kernel/hanos.elf initrd.tar limine.conf limine/limine-bios.sys ::/
	mcopy -i $(HDD_IMAGE).esp limine/BOOTX64.EFI limine/BOOTIA32.EFI ::/EFI/BOOT
	dd if=$(HDD_IMAGE).esp of=$(HDD_IMAGE) bs=512 seek=2048 conv=notrunc
	rm -f $(HDD_IMAGE).esp
	mkdir -p $(HDD_IMAGE).p2root/bin $(HDD_IMAGE).p2root/assets
	cp -f userspace/games/tetris $(HDD_IMAGE).p2root/bin/ 2>/dev/null || true
	printf 'HanOS ext2 data partition.\n' > $(HDD_IMAGE).p2root/assets/readme.txt
	mke2fs -t ext2 -q -F -O ^dir_index,^resize_inode \
		-E offset=$$((198656 * 512)) -d $(HDD_IMAGE).p2root $(HDD_IMAGE) 156M
	rm -rf $(HDD_IMAGE).p2root

clean:
	rm -rf $(ISO_IMAGE) kernel/boot/stivale2.h kernel/wget-log initrd.tar \
        release/hdd.img initrd/etc initrd/usr
	rm -rf initrd
	$(MAKE) -C userspace clean
	$(MAKE) -C kernel clean

