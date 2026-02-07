// GDT and TSS for 64-bit long mode
// Sets up kernel/user segments and TSS for privilege transitions

#include "gdt.h"

// GDT selectors (must match bootloader layout)
// 0x00: Null
// 0x08: Ring 0 code (kernel)
// 0x10: Ring 0 data
// 0x18: Ring 3 data (selector 0x1B with RPL 3)
// 0x20: Ring 3 code (selector 0x23 with RPL 3)
// 0x28: TSS (16 bytes in 64-bit mode)

struct gdt_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit;
    uint8_t  base_high;
} __attribute__((packed));

struct tss_entry {
    uint16_t limit_low;
    uint16_t base_low;
    uint8_t  base_mid;
    uint8_t  access;
    uint8_t  flags_limit;
    uint8_t  base_high;
    uint32_t base_upper;
    uint32_t reserved;
} __attribute__((packed));

struct gdt_ptr {
    uint16_t limit;
    uint64_t base;
} __attribute__((packed));

// TSS structure for 64-bit mode
struct tss {
    uint32_t reserved0;
    uint64_t rsp0;      // Ring 0 stack pointer
    uint64_t rsp1;      // Ring 1 stack pointer (unused)
    uint64_t rsp2;      // Ring 2 stack pointer (unused)
    uint64_t reserved1;
    uint64_t ist1;      // IST1-7: Interrupt stack table
    uint64_t ist2;
    uint64_t ist3;
    uint64_t ist4;
    uint64_t ist5;
    uint64_t ist6;
    uint64_t ist7;
    uint64_t reserved2;
    uint16_t reserved3;
    uint16_t iopb_offset;
} __attribute__((packed));

// GDT with 5 segment entries + 1 TSS entry (which is 16 bytes = 2 slots)
static struct {
    struct gdt_entry entries[5];
    struct tss_entry tss;
} __attribute__((packed, aligned(16))) gdt;

static struct tss tss;
static struct gdt_ptr gdt_ptr;

static void gdt_set_entry(int idx, uint32_t base, uint32_t limit, uint8_t access, uint8_t flags) {
    gdt.entries[idx].base_low = base & 0xFFFF;
    gdt.entries[idx].base_mid = (base >> 16) & 0xFF;
    gdt.entries[idx].base_high = (base >> 24) & 0xFF;
    gdt.entries[idx].limit_low = limit & 0xFFFF;
    gdt.entries[idx].flags_limit = ((limit >> 16) & 0x0F) | (flags & 0xF0);
    gdt.entries[idx].access = access;
}

static void gdt_set_tss(uint64_t base, uint32_t limit) {
    gdt.tss.limit_low = limit & 0xFFFF;
    gdt.tss.base_low = base & 0xFFFF;
    gdt.tss.base_mid = (base >> 16) & 0xFF;
    gdt.tss.access = 0x89;  // Present, 64-bit TSS (available)
    gdt.tss.flags_limit = ((limit >> 16) & 0x0F);
    gdt.tss.base_high = (base >> 24) & 0xFF;
    gdt.tss.base_upper = (base >> 32) & 0xFFFFFFFF;
    gdt.tss.reserved = 0;
}

void gdt_init(void) {
    // Zero TSS
    for (int i = 0; i < (int)sizeof(tss); i++) {
        ((uint8_t *)&tss)[i] = 0;
    }
    tss.iopb_offset = sizeof(tss);

    // Null descriptor
    gdt_set_entry(0, 0, 0, 0, 0);

    // Ring 0 code: selector 0x08
    // Access: Present, DPL 0, Code, Executable, Readable
    gdt_set_entry(1, 0, 0xFFFFF, 0x9A, 0xA0);  // 0xA0 = Long mode, 4KB granularity

    // Ring 0 data: selector 0x10
    // Access: Present, DPL 0, Data, Writable
    gdt_set_entry(2, 0, 0xFFFFF, 0x92, 0xC0);  // 0xC0 = 32-bit (ignored for data in long mode)

    // Ring 3 data: selector 0x18 (0x1B with RPL 3)
    // Access: Present, DPL 3, Data, Writable
    gdt_set_entry(3, 0, 0xFFFFF, 0xF2, 0xC0);

    // Ring 3 code: selector 0x20 (0x23 with RPL 3)
    // Access: Present, DPL 3, Code, Executable, Readable
    gdt_set_entry(4, 0, 0xFFFFF, 0xFA, 0xA0);  // 0xA0 = Long mode

    // TSS: selector 0x28
    gdt_set_tss((uint64_t)&tss, sizeof(tss) - 1);

    // Set up GDT pointer
    gdt_ptr.limit = sizeof(gdt) - 1;
    gdt_ptr.base = (uint64_t)&gdt;

    // Load GDT
    __asm__ volatile (
        "lgdt %0\n"
        // Reload segment registers
        "mov $0x10, %%ax\n"
        "mov %%ax, %%ds\n"
        "mov %%ax, %%es\n"
        "mov %%ax, %%ss\n"
        "mov %%ax, %%fs\n"
        "mov %%ax, %%gs\n"
        // Far jump to reload CS
        "pushq $0x08\n"
        "lea 1f(%%rip), %%rax\n"
        "pushq %%rax\n"
        "lretq\n"
        "1:\n"
        :
        : "m"(gdt_ptr)
        : "rax", "memory"
    );

    // Load TSS
    __asm__ volatile (
        "mov $0x28, %%ax\n"
        "ltr %%ax\n"
        :
        :
        : "rax"
    );
}

void tss_set_rsp0(uint64_t rsp0) {
    tss.rsp0 = rsp0;
}
