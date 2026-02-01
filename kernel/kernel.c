// VANTA Kernel

#include "idt.h"
#include "drivers/ata.h"
#include "drivers/keyboard.h"
#include "fs/fat32.h"
#include "fs/vfs.h"

// Video memory starts at 0xB8000
// Each character: 2 bytes (char + color)
// Color: 0x0F = white on black

volatile unsigned short *video = (volatile unsigned short *)0xB8000;

void print(const char *str, int row) {
    volatile unsigned short *pos = video + (row * 80);
    while (*str) {
        *pos++ = (0x0F << 8) | *str++;
    }
}

void print_color(const char *str, int row, unsigned char color) {
    volatile unsigned short *pos = video + (row * 80);
    while (*str) {
        *pos++ = (color << 8) | *str++;
    }
}

void kernel_main(void) {
    print("VANTA OS - 64-bit C Kernel", 0);

    // Initialize keyboard and interrupts
    keyboard_init();
    idt_init();

    // Initialize ATA and mount filesystem
    ata_init();
    ata_select_drive(ATA_DRIVE_SLAVE);

    if (fat32_init(0) == 0) {
        print_color("FAT32 mounted", 1, 0x0A);
        vfs_set_root(fat32_get_root());
    } else {
        print_color("FAT32 failed", 1, 0x0C);
    }

    print("Shell ready. Type:", 3);

    // Simple shell loop - you'll replace this with mt-shell
    int col = 0;
    int row = 4;
    while (1) {
        struct key_event ev = keyboard_get_event();

        if (ev.key == '\n') {
            // Enter pressed
            col = 0;
            row++;
            if (row >= 25) row = 24;
        } else if (ev.key == '\b') {
            // Backspace
            if (col > 0) {
                col--;
                video[row * 80 + col] = (0x0F << 8) | ' ';
            }
        } else if (ev.key >= 0x20 && ev.key < 0x7F) {
            // Printable character
            video[row * 80 + col] = (0x0F << 8) | ev.key;
            col++;
            if (col >= 80) {
                col = 0;
                row++;
            }
        }

        // Show ctrl state for testing
        if (ev.modifiers & MOD_CTRL) {
            video[0 * 80 + 70] = (0x0E << 8) | 'C';
        } else {
            video[0 * 80 + 70] = (0x0F << 8) | ' ';
        }
    }
}
