.global smp_trampoline_blob_start
.global smp_trampoline_blob_end

smp_trampoline_blob_start:
    .incbin "arch/x64/trampoline.bin"
smp_trampoline_blob_end:

