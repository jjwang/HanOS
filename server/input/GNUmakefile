CC   = x86_64-elf-gcc
LD   = $(CC)
AS   = $(CC)
NASM = nasm

AS_FLAGS  = -I . -flto
ASM_FLAGS = -f elf64 -I.

LC_FLAGS :=                 \
    -std=gnu11              \
    -I.                     \
    -ffreestanding          \
    -flto                   \
    -fno-pic                \
    -fno-omit-frame-pointer \
    -mno-3dnow              \
    -mno-red-zone           \
    -Wno-cast-function-type \
    -O2

LD_FLAGS :=                 \
	-flto                   \
	-Wno-lto-type-mismatch  \
	-no-pie                 \
	-fno-builtin            \
	-nostdlib               \
	-Tlinker.ld             \
	-z max-page-size=0x2000 \
	-I .                    \
	-O2

LIB_FILES := ./lib/string.o ./lib/memutils.o

C_FILES   := $(shell find ./ -type f,l -name '*.c')
C_OBJS    := $(C_FILES:.c=.o)
C_DEPS    := $(C_FILES:.c=.d)

ASM_FILES := $(shell find ./ -type f,l -name '*.asm')
ASM_OBJS  := $(ASM_FILES:.asm=.o)

CELF      := input 

.PHONY: all

all: $(C_OBJS) $(ASM_OBJS) $(CELF)
	mkdir -p ../../initrd/server
	cp $(CELF) ../../initrd/server

-include $(C_DEPS)
$(C_OBJS): %.o: %.c
	@echo Compiling $^ ...
	$(CC) -MM -I. -MT $@ -MF $(patsubst %.o, %.d, $@) $<
	$(CC) $(LC_FLAGS) -c $< -o $@

$(ASM_OBJS): %.o: %.asm
	@echo Compiling $^ ...
	@$(NASM) $(ASM_FLAGS) $< -o $@

$(CELF): %: %.o
	$(LD) $(LD_FLAGS) $(ASM_OBJS) $(LIB_FILES) $< -o $@

clean:
	rm -rf $(C_OBJS) $(C_DEPS) $(ASM_OBJS) $(CELF)
	rm -rf ../../initrd/server/*
