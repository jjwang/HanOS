section .rodata
global term_font_norm
global term_font_bold

term_font_norm:
    incbin "../kernel/device/display/gohufont-14.psf"

term_font_bold:
    incbin "../kernel/device/display/gohufont-14b.psf"
