#include "isr.h"
#include "drivers/keyboard.h"
#include "sched.h"

// System tick counter (incremented by timer IRQ ~18.2 times/sec by default PIT)
volatile uint64_t system_ticks = 0;
extern volatile int in_syscall;
extern volatile uint64_t sched_last_next_cs;
extern volatile int sched_last_next_is_user;
extern volatile int sched_dbg_runq;
extern volatile int sched_dbg_user;
extern volatile int sched_dbg_ready;
extern volatile int sched_dbg_running;
extern volatile int sched_dbg_has_runq;
extern volatile int sched_dbg_has_current;
extern volatile int sched_dbg_in_syscall;
extern volatile int sched_dbg_inited;
extern volatile int sched_dbg_bootstrap;
extern volatile int sched_dbg_created_user;
extern volatile int sched_dbg_created_kernel;

// Video memory for debug output
static volatile unsigned short *video = (volatile unsigned short *)0xB8000;

#define SCREEN_WIDTH 80

static void print_at(const char *str, int x, int y, unsigned char color) {
    volatile unsigned short *pos = video + (y * SCREEN_WIDTH) + x;
    while (*str) {
        *pos++ = (color << 8) | *str++;
    }
}

static void print_hex(uint64_t n, int x, int y) {
    char hex[] = "0x0000000000000000";
    char *digits = "0123456789ABCDEF";
    for (int i = 17; i >= 2; i--) {
        hex[i] = digits[n & 0xF];
        n >>= 4;
    }
    print_at(hex, x, y, 0x0F);
}

static const char *exception_names[] = {
    "Division by Zero",
    "Debug",
    "NMI",
    "Breakpoint",
    "Overflow",
    "Bound Range Exceeded",
    "Invalid Opcode",
    "Device Not Available",
    "Double Fault",
    "Coprocessor Segment Overrun",
    "Invalid TSS",
    "Segment Not Present",
    "Stack Fault",
    "General Protection Fault",
    "Page Fault",
    "Reserved",
    "x87 FPU Error",
    "Alignment Check",
    "Machine Check",
    "SIMD Floating Point",
    "Virtualization",
    "Control Protection",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved", "Reserved", "Reserved",
    "Reserved", "Reserved"
};

static void debug_tick_line(struct irq_frame *frame) {
    static const char spinner[] = "|/-\\";
    char spin[2];
    spin[0] = spinner[system_ticks & 3];
    spin[1] = '\0';

    print_at("DBG", 0, 24, 0x0E);
    print_at(spin, 4, 24, 0x0E);
    print_at("T:", 6, 24, 0x0E);

    struct task *t = sched_current();
    if (t) {
        print_hex(t->id, 8, 24);
        print_at(t->is_user ? "U" : "K", 27, 24, 0x0E);
    } else {
        print_at("none", 8, 24, 0x0E);
    }

    char mode = 'K';
    if (frame && (frame->cs & 0x3)) {
        mode = 'U';
    }
    print_at("C:", 29, 24, 0x0E);
    char m[2] = { mode, 0 };
    print_at(m, 31, 24, 0x0E);

    print_at("N:", 33, 24, 0x0E);
    char nm[2] = { sched_last_next_is_user ? 'U' : 'K', 0 };
    print_at(nm, 35, 24, 0x0E);
    print_at("NC:", 37, 24, 0x0E);
    char nc = (char)('0' + (sched_last_next_cs & 0x3));
    char ncv[2] = { nc, 0 };
    print_at(ncv, 40, 24, 0x0E);
    print_at("I:", 42, 24, 0x0E);
    char iv[2] = { (char)('0' + (in_syscall ? 1 : 0)), 0 };
    print_at(iv, 44, 24, 0x0E);
    print_at("R:", 46, 24, 0x0E);
    char rc = (char)('0' + (sched_dbg_runq & 0xF));
    char rcv[2] = { rc, 0 };
    print_at(rcv, 48, 24, 0x0E);
    print_at("U:", 50, 24, 0x0E);
    char uc = (char)('0' + (sched_dbg_user & 0xF));
    char ucv[2] = { uc, 0 };
    print_at(ucv, 52, 24, 0x0E);
    print_at("S:", 54, 24, 0x0E);
    char sr = (char)('0' + sched_dbg_ready);
    char srw[2] = { sr, 0 };
    print_at(srw, 56, 24, 0x0E);
    print_at("G:", 58, 24, 0x0E);
    char sg = (char)('0' + sched_dbg_running);
    char sgw[2] = { sg, 0 };
    print_at(sgw, 60, 24, 0x0E);
    print_at("Q:", 62, 24, 0x0E);
    char sq = (char)('0' + sched_dbg_has_runq);
    char sqw[2] = { sq, 0 };
    print_at(sqw, 64, 24, 0x0E);
    print_at("C:", 66, 24, 0x0E);
    char sc = (char)('0' + sched_dbg_has_current);
    char scw[2] = { sc, 0 };
    print_at(scw, 68, 24, 0x0E);
    print_at("I:", 70, 24, 0x0E);
    char si = (char)('0' + sched_dbg_in_syscall);
    char siw[2] = { si, 0 };
    print_at(siw, 72, 24, 0x0E);
    print_at("B:", 74, 24, 0x0E);
    char sb = (char)('0' + (sched_dbg_bootstrap ? 1 : 0));
    char sbw[2] = { sb, 0 };
    print_at(sbw, 76, 24, 0x0E);
    print_at("U:", 78, 24, 0x0E);
    char su = (char)('0' + (sched_dbg_created_user & 0xF));
    char suw[2] = { su, 0 };
    print_at(suw, 79, 24, 0x0E);

    volatile uint8_t *shared = (volatile uint8_t *)0x180000;
    char ca = (char)shared[0];
    char cb = (char)shared[1];
    if (ca == 0) ca = '.';
    if (cb == 0) cb = '.';
    (void)ca;
    (void)cb;
}

void isr_handler(uint64_t int_no) {
    print_at("EXCEPTION: ", 0, 10, 0x0C);
    if (int_no < 32) {
        print_at(exception_names[int_no], 11, 10, 0x0C);
    }
    print_at("INT#: ", 0, 11, 0x0C);
    print_hex(int_no, 6, 11);

    // Halt on exception
    while (1) {
        __asm__ volatile ("hlt");
    }
}

struct irq_frame *irq_handler(uint64_t int_no, struct irq_frame *frame) {
    if (int_no == 32) {
        // Timer interrupt - increment system tick counter
        system_ticks++;
        debug_tick_line(frame);
        frame = sched_tick(frame);
    } else if (int_no == 33) {
        // Keyboard interrupt - read scancode and pass to keyboard driver
        uint8_t scancode;
        __asm__ volatile ("inb %1, %0" : "=a"(scancode) : "Nd"((uint16_t)0x60));
        keyboard_handle_scancode(scancode);
    }

    // Send End of Interrupt (EOI) to PIC
    if (int_no >= 40) {
        __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0x20), "Nd"((uint16_t)0xA0));
    }
    __asm__ volatile ("outb %0, %1" : : "a"((uint8_t)0x20), "Nd"((uint16_t)0x20));

    return frame;
}
