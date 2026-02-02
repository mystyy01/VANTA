// Bare-metal C library for mt-shell on VANTA OS
// Links against VANTA kernel

#include "../kernel/drivers/keyboard.h"
#include "../kernel/fs/vfs.h"

// ============================================================================
// Memory Allocator (bump allocator with static buffer)
// ============================================================================

#define HEAP_SIZE 131072  // 128KB heap for shell

static char heap[HEAP_SIZE];
static int heap_offset = 0;

char* malloc(int size) {
    int aligned_size = (size + 7) & ~7;
    if (heap_offset + aligned_size > HEAP_SIZE) {
        return (char*)0;
    }
    char* ptr = &heap[heap_offset];
    heap_offset += aligned_size;
    return ptr;
}

void free(void* ptr) {
    // Bump allocator doesn't free - would need a real allocator
    (void)ptr;
}

char* realloc(void* ptr, int new_size) {
    // Simple bump allocator realloc - just allocate new memory
    // (old memory is leaked, but we don't have a proper allocator)
    if (ptr == (void*)0) {
        return malloc(new_size);
    }
    char* new_ptr = malloc(new_size);
    if (!new_ptr) return (char*)0;
    // Can't determine old size, so caller should copy manually if needed
    // For arrays, the compiler handles this
    return new_ptr;
}

void* memcpy(void* dst, const void* src, int n) {
    char* d = (char*)dst;
    const char* s = (const char*)src;
    while (n-- > 0) {
        *d++ = *s++;
    }
    return dst;
}

void* memset(void* s, int c, int n) {
    char* p = (char*)s;
    while (n-- > 0) {
        *p++ = (char)c;
    }
    return s;
}

// ============================================================================
// String Functions
// ============================================================================

int strlen(const char* s) {
    const char* p = s;
    while (*p) p++;
    return (int)(p - s);
}

void char_to_string(char c, char* out) {
    out[0] = c;
    out[1] = '\0';
}

int strcmp(const char* a, const char* b) {
    while (*a && *a == *b) {
        a++;
        b++;
    }
    return (unsigned char)*a - (unsigned char)*b;
}

char* strcpy(char* dst, const char* src) {
    char* ret = dst;
    while ((*dst++ = *src++));
    return ret;
}

char* strncpy(char* dst, const char* src, int n) {
    char* ret = dst;
    while (n > 0 && *src) {
        *dst++ = *src++;
        n--;
    }
    while (n > 0) {
        *dst++ = '\0';
        n--;
    }
    return ret;
}

char* strcat(char* dst, const char* src) {
    char* p = dst;
    while (*p) p++;
    while (*src) {
        *p++ = *src++;
    }
    *p = '\0';
    return dst;
}

char* strncat(char* dst, const char* src, int n) {
    char* p = dst;
    while (*p) p++;
    while (n > 0 && *src) {
        *p++ = *src++;
        n--;
    }
    *p = '\0';
    return dst;
}

char* concat_strings(const char* a, const char* b) {
    int len_a = strlen(a);
    int len_b = strlen(b);
    int total = len_a + len_b + 1;
    char* result = malloc(total);
    if (!result) return (char*)0;
    char* dst = result;
    while (*a) *dst++ = *a++;
    while (*b) *dst++ = *b++;
    *dst = '\0';
    return result;
}

int tolower(int c) {
    if (c >= 'A' && c <= 'Z') {
        return c + ('a' - 'A');
    }
    return c;
}

// ============================================================================
// VGA Text Mode Output
// ============================================================================

#define VGA_WIDTH 80
#define VGA_HEIGHT 25
#define VGA_BUFFER ((volatile unsigned short*)0xB8000)
#define VGA_COLOR 0x0F  // White on black

static int vga_row = 0;
static int vga_col = 0;

void vga_scroll(void) {
    // Move all rows up by one
    for (int y = 0; y < VGA_HEIGHT - 1; y++) {
        for (int x = 0; x < VGA_WIDTH; x++) {
            VGA_BUFFER[y * VGA_WIDTH + x] = VGA_BUFFER[(y + 1) * VGA_WIDTH + x];
        }
    }
    // Clear bottom row
    for (int x = 0; x < VGA_WIDTH; x++) {
        VGA_BUFFER[(VGA_HEIGHT - 1) * VGA_WIDTH + x] = (VGA_COLOR << 8) | ' ';
    }
}

