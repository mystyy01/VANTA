#ifndef ISR_H
#define ISR_H

#include <stdint.h>

struct irq_frame {
    uint64_t r15, r14, r13, r12, r11, r10, r9, r8;
    uint64_t rbp, rdi, rsi, rdx, rcx, rbx, rax;
    uint64_t int_no;
    uint64_t err_code;
    uint64_t rip;
    uint64_t cs;
    uint64_t rflags;
};

struct irq_frame_user {
    struct irq_frame base;
    uint64_t rsp;
    uint64_t ss;
};

void isr_handler(uint64_t int_no);
struct irq_frame *irq_handler(uint64_t int_no, struct irq_frame *frame);

#endif
