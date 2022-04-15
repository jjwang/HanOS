KERNEL := hanos.elf
BOOT_DIR := 3rd-party/boot
TRAMPOLINEFILE = core/trampoline.nasm

CC = x86_64-elf-gcc
LD = $(CC)
AS = $(CC)
NASM = nasm

CFLAGS =
ASFLAGS = -I . -flto

INTERNALLDFLAGS :=  \
	-flto           \
	-Wno-lto-type-mismatch \
	-no-pie         \
	-nostdlib       \
	-Tlinker.ld     \
	-z max-page-size=0x2000 \
	-I .

INTERNALCFLAGS :=       \
	-I. -Wall -Wextra -Ofast -mcmodel=kernel \
	-std=gnu2x          \
	-ffreestanding      \
	-flto               \
	-fno-pic            \
    -fno-omit-frame-pointer \
	-mno-3dnow          \
	-mno-80387          \
	-mno-mmx            \
	-mno-sse            \
	-mno-sse2           \
	-mno-red-zone       \
    -O2

CFILES := $(shell find ./ -type f -name '*.c')
COBJ   := $(CFILES:.c=.o)

ASMFILES := $(shell find ./ -type f -name '*.s')
ASMOBJ := $(ASMFILES:.s=.o)

SYMBOLS = _symbols.o
TRAMPOLINEBLOB = core/trampoline.bin

.PHONY: all clean

all: $(KERNEL)

$(KERNEL): $(TRAMPOLINEBLOB) $(COBJ) $(ASMOBJ)
	$(CC) $(INTERNALLDFLAGS) $(COBJ) $(ASMOBJ) -o $@

	@echo ">>>>> Generating symbols..."
	@./gensym $@
	$(CC) $(CFLAGS) $(INTERNALCFLAGS) -x c -c _symbols.gen -o $(SYMBOLS)

	@echo ">>>>> Linking symbols..."
	$(CC) $(INTERNALLDFLAGS) $(COBJ) $(ASMOBJ) $(SYMBOLS) -o $@

	@echo ">>>>> Re-generating symbols..."
	@./gensym $@
	$(CC) $(CFLAGS) $(INTERNALCFLAGS) -x c -c _symbols.gen -o $(SYMBOLS)

	@echo ">>>>> Re-linking symbols..."
	$(CC) $(INTERNALLDFLAGS) $(COBJ) $(ASMOBJ) $(SYMBOLS) -o $@

stivale2.h:
	$(shell [ -d "$(BOOT_DIR)" ] || mkdir -p $(BOOT_DIR))
	@if [ ! -e "$(BOOT_DIR)/stivale2.h" ]; then wget -P $(BOOT_DIR) https://github.com/stivale/stivale/raw/master/stivale2.h; fi

$(COBJ): %.o: %.c stivale2.h
	@echo Compiling $^ ...
	$(CC) $(CFLAGS) $(INTERNALCFLAGS) -c $< -o $@

$(ASMOBJ): %.o: %.s
	@echo Compiling $^ ...
	@$(AS) -o $@ -c $^ $(ASFLAGS)

# build the trampoline blob
$(TRAMPOLINEBLOB): $(TRAMPOLINEFILE)
	@echo Compiling trampoline file... $^
	@$(NASM) $^ -o $@

clean:
	rm -rf $(KERNEL) $(COBJ) $(ASMOBJ) _symbols.* $(TRAMPOLINEBLOB)
