#include <stdint.h>

#define VGA_BUFFER ((volatile uint16_t*)0xB8000)
#define VGA_COLS 80

static void write_str(const char *s, uint8_t color, int row, int col) {
    volatile uint16_t *pos = VGA_BUFFER + row * VGA_COLS + col;
    while (*s) {
        *pos++ = ((uint16_t)color << 8) | (uint8_t)(*s++);
    }
}

int main(int argc, char **argv) {
    (void)argc; (void)argv;
    write_str("Hello from ELF loader!", 0x0A, 12, 10);
    return 0;
}
