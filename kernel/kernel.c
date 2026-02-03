// PHOBOS Kernel

#include "idt.h"
#include "drivers/ata.h"
#include "drivers/keyboard.h"
#include "fs/fat32.h"
#include "fs/vfs.h"
#include "paging.h"
#include "syscall.h"

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
    print("PHOBOS - 64-bit C Kernel", 0);

    // Initialize paging with user-accessible pages
    paging_init();

    // Initialize keyboard and interrupts
    keyboard_init();
    idt_init();

    // Initialize syscall mechanism
    syscall_init();

    // Initialize ATA and mount filesystem
    ata_init();
    ata_select_drive(ATA_DRIVE_SLAVE);

    if (fat32_init(0) == 0) {
        print_color("FAT32 mounted", 1, 0x0A);
        vfs_set_root(fat32_get_root());
        // Create standard directories
        ensure_path_exists("/apps");
        ensure_path_exists("/core");
        ensure_path_exists("/users/root");
        ensure_path_exists("/cfg");
        ensure_path_exists("/temp");
        ensure_path_exists("/dev");
    } else {
        print_color("FAT32 failed", 1, 0x0C);
    }

    print("Starting mt-shell...", 3);

    // Call mt-shell main (defined in mt-shell/shell.mtc)
    extern int shell_main(void);
    shell_main();

    // If shell exits, halt
    print("Shell exited. System halted.", 5);
    while (1) {
        __asm__ volatile ("hlt");
    }
}
