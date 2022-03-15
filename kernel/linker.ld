ENTRY(kmain)

offset = 0xffffffff80000000;

SECTIONS
{
    /* Modification will cause boot failure from physical hard disk */
    . = offset + 0x200000;

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
