ISO_IMAGE = cdrom.iso
HDD_IMAGE = release/hdd.img
TARGET_ROOT = $(shell pwd)/initrd

# Applications that live on the ext2 data partition, not in the initrd image.
DISK_APPS = games/tetris games/2048 games/snake

# Optional Linux packages. `make ports` builds them and regenerates both
# images; a plain `make` never includes them.
WITH_PORTS   ?= 0
PORTS_PACKAGES = bash coreutils grep sed tar
PORTS_BUILD  = 3rdparty/build/ports
PORTS_STAGE  = $(PORTS_BUILD)/root
PORTS_STAMP  = $(PORTS_BUILD)/.installed
PORTS_INITRD = initrd-ports.tar
PORTS_HDD_INITRD = initrd-ports-hdd.tar
INITRD_TAR   = initrd.tar
MUSL_LIB     = 3rdparty/build/musl/lib/libc.a

ifeq ($(WITH_PORTS),1)
PORTS_DEP  = $(PORTS_STAMP)
ISO_INITRD = $(PORTS_INITRD)
HDD_INITRD = $(PORTS_HDD_INITRD)
IMAGE_MODE = .image-ports
else
PORTS_DEP  =
ISO_INITRD = $(INITRD_TAR)
HDD_INITRD = $(INITRD_TAR)
IMAGE_MODE = .image-noports
endif

.PHONY: clean all ports clean-ports initrd kernel run run-ports run-hdd run-hdd-ports run-uefi run-hdd-uefi

all: $(ISO_IMAGE) $(HDD_IMAGE)

ports:
	$(MAKE) all WITH_PORTS=1

$(IMAGE_MODE):
	rm -f .image-ports .image-noports
	touch $@

$(MUSL_LIB): 3rdparty/musl/build.sh 3rdparty/musl/syscall_arch.h \
		3rdparty/musl/__set_thread_area.s
	$(MAKE) -C userspace musl

