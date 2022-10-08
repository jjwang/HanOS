KERNEL := hanos.elf
BOOT_DIR := 3rd-party/boot
TRAMPOLINEFILE = core/trampoline.nasm

CC = x86_64-elf-gcc
LD = $(CC)
AS = $(CC)
NASM = nasm

CFLAGS =
AS_FLAGS = -I . -flto
ASM_FLAGS = -f elf64 -I.

INTERNALLDFLAGS :=  \
	-flto           \
	-Wno-lto-type-mismatch \
	-no-pie         \
	-nostdlib       \
	-Tlinker.ld     \
	-z max-page-size=0x2000 \
	-I .    \
    -O2

INTERNALCFLAGS :=       \
	-I. -Wall -Wextra -mcmodel=kernel \
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
	-Wno-cast-function-type \
	-O2


CFILES := $(shell find ./ -type f -name '*.c')
COBJ   := $(CFILES:.c=.o)

AS_FILES := $(shell find ./ -type f -name '*.s')
AS_OBJ := $(AS_FILES:.s=.o)

ASM_FILES := $(shell find ./ -type f -name '*.asm')
ASM_OBJ := $(ASM_FILES:.asm=.o)

SYMBOLS = _symbols.o
TRAMPOLINEBLOB = core/trampoline.bin

.PHONY: all clean

all: $(KERNEL)

$(KERNEL): $(TRAMPOLINEBLOB) $(COBJ) $(AS_OBJ) $(ASM_OBJ)
	$(LD) $(INTERNALLDFLAGS) $(COBJ) $(AS_OBJ) $(ASM_OBJ)  -o $@

	@echo ">>>>> Generating symbols..."
	@./gensym $@
	$(CC) $(CFLAGS) $(INTERNALCFLAGS) -x c -c _symbols.gen -o $(SYMBOLS)

	@echo ">>>>> Linking symbols..."
	$(LD) $(INTERNALLDFLAGS) $(COBJ) $(AS_OBJ) $(ASM_OBJ) $(SYMBOLS) -o $@

	@echo ">>>>> Re-generating symbols..."
	@./gensym $@
	$(CC) $(CFLAGS) $(INTERNALCFLAGS) -x c -c _symbols.gen -o $(SYMBOLS)

	@echo ">>>>> Re-linking symbols..."
	$(LD) $(INTERNALLDFLAGS) $(COBJ) $(AS_OBJ) $(ASM_OBJ) $(SYMBOLS) -o $@

limine.h:
	$(shell [ -d "$(BOOT_DIR)" ] || mkdir -p $(BOOT_DIR))
	@if [ ! -e "$(BOOT_DIR)/limine.h" ]; then wget -P $(BOOT_DIR) https://raw.githubusercontent.com/limine-bootloader/limine/trunk/limine.h; fi

$(COBJ): %.o: %.c limine.h
	@echo Compiling $^ ...
	$(CC) $(CFLAGS) $(INTERNALCFLAGS) -c $< -o $@

$(AS_OBJ): %.o: %.s
	@echo Compiling $^ ...
	@$(AS) -o $@ -c $^ $(AS_FLAGS)

$(ASM_OBJ): %.o: %.asm 
	@echo Compiling $^ ...
	@$(NASM) $(ASM_FLAGS) $< -o $@

# build the trampoline blob
$(TRAMPOLINEBLOB): $(TRAMPOLINEFILE)
	@echo Compiling trampoline file... $^
	@$(NASM) $^ -o $@

clean:
	rm -rf $(KERNEL) $(COBJ) $(AS_OBJ) $(ASM_OBJ) _symbols.* $(TRAMPOLINEBLOB)
