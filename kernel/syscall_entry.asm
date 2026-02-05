; Syscall entry point for x86_64
DEFAULT ABS
; When syscall executes:
;   - RCX = return RIP (user's next instruction)
;   - R11 = saved RFLAGS
;   - RAX = syscall number
;   - RDI, RSI, RDX, R10, R8, R9 = arguments 1-6
; Note: We use R10 instead of RCX for arg4 because RCX is clobbered

section .text
global syscall_entry
extern syscall_handler
extern in_syscall

syscall_entry:
    ; Save user stack pointer and switch to kernel stack
    mov [user_rsp], rsp
    lea rsp, [kernel_syscall_stack_top]
    mov dword [in_syscall], 1

    ; Save registers we need to preserve
    push rcx        ; return RIP
    push r11        ; saved RFLAGS
    push rbx        ; callee-saved
    push rbp
    push r12
    push r13
    push r14
    push r15

    ; Set up arguments for syscall_handler(num, arg1, arg2, arg3, arg4, arg5)
    mov r9, r8      ; arg5 -> sixth param
    mov r8, r10     ; arg4 -> fifth param
    mov rcx, rdx    ; arg3 -> fourth param
    mov rdx, rsi    ; arg2 -> third param
    mov rsi, rdi    ; arg1 -> second param
    mov rdi, rax    ; syscall_num -> first param

    sti
    ; Call the C handler
    call syscall_handler
    cli

    ; Return value is in RAX - leave it there

    ; Restore registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop r11         ; RFLAGS for sysret
    pop rcx         ; return RIP for sysret

    ; Restore user stack
    mov rsp, [user_rsp]
    mov dword [in_syscall], 0

    ; Return to user mode
    o64 sysret

section .data
    user_rsp: dq 0

section .bss
    align 16
    kernel_syscall_stack: resb 8192  ; 8KB stack for syscalls
    kernel_syscall_stack_top:
