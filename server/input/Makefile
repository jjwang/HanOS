CC = x86_64-elf-gcc
LD = $(CC)
AS = $(CC)
NASM = nasm

CFLAGS =
ASFLAGS = -I . -flto
ASM_FLAGS = -f elf64 -I.

INTERNALLDFLAGS :=  \
	-flto           \
	-Wno-lto-type-mismatch \
	-no-pie         \
	-fno-builtin    \
	-nostdlib       \
	-Tlinker.ld     \
	-z max-page-size=0x2000 \
	-I .            \
	-O2

INTERNALCFLAGS :=       \
	-std=gnu11          \
	-I.                 \
 	-ffreestanding      \
	-flto               \
 	-fno-pic            \
 	-fno-omit-frame-pointer \
 	-mno-3dnow          \
 	-mno-red-zone       \
 	-Wno-cast-function-type \
	-O2

LIBFILES := ./lib/string.o ./lib/memutils.o

CFILES := $(shell find ./ -type f,l -name '*.c')
COBJ   := $(CFILES:.c=.o)

ASM_FILES := $(shell find ./ -type f,l -name '*.asm')
ASM_OBJ := $(ASM_FILES:.asm=.o)

CELF   := input 

.PHONY: all

all: $(COBJ) $(ASM_OBJ) $(CELF)
	mkdir -p ../../initrd/server
	cp $(CELF) ../../initrd/server

$(COBJ): %.o: %.c
	@echo Compiling $^ ...
	$(CC) $(CFLAGS) $(INTERNALCFLAGS) -c $< -o $@

$(ASM_OBJ): %.o: %.asm
	@echo Compiling $^ ...
	@$(NASM) $(ASM_FLAGS) $< -o $@

$(CELF): %: %.o
	$(LD) $(CFLAGS) $(INTERNALLDFLAGS) $(ASM_OBJ) $(LIBFILES) $< -o $@

clean:
	rm -rf $(COBJ) $(ASM_OBJ) $(CELF)
	rm -rf ../../initrd/server/*
