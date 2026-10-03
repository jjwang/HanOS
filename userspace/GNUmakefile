CC       := x86_64-elf-gcc
LD       := $(CC)
AS       := $(CC)
NASM     := nasm

AS_FLAGS := -I . -flto
ASM_FLAGS:= -f elf64 -I.

INDENT   := indent -kr -i4 -nut

# musl provides the C library. The HanOS syscall and keycode runtime lives in
# userspace/runtime and links next to musl.
MUSL      := ../3rdparty/musl
MUSL_LIB  := $(MUSL)/lib
MUSL_CRT1 := $(MUSL)/obj/crt/crt1.o
MUSL_CRTI := $(MUSL)/obj/crt/x86_64/crti.o
MUSL_CRTN := $(MUSL)/obj/crt/x86_64/crtn.o

LC_FLAGS :=                 \
	-std=gnu11              \
	-nostdinc               \
	-I$(MUSL)/arch/x86_64   \
	-I$(MUSL)/arch/generic  \
	-I$(MUSL)/obj/include   \
	-I$(MUSL)/include       \
	-I. -Iinclude -I.. -I../include                \
	-ffreestanding          \
	-fno-pic                \
	-fno-omit-frame-pointer \
	-mgeneral-regs-only     \
	-mno-red-zone           \
	-Wno-cast-function-type \
	-O2 -mcmodel=large

LD_FLAGS :=                 \
	-no-pie                 \
	-nostdlib               \
	-T../musl/linker.ld     \
	-O2 -mcmodel=large

LIB_FILES :=

C_FILES   := $(shell find ./ -type f,l -name '*.c' ! -name '_help.c' ! -path './runtime/*')
H_FILES   := $(shell find ./ -type f,l -name '*.h')
C_OBJS    := $(C_FILES:.c=.o)
C_DEPS    := $(C_FILES:.c=.d)

RUNTIME_FILES := runtime/sysfunc.c runtime/keycode.c
RUNTIME_OBJS  := $(patsubst %.c, %.u.o, ${RUNTIME_FILES})
RUNTIME_DEPS  := $(patsubst %.c, %.u.d, ${RUNTIME_FILES})

ASM_FILES := $(shell find ./ -type f,l -name '*.asm')
ASM_OBJS  := $(ASM_FILES:.asm=.o)

HONOS_RT  := $(RUNTIME_OBJS)

CELF      := servers/block servers/console servers/fat32 servers/input servers/net servers/pipe servers/process servers/tty servers/vfs bin/cat bin/echo bin/hansh bin/help bin/init bin/ls bin/pwd bin/rm bin/wc test/crtest test/fdtest test/cloexectest test/nettest test/musltest test/faulttest

.PHONY: clean all musl

all: $(C_OBJS) $(ASM_OBJS) $(RUNTIME_OBJS) $(CELF)
	mkdir -p ../initrd/bin
	cp $(CELF) ../initrd/bin

musl: $(MUSL_LIB)/libc.a

# Rebuild musl only when a port input changes, not for README edits.
MUSL_INPUTS := ../musl/build.sh ../musl/syscall_arch.h ../musl/__set_thread_area.s

$(MUSL_LIB)/libc.a: $(MUSL_INPUTS)
	../musl/build.sh

$(MUSL_CRT1) $(MUSL_CRTI) $(MUSL_CRTN): $(MUSL_LIB)/libc.a

indent: $(C_FILES) $(H_FILES)
	@for file in $(C_FILES); do \
		$(INDENT) $$file;       \
		rm -f $${file}~;        \
	done
	@for file in $(H_FILES); do \
		$(INDENT) $$file;       \
		rm -f $${file}~;        \
	done

_help.c: $(C_FILES) genhelp
	@./genhelp

_help.o: _help.c
	$(CC) $(LC_FLAGS) -c $< -o $@

-include $(C_DEPS)
$(C_OBJS): %.o: %.c
	@echo Compiling $^ ...
	$(CC) -MM $(LC_FLAGS) -MT $@ -MF $(patsubst %.o, %.d, $@) $<
	$(CC) $(LC_FLAGS) -c $< -o $@

-include $(RUNTIME_DEPS)
$(RUNTIME_OBJS): %.u.o: %.c
	@echo Compiling $^ ...
	$(CC) -MM $(LC_FLAGS) -MT $@ -MF $(patsubst %.u.o, %.u.d, $@) $<
	$(CC) $(LC_FLAGS) -c $< -o $@

$(ASM_OBJS): %.o: %.asm
	@echo Compiling $^ ...
	@$(NASM) $(ASM_FLAGS) $< -o $@

$(CELF): %: %.o $(HONOS_RT) _help.o font.o $(MUSL_CRT1)
	$(LD) $(LD_FLAGS) \
		$(MUSL_CRT1) $(MUSL_CRTI) $< $(HONOS_RT) font.o _help.o \
		-L$(MUSL_LIB) -lc -lgcc $(MUSL_CRTN) -o $@

clean:
	rm -rf $(C_OBJS) $(C_DEPS) $(RUNTIME_OBJS) $(RUNTIME_DEPS) $(ASM_OBJS) $(CELF) sh.sym
	rm -rf ../initrd/bin/* _help.c _help.o
