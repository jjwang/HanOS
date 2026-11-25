ISO_IMAGE = cdrom.iso
HDD_IMAGE = release/hdd.img
TARGET_ROOT = $(shell pwd)/initrd

# Applications that live on the ext2 data partition, not in the initrd image.
DISK_APPS = games/tetris games/2048 games/snake

# Optional Linux packages. `make ports` builds them and regenerates both
# images; a plain `make` never includes them.
WITH_PORTS   ?= 0
PORTS_PACKAGES = bash coreutils grep sed tar make m4 gawk gzip patch diffutils python libffi expat wayland compositor
PORTS_BUILD  = 3rdparty/build/ports
PORTS_STAGE  = $(PORTS_BUILD)/root
PORTS_STAMP  = $(PORTS_BUILD)/.installed
PORTS_INITRD = initrd-ports.tar
PORTS_HDD_INITRD = initrd-ports-hdd.tar
INITRD_TAR   = initrd.tar
MUSL_LIB     = 3rdparty/build/musl/lib/libc.a
MUSL_SHARED  = 3rdparty/build/musl-shared/lib/libc.so
SYSROOT_STAGE = 3rdparty/build/sysroot
SYSROOT_STAMP = $(SYSROOT_STAGE)/.installed
NATIVE_BINUTILS = 3rdparty/build/toolchain/native/bin
CROSS_GCC = 3rdparty/build/toolchain/x86_64-hanos/lib/gcc/x86_64-hanos/10.2.0
NATIVE_GCC = 3rdparty/build/toolchain/native-gcc/usr
WITH_NATIVE_GCC ?= 0

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

.PHONY: clean all ports clean-ports initrd indent kernel run run-ports run-hdd run-hdd-ports hdd-ports run-uefi run-hdd-uefi stress sysroot binutils gcc native-binutils native-gcc

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

$(MUSL_SHARED): 3rdparty/musl/build-shared.sh $(MUSL_LIB)
	3rdparty/musl/build-shared.sh

$(SYSROOT_STAMP): 3rdparty/toolchain/install-sysroot.sh $(MUSL_LIB) $(MUSL_SHARED)
	3rdparty/toolchain/install-sysroot.sh

sysroot: $(SYSROOT_STAMP)

binutils:
	3rdparty/toolchain/build-binutils.sh

gcc:
	3rdparty/toolchain/build-gcc.sh

native-binutils:
	3rdparty/toolchain/build-native-binutils.sh

native-gcc:
	3rdparty/toolchain/build-native-gcc.sh


