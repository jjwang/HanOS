.section .rodata
.global term_font_norm
.type term_font_norm, @object
.align 8

term_font_norm:
    .incbin "device/display/gohufont-14.psf"

.global term_font_bold
.type term_font_bold, @object
.align 8

term_font_bold:
    .incbin "device/display/gohufont-14b.psf"

