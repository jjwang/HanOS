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
	-I .    \
	-O2


INTERNALCFLAGS :=       \
	-std=gnu2x          \
	-O2


CFILES := $(shell find ./ -type f -name '*.c')
COBJ   := $(CFILES:.c=.o)
CELF   := $(CFILES:.c=.elf)

all: $(COBJ) $(CELF)
	cp $(CELF) ../tgroot/bin

$(COBJ): %.o: %.c
	$(CC) $(CFLAGS) $(INTERNALCFLAGS) -c $< -o $@

$(CELF): %.elf: %.o
	$(LD) $(CFLAGS) $(INTERNALLDFLAGS) $< -o $@

clean:
	rm -rf $(COBJ) $(CELF)
	rm -rf ../tgroot/bin/*.elf
