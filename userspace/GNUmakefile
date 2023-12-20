CC   = x86_64-elf-gcc
LD   = $(CC)
AS   = $(CC)
NASM = nasm

AS_FLAGS  = -I . -flto
ASM_FLAGS = -f elf64 -I.

LC_FLAGS :=                 \
    -std=gnu11              \
    -I. -I..                \
    -ffreestanding          \
    -flto                   \
    -fno-pic                \
    -fno-omit-frame-pointer \
    -mno-3dnow              \
    -mno-red-zone           \
    -Wno-cast-function-type \
    -O2 -mcmodel=large

LD_FLAGS :=                 \
	-flto                   \
	-Wno-lto-type-mismatch  \
	-no-pie                 \
	-fno-builtin            \
	-nostdlib               \
	-Tlinker.ld             \
	-z max-page-size=0x2000 \
	-I. -I..                \
	-O2 -mcmodel=large

LIB_FILES :=

C_FILES   := $(shell find ./ -type f,l -name '*.c')
C_OBJS    := $(C_FILES:.c=.o)
C_DEPS    := $(C_FILES:.c=.d)

LIBC_FILES:= $(shell find ../libc -type f,l -name '*.c')
LIBC_OBJS := $(patsubst %.c, %.u.o, ${LIBC_FILES})
LIBC_DEPS := $(patsubst %.c, %.u.d, ${LIBC_FILES})

ASM_FILES := $(shell find ./ -type f,l -name '*.asm')
ASM_OBJS  := $(ASM_FILES:.asm=.o)

CELF      := init hansh echo cat wc ls pwd help rm

.PHONY: clean all

all: $(C_OBJS) $(ASM_OBJS) $(LIBC_OBJS) $(CELF)
	mkdir -p ../initrd/bin
	cp $(CELF) ../initrd/bin

-include $(C_DEPS)
$(C_OBJS): %.o: %.c
	@./genhelp
	$(CC) $(LC_FLAGS) -c _help.c -o _help.o
	@echo Compiling $^ ...
	$(CC) -MM -I. -MT $@ -MF $(patsubst %.o, %.d, $@) $<
	$(CC) $(LC_FLAGS) -c $< -o $@

-include $(LIBC_DEPS)
$(LIBC_OBJS): %.u.o: %.c 
	@echo Compiling $^ ...
	$(CC) -MM -I. -MT $@ -MF $(patsubst %.u.o, %.u.d, $@) $<
	$(CC) $(LC_FLAGS) -c $< -o $@

$(ASM_OBJS): %.o: %.asm
	@echo Compiling $^ ...
	@$(NASM) $(ASM_FLAGS) $< -o $@

$(CELF): %: %.o $(ASM_OBJS) $(C_OBJS) $(LIBC_OBJS) _help.o
	$(LD) $(LD_FLAGS) $(ASM_OBJS) $(LIBC_OBJS) $(LIB_FILES) _help.o $< -o $@

clean:
	rm -rf $(C_OBJS) $(C_DEPS) $(LIBC_OBJS) $(LIBC_DEPS) $(ASM_OBJS) $(CELF) sh.sym
	rm -rf ../initrd/bin/* _help.c
