[BITS 64]

global ctx_switch
global user_mode_enter

; void ctx_switch(uint64_t *old_rsp, uint64_t new_rsp, uint64_t new_cr3)
ctx_switch:
    push rbp
    push rbx
    push r12
    push r13
    push r14
    push r15
    mov [rdi], rsp        ; save old rsp
    mov rsp, rsi          ; load new rsp
    mov cr3, rdx          ; switch address space
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbx
    pop rbp
    ret

; void user_mode_enter(uint64_t rip, uint64_t rsp)
; Uses current CR3. Drops to ring 3 with CS=0x23, SS=0x1B
user_mode_enter:
    mov rcx, rdi      ; user RIP
    mov rdx, rsi      ; user RSP
    push qword 0x1B   ; SS
    push rdx          ; RSP
    push qword 0x202  ; RFLAGS with IF
    push qword 0x23   ; CS
    push rcx          ; RIP
    ; Clear registers for cleanliness
    xor rax, rax
    xor rbx, rbx
    xor rcx, rcx
    xor rdx, rdx
    xor rsi, rsi
    xor rdi, rdi
    xor r8, r8
    xor r9, r9
    xor r10, r10
    xor r11, r11
    xor r12, r12
    xor r13, r13
    xor r14, r14
    xor r15, r15
    iretq
