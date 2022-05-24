OUTPUT_FORMAT(elf64-x86-64)
OUTPUT_ARCH(i386:x86-64)

ENTRY(kmain)

PHDRS
{
    null    PT_NULL    FLAGS(0) ;                   /* Null segment */
    text    PT_LOAD    FLAGS((1 << 0) | (1 << 2)) ; /* Execute + Read */
    rodata  PT_LOAD    FLAGS((1 << 2)) ;            /* Read only */
    data    PT_LOAD    FLAGS((1 << 1) | (1 << 2)) ; /* Write + Read */
}

offset = 0xffffffff80000000;

SECTIONS
{
    /* Modification will cause boot failure from physical hard disk */
    . = 0xffffffff80000000 + 0x200000;

    kernel_start = .;

    .text : {
        *(.text .text.*)
    } :text
      
    /* Move to the next memory page for .rodata */
    . += CONSTANT(MAXPAGESIZE);

    .rodata : {
        *(.rodata .rodata.*)
    } :rodata
    
    /* Move to the next memory page for .data */
    . += CONSTANT(MAXPAGESIZE);

    .data : {
        *(.data .data.*)
    } :data
    
    .bss : {
        . = ALIGN(16);
        *(.bss .bss.*)
        *(COMMON)
    } :data
    
    kernel_end = .;
}
