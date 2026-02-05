[BITS 64]
[GLOBAL _start]
[EXTERN kernel_main]
[EXTERN __bss_start]
[EXTERN __bss_end]

_start:
    ; Set up a stack safely above kernel .bss (below 2MB)
    mov rsp, 0x1F0000

    ; Zero .bss/.lbss (freestanding, no runtime)
    lea rdi, [rel __bss_start]
    lea rax, [rel __bss_end]
    mov rcx, rax
    sub rcx, rdi
    xor eax, eax
    rep stosb

    ; Call C kernel
    call kernel_main

    ; Halt if kernel returns
.halt:
    hlt
    jmp .halt
