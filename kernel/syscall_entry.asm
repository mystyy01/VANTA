; Syscall entry point for x86_64
DEFAULT ABS
; When syscall executes:
;   - RCX = return RIP (user's next instruction)
;   - R11 = saved RFLAGS
;   - RAX = syscall number
;   - RDI, RSI, RDX, R10, R8, R9 = arguments 1-6
;
; IMPORTANT: All user context is saved on the per-task kernel stack,
; NOT in globals.  This prevents race conditions when two user tasks
; are both inside syscalls (one preempted, one active).
;
; The globals (user_ctx_*) are written ONLY while interrupts are disabled
; (SYSCALL masks IF via FMASK) and are a snapshot for fork() to read
; during the current syscall invocation.

section .text
global syscall_entry
extern syscall_handler
extern current_kernel_rsp

; Snapshot of user context for fork() — only valid during current syscall
global user_ctx_rsp
global user_ctx_rip
global user_ctx_rflags
global user_ctx_rbx
global user_ctx_rbp
global user_ctx_r12
global user_ctx_r13
global user_ctx_r14
global user_ctx_r15

syscall_entry:
    ; Interrupts are DISABLED here (FMASK cleared IF).
    ; Save user context snapshot to globals for fork().
    mov [user_ctx_rsp], rsp
    mov [user_ctx_rip], rcx
    mov [user_ctx_rflags], r11
    mov [user_ctx_rbx], rbx
    mov [user_ctx_rbp], rbp
    mov [user_ctx_r12], r12
    mov [user_ctx_r13], r13
    mov [user_ctx_r14], r14
    mov [user_ctx_r15], r15

    ; Switch to per-task kernel stack
    mov rsp, [current_kernel_rsp]

    ; Save user context on the PER-TASK kernel stack (safe across preemption).
    ; This is what we restore from — NOT the globals.
    push qword [user_ctx_rsp]   ; user RSP
    push rcx                    ; user RIP (= return address)
    push r11                    ; user RFLAGS
    push rbx                    ; callee-saved registers
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
    call syscall_handler
    cli

    ; Return value is in RAX — leave it there

    ; Restore callee-saved registers from kernel stack
    pop r15
    pop r14
    pop r13
    pop r12
    pop rbp
    pop rbx
    pop r11         ; user RFLAGS for SYSRET
    pop rcx         ; user RIP for SYSRET
    pop rsp         ; user RSP — restores user stack directly

    ; Return to user mode
    o64 sysret

section .data
    user_ctx_rsp:    dq 0
    user_ctx_rip:    dq 0
    user_ctx_rflags: dq 0
    user_ctx_rbx:    dq 0
    user_ctx_rbp:    dq 0
    user_ctx_r12:    dq 0
    user_ctx_r13:    dq 0
    user_ctx_r14:    dq 0
    user_ctx_r15:    dq 0
