ENTRY(_start)
SECTIONS
{
    .text 0x0000400000000000 :
    {
        *(.text*)
        . = ALIGN(4096);
    }

    .rodata :
    {
        *(.rodata*)
        . = ALIGN(4096);
    }

    .init_array :
    {
        __init_array_start = .;
        *(.init_array*)
        __init_array_end = .;
        . = ALIGN(4096);
    }

    .fini_array :
    {
        __fini_array_start = .;
        *(.fini_array*)
        __fini_array_end = .;
        . = ALIGN(4096);
    }

    .data :
    {
        *(.data*)
        . = ALIGN(4096);
    }

    .bss :
    {
        __bss_start = .;
        *(.bss*)
        *(COMMON)
        __bss_end = .;
        . = ALIGN(4096);
    }

    _end = .;

    /* musl's crt1 loads this into a register and never dereferences it. Give it
     * an in-range address so the PC-relative relocation resolves at the high
     * userspace load address. */
    _DYNAMIC = 0x0000400010000000;
}
