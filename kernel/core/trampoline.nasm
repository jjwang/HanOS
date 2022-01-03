LOAD_ADDRESS        equ 0x70000
HIGHERHALF_OFFSET   equ 0xffffffff80000000
AP_BOOT_COUNTER     equ 0xff0
ARG_CPUINFO         equ 0xfe0
ARG_CR3_VAL         equ 0xfd0
ARG_ENTRYPOINT      equ 0xfc0
ARG_RSP             equ 0xfb0
ARG_IDTPTR          equ 0xfa0

bits 16

rmode_entry:
    cli
    jmp load_gdt32

gdt32_start:
    dq 0x0000000000000000   ; null segment
    dq 0x00df9a000000ffff   ; code segment
    dq 0x00df92000000ffff   ; data segment

gdt32_ptr:
    dw 23
    dd LOAD_ADDRESS + gdt32_start

gdt64_start:
    dq 0x0000000000000000   ; null segment
    dq 0x00AF9A000000FFFF   ; code segment
    dq 0x008F92000000FFFF   ; data segment

gdt64_ptr:
    dw 23
    dq LOAD_ADDRESS + gdt64_start

load_gdt32:
    lgdt [cs:gdt32_ptr]
    mov eax, cr0
    or eax, 0x1
    mov cr0, eax
    jmp dword 0x08:LOAD_ADDRESS + pmode_entry

bits 32

pmode_entry:
    ; update segment registers
    mov eax, 0x10
    mov es, eax
    mov ss, eax
    mov ds, eax
    mov fs, eax
    mov gs, eax

    ; update cr3
    mov eax, [ARG_CR3_VAL]
    mov cr3, eax

    ; enable pae
    mov eax, cr4
    or eax, 1 << 5
    mov cr4, eax

    ; enable long mode
    mov ecx, 0xc0000080
    rdmsr
    or eax, 1 << 8
    wrmsr

    ; enable paging
    mov eax, cr0
    or eax, 1 << 31
    mov cr0, eax

    ; load 64-bit GDT
    lgdt [LOAD_ADDRESS + gdt64_ptr]
    jmp 0x08:LOAD_ADDRESS + lmode_entry

bits 64

lmode_entry:
    ; load idt
    lidt [ARG_IDTPTR]

    ; initialize stack
    mov rsp, [ARG_RSP]

    ; pass cpu information to kernel code
    mov rdi, [ARG_CPUINFO]

    ; increment counter to indicate successful boot
    lock add word [AP_BOOT_COUNTER], 1
    
    ; jump to kernel code
    call [ARG_ENTRYPOINT]
