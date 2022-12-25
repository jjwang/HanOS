ENTRY(main)
SECTIONS
{

    .text 0xFFFF800050000000 :
    {
        *(.text)
        . = ALIGN(4096);
    }

    .data :
    {
        *(.data)
        *(.rodata)
        . = ALIGN(4096);
    }

    .bss :
    {
        *(.bss)
        . = ALIGN(4096);
    }
}
