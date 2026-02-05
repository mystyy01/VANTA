#include "idt.h"

#define IDT_ENTRIES 256
#define PIT_HZ 100
#define PIT_FREQ 1193182

static struct idt_entry idt[IDT_ENTRIES];
static struct idt_ptr idtp;

// External ISR handlers (defined in isr.asm)
extern void isr0(void);
extern void isr1(void);
extern void isr2(void);
extern void isr3(void);
extern void isr4(void);
extern void isr5(void);
extern void isr6(void);
extern void isr7(void);
extern void isr8(void);
extern void isr9(void);
extern void isr10(void);
extern void isr11(void);
extern void isr12(void);
extern void isr13(void);
extern void isr14(void);
extern void isr15(void);
extern void isr16(void);
extern void isr17(void);
extern void isr18(void);
extern void isr19(void);
extern void isr20(void);
extern void isr21(void);
extern void isr22(void);
extern void isr23(void);
extern void isr24(void);
extern void isr25(void);
extern void isr26(void);
extern void isr27(void);
extern void isr28(void);
extern void isr29(void);
extern void isr30(void);
extern void isr31(void);
extern void irq0(void);
extern void irq1(void);

static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

void idt_set_gate(int n, uint64_t handler) {
    idt[n].offset_low = handler & 0xFFFF;
    idt[n].selector = 0x08;  // Kernel code segment
    idt[n].ist = 0;
    idt[n].type_attr = 0x8E; // Present, Ring 0, Interrupt Gate
    idt[n].offset_mid = (handler >> 16) & 0xFFFF;
    idt[n].offset_high = (handler >> 32) & 0xFFFFFFFF;
    idt[n].zero = 0;
}

// Remap PIC to avoid conflicts with CPU exceptions
static void pic_remap(void) {
    // ICW1: Start initialization
    outb(0x20, 0x11);
    outb(0xA0, 0x11);

    // ICW2: Vector offsets (IRQ 0-7 -> INT 32-39, IRQ 8-15 -> INT 40-47)
    outb(0x21, 0x20);
    outb(0xA1, 0x28);

    // ICW3: PIC cascading
    outb(0x21, 0x04);
    outb(0xA1, 0x02);

    // ICW4: 8086 mode
    outb(0x21, 0x01);
    outb(0xA1, 0x01);

    // Mask all interrupts except IRQ0 (timer) and IRQ1 (keyboard)
    outb(0x21, 0xFC);
    outb(0xA1, 0xFF);
}

static void pit_init(void) {
    uint16_t divisor = (uint16_t)(PIT_FREQ / PIT_HZ);
    outb(0x43, 0x36);
    outb(0x40, (uint8_t)(divisor & 0xFF));
    outb(0x40, (uint8_t)((divisor >> 8) & 0xFF));
}

void idt_init(void) {
    // Set up CPU exception handlers (0-31)
    idt_set_gate(0, (uint64_t)isr0);
    idt_set_gate(1, (uint64_t)isr1);
    idt_set_gate(2, (uint64_t)isr2);
    idt_set_gate(3, (uint64_t)isr3);
    idt_set_gate(4, (uint64_t)isr4);
    idt_set_gate(5, (uint64_t)isr5);
    idt_set_gate(6, (uint64_t)isr6);
    idt_set_gate(7, (uint64_t)isr7);
    idt_set_gate(8, (uint64_t)isr8);
    idt_set_gate(9, (uint64_t)isr9);
    idt_set_gate(10, (uint64_t)isr10);
    idt_set_gate(11, (uint64_t)isr11);
    idt_set_gate(12, (uint64_t)isr12);
    idt_set_gate(13, (uint64_t)isr13);
    idt_set_gate(14, (uint64_t)isr14);
    idt_set_gate(15, (uint64_t)isr15);
    idt_set_gate(16, (uint64_t)isr16);
    idt_set_gate(17, (uint64_t)isr17);
    idt_set_gate(18, (uint64_t)isr18);
    idt_set_gate(19, (uint64_t)isr19);
    idt_set_gate(20, (uint64_t)isr20);
    idt_set_gate(21, (uint64_t)isr21);
    idt_set_gate(22, (uint64_t)isr22);
    idt_set_gate(23, (uint64_t)isr23);
    idt_set_gate(24, (uint64_t)isr24);
    idt_set_gate(25, (uint64_t)isr25);
    idt_set_gate(26, (uint64_t)isr26);
    idt_set_gate(27, (uint64_t)isr27);
    idt_set_gate(28, (uint64_t)isr28);
    idt_set_gate(29, (uint64_t)isr29);
    idt_set_gate(30, (uint64_t)isr30);
    idt_set_gate(31, (uint64_t)isr31);

    // Remap PIC
    pic_remap();

    // Set up IRQ handlers (32+)
    idt_set_gate(32, (uint64_t)irq0);  // Timer
    idt_set_gate(33, (uint64_t)irq1);  // Keyboard

    // Load IDT
    idtp.limit = sizeof(idt) - 1;
    idtp.base = (uint64_t)&idt;
    __asm__ volatile ("lidt %0" : : "m"(idtp));

    // Program PIT to periodic interrupts
    pit_init();

    // Interrupts are enabled later once scheduling is ready.
}
