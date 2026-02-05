[BITS 64]

; Export all ISR/IRQ symbols
global isr0, isr1, isr2, isr3, isr4, isr5, isr6, isr7
global isr8, isr9, isr10, isr11, isr12, isr13, isr14, isr15
global isr16, isr17, isr18, isr19, isr20, isr21, isr22, isr23
global isr24, isr25, isr26, isr27, isr28, isr29, isr30, isr31
global irq0, irq1

; Import C handler
extern isr_handler
extern irq_handler

; Macro for ISRs that don't push an error code
%macro ISR_NOERR 1
isr%1:
    push 0          ; Dummy error code
    push %1         ; Interrupt number
    jmp isr_common
%endmacro

; Macro for ISRs that push an error code
%macro ISR_ERR 1
isr%1:
    push %1         ; Interrupt number (error code already pushed by CPU)
    jmp isr_common
%endmacro

; Macro for IRQs
%macro IRQ 2
irq%1:
    push 0          ; Dummy error code
    push %2         ; Interrupt number
    jmp irq_common
%endmacro

; CPU Exceptions
ISR_NOERR 0   ; Division by zero
ISR_NOERR 1   ; Debug
ISR_NOERR 2   ; NMI
ISR_NOERR 3   ; Breakpoint
ISR_NOERR 4   ; Overflow
ISR_NOERR 5   ; Bound range exceeded
ISR_NOERR 6   ; Invalid opcode
ISR_NOERR 7   ; Device not available
ISR_ERR   8   ; Double fault
ISR_NOERR 9   ; Coprocessor segment overrun
ISR_ERR   10  ; Invalid TSS
ISR_ERR   11  ; Segment not present
ISR_ERR   12  ; Stack fault
ISR_ERR   13  ; General protection fault
ISR_ERR   14  ; Page fault
ISR_NOERR 15  ; Reserved
ISR_NOERR 16  ; x87 FPU error
ISR_ERR   17  ; Alignment check
ISR_NOERR 18  ; Machine check
ISR_NOERR 19  ; SIMD floating point
ISR_NOERR 20  ; Virtualization
ISR_ERR   21  ; Control protection
ISR_NOERR 22
ISR_NOERR 23
ISR_NOERR 24
ISR_NOERR 25
ISR_NOERR 26
ISR_NOERR 27
ISR_NOERR 28
ISR_NOERR 29
ISR_NOERR 30
ISR_NOERR 31

; Hardware IRQs
IRQ 0, 32    ; Timer
IRQ 1, 33    ; Keyboard

; Common ISR handler
isr_common:
    ; Save all registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Call C handler with interrupt number as argument
    mov rdi, [rsp + 120]  ; Get interrupt number (15 regs * 8 = 120)
    call isr_handler

    ; Restore all registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16     ; Remove error code and interrupt number
    iretq

; Common IRQ handler
irq_common:
    ; Save all registers
    push rax
    push rbx
    push rcx
    push rdx
    push rsi
    push rdi
    push rbp
    push r8
    push r9
    push r10
    push r11
    push r12
    push r13
    push r14
    push r15

    ; Call C handler with interrupt number and frame pointer
    mov rdi, [rsp + 120]
    mov rsi, rsp
    call irq_handler
    test rax, rax
    jnz .have_frame
    mov rax, rsp
.have_frame:
    mov rsp, rax

    ; Restore all registers
    pop r15
    pop r14
    pop r13
    pop r12
    pop r11
    pop r10
    pop r9
    pop r8
    pop rbp
    pop rdi
    pop rsi
    pop rdx
    pop rcx
    pop rbx
    pop rax

    add rsp, 16
    iretq
