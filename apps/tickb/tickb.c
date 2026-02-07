// tickb - Test user task B
// Writes 'B' markers to VGA memory to demonstrate multitasking

#include "../libsys.h"

// VGA text buffer at 0xB8000
#define VGA_BASE 0xB8000
#define VGA_WIDTH 80
#define VGA_HEIGHT 25

static volatile unsigned short *vga = (volatile unsigned short *)VGA_BASE;

// Simple delay loop
static void delay(int count) {
    for (volatile int i = 0; i < count; i++) {
        __asm__ volatile ("nop");
    }
}

int main(void) {
    int pos = 40;  // Start at column 40
    int row = 10;  // Use row 10 for task B (same row, different side)
    unsigned char color = 0x0B;  // Cyan on black

    while (1) {
        // Write 'B' at current position in row
        vga[row * VGA_WIDTH + pos] = (color << 8) | 'B';

        // Move to next position (wrap between columns 40-79)
        pos = 40 + ((pos - 40 + 1) % 40);

        // Delay and yield to other tasks
        delay(500000);
        yield();
    }

    return 0;
}