void print_char(int c) {
    if (c == '\n') {
        vga_col = 0;
        vga_row++;
    } else if (c == '\r') {
        vga_col = 0;
    } else if (c == '\b') {
        if (vga_col > 0) {
            vga_col--;
            VGA_BUFFER[vga_row * VGA_WIDTH + vga_col] = (VGA_COLOR << 8) | ' ';
        }
    } else if (c == '\t') {
        vga_col = (vga_col + 8) & ~7;
    } else {
        VGA_BUFFER[vga_row * VGA_WIDTH + vga_col] = (VGA_COLOR << 8) | (unsigned char)c;
        vga_col++;
    }

    if (vga_col >= VGA_WIDTH) {
        vga_col = 0;
        vga_row++;
    }

    while (vga_row >= VGA_HEIGHT) {
        vga_scroll();
        vga_row = VGA_HEIGHT - 1;
    }
}

void mt_print(const char* s) {
    while (*s) {
        print_char(*s++);
    }
}

void print_int(int n) {
    if (n < 0) {
        print_char('-');
        n = -n;
    }
    if (n == 0) {
        print_char('0');
        return;
    }
    char buf[12];
    int i = 0;
    while (n > 0) {
        buf[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i > 0) {
        print_char(buf[--i]);
    }
}

// Format an integer to a string buffer, return pointer to end
static char* format_int_to_buf(char* buf, int n) {
    if (n < 0) {
        *buf++ = '-';
        n = -n;
    }
    if (n == 0) {
        *buf++ = '0';
        return buf;
    }
    char tmp[12];
    int i = 0;
    while (n > 0) {
        tmp[i++] = '0' + (n % 10);
        n /= 10;
    }
    while (i > 0) {
        *buf++ = tmp[--i];
    }
    return buf;
}

// Simple printf - supports %s and %d
int printf(const char* fmt, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    int count = 0;

    while (*fmt) {
        if (*fmt == '%' && *(fmt + 1)) {
            fmt++;
            if (*fmt == 's') {
                const char* s = __builtin_va_arg(args, const char*);
                if (s) {
                    while (*s) {
                        print_char(*s++);
                        count++;
                    }
                }
            } else if (*fmt == 'd') {
                int val = __builtin_va_arg(args, int);
                print_int(val);
                count += 10; // approximate
            } else if (*fmt == '%') {
                print_char('%');
                count++;
            } else {
                // Unknown format - print as-is
                print_char('%');
                print_char(*fmt);
                count += 2;
            }
        } else {
            print_char(*fmt);
            count++;
        }
        fmt++;
    }
    __builtin_va_end(args);
    return count;
}

// Simple sprintf - supports %s and %d
int sprintf(char* buf, const char* fmt, ...) {
    __builtin_va_list args;
    __builtin_va_start(args, fmt);
    char* start = buf;

    while (*fmt) {
        if (*fmt == '%' && *(fmt + 1)) {
            fmt++;
            if (*fmt == 's') {
                const char* s = __builtin_va_arg(args, const char*);
                if (s) {
                    while (*s) {
                        *buf++ = *s++;
                    }
                }
            } else if (*fmt == 'd') {
                int val = __builtin_va_arg(args, int);
                buf = format_int_to_buf(buf, val);
            } else if (*fmt == '%') {
                *buf++ = '%';
            } else {
                *buf++ = '%';
                *buf++ = *fmt;
            }
        } else {
            *buf++ = *fmt;
        }
        fmt++;
    }
    *buf = '\0';
    __builtin_va_end(args);
    return (int)(buf - start);
}

// Exit - halt the system (in bare-metal, just spin)
void exit(int code) {
    mt_print("exit(");
    print_int(code);
    mt_print(")\n");
    // Halt CPU
    while (1) {
        __asm__ volatile ("hlt");
    }
}

void clear_screen(void) {
    for (int i = 0; i < VGA_WIDTH * VGA_HEIGHT; i++) {
        VGA_BUFFER[i] = (VGA_COLOR << 8) | ' ';
    }
    vga_row = 0;
    vga_col = 0;
}

void set_cursor(int row, int col) {
    vga_row = row;
    vga_col = col;
}

// ============================================================================
// Keyboard Input
// ============================================================================

static char line_buffer[512];
static int line_pos = 0;

char* read_line(void) {
    line_pos = 0;

    while (1) {
        struct key_event ev = keyboard_get_event();

        if (!ev.pressed) continue;  // Only handle key presses

        if (ev.key == '\n') {
            print_char('\n');
            line_buffer[line_pos] = '\0';
            return line_buffer;
        } else if (ev.key == '\b') {
            if (line_pos > 0) {
                line_pos--;
                print_char('\b');
            }
        } else if (ev.key >= 0x20 && ev.key < 0x7F) {
            if (line_pos < 510) {
                line_buffer[line_pos++] = ev.key;
                print_char(ev.key);
            }
        }
        // Handle Ctrl+C - return empty line
        if ((ev.modifiers & MOD_CTRL) && (ev.key == 'c' || ev.key == 'C')) {
            mt_print("^C\n");
            line_buffer[0] = '\0';
            return line_buffer;
        }
    }
}

// ============================================================================
// Filesystem Interface
// ============================================================================

static char cwd[VFS_MAX_PATH] = "/";

char* get_cwd(void) {
    return cwd;
}

int set_cwd(const char* path) {
    // Handle relative paths
    char full_path[VFS_MAX_PATH];

    if (path[0] == '/') {
        strncpy(full_path, path, VFS_MAX_PATH - 1);
        full_path[VFS_MAX_PATH - 1] = '\0';
    } else {
        // Relative path - combine with cwd
        int cwd_len = strlen(cwd);
        strcpy(full_path, cwd);
        if (cwd_len > 0 && cwd[cwd_len - 1] != '/') {
            full_path[cwd_len] = '/';
            full_path[cwd_len + 1] = '\0';
        }
        int remaining = VFS_MAX_PATH - strlen(full_path) - 1;
        strncat(full_path, path, remaining);
    }

    // Verify path exists and is a directory
    struct vfs_node* node = vfs_resolve_path(full_path);
    if (!node) {
        return -1;  // Path not found
    }
    if (!(node->flags & VFS_DIRECTORY)) {
        return -2;  // Not a directory
    }

    strncpy(cwd, full_path, VFS_MAX_PATH - 1);
    cwd[VFS_MAX_PATH - 1] = '\0';
    return 0;
}

int file_exists(const char* path) {
    struct vfs_node* node = vfs_resolve_path(path);
    return node != (void*)0;
}

char* read_file(const char* path) {
    struct vfs_node* node = vfs_resolve_path(path);
    if (!node) return "";
    if (!(node->flags & VFS_FILE)) return "";

    char* buffer = malloc(node->size + 1);
    if (!buffer) return "";

    int bytes_read = vfs_read(node, 0, node->size, (uint8_t*)buffer);
    if (bytes_read < 0) {
        buffer[0] = '\0';
        return buffer;
    }

    buffer[bytes_read] = '\0';
    return buffer;
}

int write_file(const char* path, const char* content) {
    struct vfs_node* node = vfs_resolve_path(path);
    if (!node) return -1;

    int len = strlen(content);
    return vfs_write(node, 0, len, (const uint8_t*)content);
}

// List directory - returns array of names
// For mt-lang, we'll build a simple linked structure
typedef struct dir_entry_list {
    char name[VFS_MAX_NAME];
    struct dir_entry_list* next;
} dir_entry_list;

static dir_entry_list* dir_entries = (void*)0;
static int dir_entry_count = 0;

// Get directory entries as array (for mt-lang)
int list_dir_count(const char* path) {
    struct vfs_node* node = vfs_resolve_path(path);
    if (!node) return 0;
    if (!(node->flags & VFS_DIRECTORY)) return 0;

    dir_entry_count = 0;
    uint32_t index = 0;
    struct dirent* entry;

    while ((entry = vfs_readdir(node, index)) != (void*)0) {
        dir_entry_count++;
        index++;
    }

    return dir_entry_count;
}

char* list_dir_entry(const char* path, int index) {
    struct vfs_node* node = vfs_resolve_path(path);
    if (!node) return "";

    struct dirent* entry = vfs_readdir(node, index);
    if (!entry) return "";

    return entry->name;
}

// Simple array-style list_dir for mt-lang
// Returns newline-separated list of entries
char* list_dir(const char* path) {
    struct vfs_node* node = vfs_resolve_path(path);
    if (!node) return "";
    if (!(node->flags & VFS_DIRECTORY)) return "";

    // First pass: calculate size
    int total_size = 0;
    uint32_t index = 0;
    struct dirent* entry;

    while ((entry = vfs_readdir(node, index)) != (void*)0) {
        total_size += strlen(entry->name) + 1;  // +1 for newline
        index++;
    }

    if (total_size == 0) return "";

    char* result = malloc(total_size + 1);
    if (!result) return "";

    result[0] = '\0';
    index = 0;
    char* pos = result;

    while ((entry = vfs_readdir(node, index)) != (void*)0) {
        int len = strlen(entry->name);
        strcpy(pos, entry->name);
        pos += len;
        *pos++ = '\n';
        index++;
    }
    *pos = '\0';

    return result;
}

// ============================================================================
// Program Execution (placeholder - needs ELF loader)
// ============================================================================

int exec_program(const char* path, char** args) {
    // Check if file exists
    struct vfs_node* node = vfs_resolve_path(path);
    if (!node) {
        return -1;  // File not found
    }

    // TODO: Implement ELF loader
    // For now, just report that execution isn't implemented
    mt_print("exec: not implemented yet: ");
    mt_print(path);
    mt_print("\n");

    return -1;
}