$(PORTS_STAMP): 3rdparty/ports/build.sh 3rdparty/ports/cc \
		3rdparty/ports/common.sh $(wildcard 3rdparty/ports/packages/*.sh) \
		$(MUSL_LIB)
	3rdparty/ports/build.sh $(PORTS_PACKAGES)

$(PORTS_INITRD): $(INITRD_TAR) $(PORTS_STAMP)
	rm -rf initrd-ports
	mkdir -p initrd-ports
	tar -xpf $(INITRD_TAR) -C initrd-ports
	for app in $(PORTS_STAGE)/bin/*; do \
		case $$(basename $$app) in \
			block|console|ext2|fat32|init|input|mem|misc|net|pipe|process|tty|vfs) \
				continue ;; \
		esac; \
		cp -fL $$app initrd-ports/bin/; \
	done
	tar -cpf $@ -C initrd-ports bin assets etc usr root data
	rm -rf initrd-ports

# Option for hyper-threading: -smp 4,sockets=1,cores=2
# Option for debug information: -d in_asm,out_asm,int,op
run: $(ISO_IMAGE)
	qemu-system-x86_64 -enable-kvm -cpu host -serial stdio -M q35,smm=off -m 2G -smp 2 -no-reboot -rtc base=localtime -cdrom $(ISO_IMAGE)

run-ports:
	$(MAKE) $(ISO_IMAGE) WITH_PORTS=1
	qemu-system-x86_64 -enable-kvm -cpu host -serial stdio -M q35,smm=off -m 2G -smp 2 -no-reboot -rtc base=localtime -cdrom $(ISO_IMAGE)

run-uefi: ovmf $(ISO_IMAGE)
	qemu-system-x86_64 -enable-kvm -cpu host -serial stdio -M q35 -m 4G -smp 2 -no-reboot -rtc base=localtime -bios ovmf/OVMF.fd -cdrom $(ISO_IMAGE)

run-hdd: $(HDD_IMAGE)
	qemu-system-x86_64 -enable-kvm -cpu host -serial stdio -M q35 -m 2G -smp 4 -no-reboot -rtc base=localtime -drive id=handisk,if=none,format=raw,file=$(HDD_IMAGE) -device ide-hd,drive=handisk,bus=ide.0

run-hdd-ports:
	$(MAKE) $(HDD_IMAGE) WITH_PORTS=1
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

# Both images carry the same initrd; build it once so a sequential or
# parallel make does not race on the archive.
$(INITRD_TAR): initrd
	rm -rf initrd/etc initrd/usr initrd/root
	mkdir -p initrd/etc initrd/usr initrd/root
	cp -rf sysroot/* initrd
	tar -cvpf $(INITRD_TAR) -C $(TARGET_ROOT) bin assets etc usr root data

$(PORTS_HDD_INITRD): $(INITRD_TAR) $(PORTS_STAMP)
	rm -rf initrd-ports-hdd
	mkdir -p initrd-ports-hdd
	tar -xpf $(INITRD_TAR) -C initrd-ports-hdd
	mkdir -p initrd-ports-hdd/etc
	touch initrd-ports-hdd/etc/ports-image
	tar -cpf $@ -C initrd-ports-hdd bin assets etc usr root data
	rm -rf initrd-ports-hdd

$(ISO_IMAGE): limine $(ISO_INITRD) kernel $(IMAGE_MODE) $(PORTS_DEP)
	rm -rf iso_root
	mkdir -p iso_root
	cp kernel/hanos.elf \
		limine.conf limine/limine-bios.sys limine/limine-bios-cd.bin limine/limine-uefi-cd.bin iso_root/
	cp $(ISO_INITRD) iso_root/initrd.tar
	xorriso -as mkisofs -b limine-bios-cd.bin \
		-no-emul-boot -boot-load-size 4 -boot-info-table \
		--efi-boot limine-uefi-cd.bin \
		-efi-boot-part --efi-boot-image --protective-msdos-label \
		iso_root -o $(ISO_IMAGE)
	limine/limine bios-install $(ISO_IMAGE)
	rm -rf iso_root

$(HDD_IMAGE): limine $(HDD_INITRD) kernel $(IMAGE_MODE) $(PORTS_DEP)
	rm -f $(HDD_IMAGE) $(HDD_IMAGE).esp
	rm -rf $(HDD_IMAGE).p2root
	mkdir -p $(dir $(HDD_IMAGE))
	dd if=/dev/zero bs=1M count=0 seek=256 of=$(HDD_IMAGE)
	sgdisk $(HDD_IMAGE) -n 1:2048:+96M -t 1:ef00 -n 2:198656:0 -t 2:8300
	./limine/limine bios-install $(HDD_IMAGE)
	dd if=/dev/zero of=$(HDD_IMAGE).esp bs=1M count=96
	mformat -F -i $(HDD_IMAGE).esp ::
	mmd -i $(HDD_IMAGE).esp ::/EFI ::/EFI/BOOT
	mcopy -i $(HDD_IMAGE).esp kernel/hanos.elf limine.conf limine/limine-bios.sys ::/
	mcopy -i $(HDD_IMAGE).esp $(HDD_INITRD) ::/initrd.tar
	mcopy -i $(HDD_IMAGE).esp limine/BOOTX64.EFI limine/BOOTIA32.EFI ::/EFI/BOOT
	dd if=$(HDD_IMAGE).esp of=$(HDD_IMAGE) bs=512 seek=2048 conv=notrunc
	rm -f $(HDD_IMAGE).esp
	mkdir -p $(HDD_IMAGE).p2root/bin $(HDD_IMAGE).p2root/assets
	for app in $(DISK_APPS); do cp -f userspace/$$app $(HDD_IMAGE).p2root/bin/; done
ifeq ($(WITH_PORTS),1)
	cp -fL $(PORTS_STAGE)/bin/* $(HDD_IMAGE).p2root/bin/
endif
	printf 'HanOS ext2 data partition.\n' > $(HDD_IMAGE).p2root/assets/readme.txt
	mke2fs -t ext2 -q -F -O ^dir_index,^resize_inode \
		-E offset=$$((198656 * 512)) -d $(HDD_IMAGE).p2root $(HDD_IMAGE) 156M
	rm -rf $(HDD_IMAGE).p2root

clean:
	rm -rf $(ISO_IMAGE) kernel/boot/stivale2.h kernel/wget-log initrd.tar \
        initrd-ports.tar initrd-ports initrd-ports-hdd.tar initrd-ports-hdd \
        .image-ports .image-noports \
        release/hdd.img initrd/etc initrd/usr
	rm -rf initrd
	$(MAKE) -C userspace clean
	$(MAKE) -C kernel clean

clean-ports:
	rm -rf $(PORTS_BUILD)

