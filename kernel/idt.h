#ifndef IDT_H
#define IDT_H

#include <stdint.h>

// IDT entry for 64-bit mode
struct idt_entry {
    uint16_t offset_low;
    uint16_t selector;
    uint8_t ist;          // Interrupt Stack Table offset
    uint8_t type_attr;    // Type and attributes
    uint16_t offset_mid;
    uint32_t offset_high;
    uint32_t zero;
} __attribute__((packed));

struct idt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

void idt_init(void);
void idt_set_gate(int n, uint64_t handler);

#endif
