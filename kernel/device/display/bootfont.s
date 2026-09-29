.section .rodata
.global boot_font_norm
.type boot_font_norm, @object
.align 8

boot_font_norm:
    .incbin "device/display/gohufont-14.psf"

.global boot_font_bold
.type boot_font_bold, @object
.align 8

boot_font_bold:
    .incbin "device/display/gohufont-14b.psf"