$(PORTS_INITRD): $(INITRD_TAR) $(PORTS_STAMP)
	rm -rf initrd-ports
	mkdir -p initrd-ports
	tar -xpf $(INITRD_TAR) -C initrd-ports
	touch initrd-ports/etc/ports-image
	for app in $(PORTS_STAGE)/bin/*; do \
		case $$(basename $$app) in \
			block|console|ext2|fat32|init|input|ls|mem|misc|net|pipe|process|tty|vfs) \
				continue ;; \
			python|python3|python3.8) \
				continue ;; \
		esac; \
		cp -fL $$app initrd-ports/bin/; \
	done
	tar -cpf $@ -C initrd-ports bin assets etc usr root data lib sbin
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
	qemu-system-x86_64 -enable-kvm -cpu host -serial stdio -M q35,smm=off -m 2G -smp 4 -no-reboot -rtc base=localtime -drive id=handisk,if=none,format=raw,file=$(HDD_IMAGE) -device ide-hd,drive=handisk,bus=ide.0

run-hdd-ports:
	$(MAKE) $(HDD_IMAGE) WITH_PORTS=1
	qemu-system-x86_64 -enable-kvm -cpu host -serial stdio -M q35,smm=off -m 2G -smp 4 -no-reboot -rtc base=localtime -drive id=handisk,if=none,format=raw,file=$(HDD_IMAGE) -device ide-hd,drive=handisk,bus=ide.0

stress:
	rm -f $(HDD_IMAGE)
	$(MAKE) $(HDD_IMAGE) WITH_PORTS=1
	scripts/stress-test.sh $(HDD_IMAGE)

hdd-ports:
	$(MAKE) $(HDD_IMAGE) WITH_PORTS=1

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

initrd: $(MUSL_SHARED) $(SYSROOT_STAMP)
	mkdir -p initrd initrd/lib
	cp -rf sysroot/* initrd
	mkdir -p initrd/usr/include
	find $(SYSROOT_STAGE)/usr/include -mindepth 1 -maxdepth 1 ! -name c++ -exec cp -a {} initrd/usr/include/ \;
	cp -f $(MUSL_SHARED) initrd/lib/ld-musl-x86_64.so.1
	cp -f $(MUSL_SHARED) initrd/lib/libc.so
	ln -sf ld-musl-x86_64.so.1 initrd/lib/ld64.so.1
	@if [ "$(WITH_NATIVE_GCC)" = "1" ]; then 		[ -f $(NATIVE_GCC)/bin/gcc ] || { echo "run make native-gcc first" >&2; exit 1; }; 		cp -a $(NATIVE_GCC)/. initrd/usr/; 		cp -f $(NATIVE_GCC)/bin/gcc $(NATIVE_GCC)/bin/g++ $(NATIVE_GCC)/bin/cpp initrd/bin/; 		mkdir -p initrd/usr/local/bin; 		cp -f $(NATIVE_GCC)/libexec/gcc/x86_64-hanos/10.2.0/cc1 initrd/usr/local/bin/; 		cp -f $(NATIVE_GCC)/libexec/gcc/x86_64-hanos/10.2.0/cc1plus initrd/usr/local/bin/; 		cp -f $(NATIVE_GCC)/libexec/gcc/x86_64-hanos/10.2.0/collect2 initrd/usr/local/bin/; 		cp -f $(NATIVE_GCC)/libexec/gcc/x86_64-hanos/10.2.0/lto1 initrd/usr/local/bin/; 		cp -f $(NATIVE_BINUTILS)/x86_64-hanos-as initrd/usr/local/bin/as; 		cp -f $(NATIVE_BINUTILS)/x86_64-hanos-ld initrd/usr/local/bin/ld; 		mkdir -p initrd/usr/local/lib/gcc/x86_64-hanos/10.2.0; 		cp -f $(NATIVE_GCC)/lib/gcc/x86_64-hanos/10.2.0/libgcc.a initrd/usr/local/lib/gcc/x86_64-hanos/10.2.0/; 		cp -f $(CROSS_GCC)/crtbegin.o $(CROSS_GCC)/crtend.o initrd/usr/local/lib/gcc/x86_64-hanos/10.2.0/; 		cp -f $(CROSS_GCC)/crtbegin.o $(CROSS_GCC)/crtend.o initrd/usr/local/lib/; 		cp -f $(NATIVE_GCC)/lib/gcc/x86_64-hanos/10.2.0/libgcc.a initrd/usr/local/lib/; 		cp -f $(CROSS_GCC)/crtbegin.o $(CROSS_GCC)/crtend.o initrd/lib/; 		cp -f $(NATIVE_GCC)/lib/gcc/x86_64-hanos/10.2.0/libgcc.a initrd/lib/; 		cp -f $(SYSROOT_STAGE)/usr/lib/libm.a initrd/lib/; 		cp -f $(NATIVE_GCC)/lib/libstdc++.a initrd/lib/; 		cp -f $(NATIVE_GCC)/lib/libstdc++.a initrd/usr/local/lib/; 		cp -f $(NATIVE_GCC)/lib/libstdc++.so initrd/lib/; 		cp -f $(NATIVE_GCC)/lib/libstdc++.so.6 initrd/lib/; 		cp -f $(NATIVE_GCC)/lib/libstdc++.so initrd/usr/local/lib/; 		cp -f $(NATIVE_GCC)/lib/libstdc++.so.6 initrd/usr/local/lib/; 		mkdir -p initrd/usr/local/include; 		cp -a $(NATIVE_GCC)/include/c++ initrd/usr/local/include/; 		mkdir -p initrd/include; 		cp -a $(NATIVE_GCC)/include/c++ initrd/include/; 		rm -f initrd/usr/libexec/gcc/x86_64-hanos/10.2.0/cc1 initrd/usr/libexec/gcc/x86_64-hanos/10.2.0/cc1plus initrd/usr/libexec/gcc/x86_64-hanos/10.2.0/lto1; 	fi
	mkdir -p initrd/usr/local/bin
	mkdir -p initrd/lib
	if [ -f $(NATIVE_GCC)/lib/libstdc++.so.6 ]; then cp -f $(NATIVE_GCC)/lib/libstdc++.so.6 initrd/lib/; fi
	$(MAKE) -C userspace

# Both images carry the same initrd; build it once so a sequential or
# parallel make does not race on the archive.
$(INITRD_TAR): initrd
	rm -rf initrd/etc initrd/usr initrd/root initrd/include initrd/usr/local
	mkdir -p initrd/etc initrd/usr initrd/root initrd/sbin
	cp -rf sysroot/* initrd
	mkdir -p initrd/usr/include
	find $(SYSROOT_STAGE)/usr/include -mindepth 1 -maxdepth 1 ! -name c++ -exec cp -a {} initrd/usr/include/ \;
	mkdir -p initrd/usr/local/bin
	mkdir -p initrd/lib
	if [ -f $(NATIVE_GCC)/lib/libstdc++.so.6 ]; then cp -f $(NATIVE_GCC)/lib/libstdc++.so.6 initrd/lib/; fi
	ln -sf ld-musl-x86_64.so.1 initrd/lib/ld64.so.1
	@if [ "$(WITH_NATIVE_GCC)" = "1" ]; then 		[ -f $(NATIVE_GCC)/bin/gcc ] || { echo "run make native-gcc first" >&2; exit 1; }; 		cp -a $(NATIVE_GCC)/. initrd/usr/; 		cp -f $(NATIVE_GCC)/bin/gcc $(NATIVE_GCC)/bin/g++ $(NATIVE_GCC)/bin/cpp initrd/bin/; 		mkdir -p initrd/usr/local/bin; 		cp -f $(NATIVE_GCC)/libexec/gcc/x86_64-hanos/10.2.0/cc1 initrd/usr/local/bin/; 		cp -f $(NATIVE_GCC)/libexec/gcc/x86_64-hanos/10.2.0/cc1plus initrd/usr/local/bin/; 		cp -f $(NATIVE_GCC)/libexec/gcc/x86_64-hanos/10.2.0/collect2 initrd/usr/local/bin/; 		cp -f $(NATIVE_GCC)/libexec/gcc/x86_64-hanos/10.2.0/lto1 initrd/usr/local/bin/; 		cp -f $(NATIVE_BINUTILS)/x86_64-hanos-as initrd/usr/local/bin/as; 		cp -f $(NATIVE_BINUTILS)/x86_64-hanos-ld initrd/usr/local/bin/ld; 		mkdir -p initrd/usr/local/lib/gcc/x86_64-hanos/10.2.0; 		cp -f $(NATIVE_GCC)/lib/gcc/x86_64-hanos/10.2.0/libgcc.a initrd/usr/local/lib/gcc/x86_64-hanos/10.2.0/; 		cp -f $(CROSS_GCC)/crtbegin.o $(CROSS_GCC)/crtend.o initrd/usr/local/lib/gcc/x86_64-hanos/10.2.0/; 		cp -f $(CROSS_GCC)/crtbegin.o $(CROSS_GCC)/crtend.o initrd/usr/local/lib/; 		cp -f $(NATIVE_GCC)/lib/gcc/x86_64-hanos/10.2.0/libgcc.a initrd/usr/local/lib/; 		cp -f $(CROSS_GCC)/crtbegin.o $(CROSS_GCC)/crtend.o initrd/lib/; 		cp -f $(NATIVE_GCC)/lib/gcc/x86_64-hanos/10.2.0/libgcc.a initrd/lib/; 		cp -f $(SYSROOT_STAGE)/usr/lib/libm.a initrd/lib/; 		cp -f $(NATIVE_GCC)/lib/libstdc++.a initrd/lib/; 		cp -f $(NATIVE_GCC)/lib/libstdc++.a initrd/usr/local/lib/; 		cp -f $(NATIVE_GCC)/lib/libstdc++.so initrd/lib/; 		cp -f $(NATIVE_GCC)/lib/libstdc++.so.6 initrd/lib/; 		cp -f $(NATIVE_GCC)/lib/libstdc++.so initrd/usr/local/lib/; 		cp -f $(NATIVE_GCC)/lib/libstdc++.so.6 initrd/usr/local/lib/; 		mkdir -p initrd/usr/local/include; 		cp -a $(NATIVE_GCC)/include/c++ initrd/usr/local/include/; 		mkdir -p initrd/include; 		cp -a $(NATIVE_GCC)/include/c++ initrd/include/; 		rm -f initrd/usr/libexec/gcc/x86_64-hanos/10.2.0/cc1 initrd/usr/libexec/gcc/x86_64-hanos/10.2.0/cc1plus initrd/usr/libexec/gcc/x86_64-hanos/10.2.0/lto1; 	fi
	mkdir -p initrd/include
	tar --dereference --hard-dereference -cvpf $(INITRD_TAR) -C $(TARGET_ROOT) bin assets etc usr root data lib include sbin

$(PORTS_HDD_INITRD): $(INITRD_TAR) $(PORTS_STAMP)
	rm -rf initrd-ports-hdd
	mkdir -p initrd-ports-hdd
	tar -xpf $(INITRD_TAR) -C initrd-ports-hdd
	mkdir -p initrd-ports-hdd/etc
	touch initrd-ports-hdd/etc/ports-image
	tar -cpf $@ -C initrd-ports-hdd bin assets etc usr root data lib sbin
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
	truncate -s 512M $(ISO_IMAGE)
	rm -rf iso_root

$(HDD_IMAGE): limine $(HDD_INITRD) kernel $(IMAGE_MODE) $(PORTS_DEP) scripts/build-hdd-image.sh scripts/guest-stress.sh
	scripts/build-hdd-image.sh $(HDD_INITRD)


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

