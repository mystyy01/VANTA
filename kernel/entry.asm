[BITS 64]
[GLOBAL _start]
[EXTERN kernel_main]

_start:
    ; Set up a stack (16KB at 0x90000)
    mov rsp, 0x90000

    ; Call C kernel
    call kernel_main

    ; Halt if kernel returns
.halt:
    hlt
    jmp .halt
