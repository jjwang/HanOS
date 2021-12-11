ENTRY(kmain)

offset = 0xffffffff80000000;

SECTIONS
{
    . = offset + 0x100000;

    kernel_start = .;

    .stivale2hdr : ALIGN(4K) {
        KEEP(*(.stivale2hdr))
    }

    .text : {
        *(.text .text.*)
    }
    
    .rodata : {
        *(.rodata .rodata.*)
    }
    
    .data : {
        *(.data .data.*)
    }
    
    .bss : {
        . = ALIGN(16);
        *(.bss .bss.*)
        *(COMMON)
    }
    
    kernel_end = .;
}
