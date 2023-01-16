CC   = x86_64-elf-gcc
LD   = $(CC)
AS   = $(CC)
NASM = nasm

AS_FLAGS  = -I . -flto
ASM_FLAGS = -f elf64 -I.

LC_FLAGS :=             \
    -std=gnu11          \
    -I.                 \
    -ffreestanding      \
    -flto               \
    -fno-pic            \
    -fno-omit-frame-pointer \
    -mno-3dnow          \
    -mno-red-zone       \
    -Wno-cast-function-type \
    -O2 -mcmodel=large

LD_FLAGS :=             \
	-flto               \
	-Wno-lto-type-mismatch \
	-no-pie             \
	-fno-builtin        \
	-nostdlib           \
	-Tlinker.ld         \
	-z max-page-size=0x2000 \
	-I .                \
	-O2 -mcmodel=large

LIB_FILES := lib/string.o lib/memutils.o lib/re.o lib/ctype.o lib/command.o

C_FILES   := $(shell find ./ -type f,l -name '*.c')
C_OBJ     := $(C_FILES:.c=.o)
C_DEPS    := $(C_FILES:.c=.d)

ASM_FILES := $(shell find ./ -type f,l -name '*.asm')
ASM_OBJ   := $(ASM_FILES:.asm=.o)

CELF      := sh

.PHONY: clean all

all: $(C_OBJ) $(ASM_OBJ) $(CELF)
	mkdir -p ../initrd/bin
	cp $(CELF) ../initrd/bin

-include $(C_DEPS)
$(C_OBJ): %.o: %.c
	@echo Compiling $^ ...
	$(CC) -MM -I. -MT $@ -MF $(patsubst %.o, %.d, $@) $<
	$(CC) $(LC_FLAGS) -c $< -o $@

$(ASM_OBJ): %.o: %.asm
	@echo Compiling $^ ...
	@$(NASM) $(ASM_FLAGS) $< -o $@

$(CELF): %: %.o $(ASM_OBJ) $(C_OBJ)
	$(LD) $(LD_FLAGS) $(ASM_OBJ) $(LIB_FILES) $< -o $@

clean:
	rm -rf $(C_OBJ) $(C_DEPS) $(ASM_OBJ) $(CELF)
	rm -rf ../initrd/bin/*
