// PHOBOS Shell - Pure C implementation
// Replaces mt-lang shell due to memory issues

#include <stdint.h>
#include "../kernel/drivers/keyboard.h"
#include "../kernel/fs/vfs.h"

// ============================================================================
// External functions from lib.c
// ============================================================================

extern void mt_print(const char* s);
extern void print_char(int c);
extern void print_int(int n);
extern char* get_cwd(void);
extern int set_cwd(const char* path);
extern char* list_dir(const char* path);
extern char* read_file(const char* path);
extern void cursor_get(int *row, int *col);
extern void set_cursor(int row, int col);

// VGA text-mode access for cursor rendering
#define VGA_WIDTH 80
#define VGA_BUFFER ((volatile uint16_t*)0xB8000)

// Disable the hardware text-mode cursor (avoids double-blink with software caret)
static inline void outb(uint16_t port, uint8_t val) {
    __asm__ volatile ("outb %0, %1" : : "a"(val), "Nd"(port));
}

static void disable_hw_cursor(void) {
    outb(0x3D4, 0x0A);
    outb(0x3D5, 0x20);  // bit 5 = disable
}



// ============================================================================
// String utilities
// ============================================================================

static int str_len(const char* s) {
    if (!s) return 0;
    int len = 0;
    while (s[len]) len++;
    return len;
}

static int str_eq(const char* a, const char* b) {
    if (!a || !b) return 0;
    while (*a && *b) {
        if (*a != *b) return 0;
        a++;
        b++;
    }
    return *a == *b;
}

static int str_starts_with(const char* str, const char* prefix) {
    if (!str || !prefix) return 0;
    while (*prefix) {
        if (*str != *prefix) return 0;
        str++;
        prefix++;
    }
    return 1;
}

// ============================================================================
// Command parsing
// ============================================================================

static char cmd_buf[64];
static char args_buf[256];

static void parse_input(const char* input) {
    int i = 0;
    int cmd_i = 0;
    int args_i = 0;
    
    // Skip leading whitespace
    while (input[i] == ' ' || input[i] == '\t') i++;
    
    // Get command (first word)
    while (input[i] && input[i] != ' ' && input[i] != '\t' && cmd_i < 63) {
        cmd_buf[cmd_i++] = input[i++];
    }
    cmd_buf[cmd_i] = '\0';
    
    // Skip whitespace between command and args
    while (input[i] == ' ' || input[i] == '\t') i++;
    
    // Get rest as arguments
    while (input[i] && args_i < 255) {
        args_buf[args_i++] = input[i++];
    }
    args_buf[args_i] = '\0';
}

// ============================================================================
// Built-in commands
// ============================================================================

static int cmd_help(void) {
    mt_print("phobos-shell builtins:\n");
    mt_print("  help        - show this help\n");
    mt_print("  ls [path]   - list directory\n");
    mt_print("  cd <path>   - change directory\n");
    mt_print("  mkdir <dir> - create directory\n");
    mt_print("  cat <file>  - print file contents\n");
    mt_print("  pwd         - print working directory\n");
    mt_print("  echo <...>  - print arguments\n");
    mt_print("  clear       - clear screen\n");
    mt_print("  exit        - exit shell\n");
    return 0;
}

static int cmd_pwd(void) {
    char* cwd = get_cwd();
    mt_print(cwd);
    mt_print("\n");
    return 0;
}

static int cmd_ls(const char* path) {
    const char* target = path;
    if (!path || path[0] == '\0') {
        target = get_cwd();
    }
    char* entries = list_dir(target);
    if (entries && entries[0]) {
        mt_print(entries);
    }
    return 0;
}

static int cmd_cd(const char* path) {
    const char* target = path;
    if (!path || path[0] == '\0') {
        target = "/";
    }
    int result = set_cwd(target);
    if (result != 0) {
        mt_print("cd: no such directory: ");
        mt_print(target);
        mt_print("\n");
    }
    return result;
}

static int cmd_cat(const char* path) {
    if (!path || path[0] == '\0') {
        mt_print("cat: missing file argument\n");
        return 1;
    }
    char* content = read_file(path);
    if (content) {
        mt_print(content);
    }
    return 0;
}

static int cmd_echo(const char* text) {
    if (text) {
        mt_print(text);
    }
    mt_print("\n");
    return 0;
}

extern void clear_screen(void);
extern void set_cursor(int row, int col);
extern struct vfs_node *ensure_path_exists(const char *path);
extern volatile uint64_t system_ticks;

static int cmd_clear(void) {
    clear_screen();
    return 0;
}

static int cmd_mkdir(const char* path) {
    if (!path || path[0] == '\0') {
        mt_print("mkdir: missing directory argument\n");
        return 1;
    }
    
    // Build full path if relative
    char full_path[256];
    if (path[0] == '/') {
        // Absolute path
        int i = 0;
        while (path[i] && i < 255) {
            full_path[i] = path[i];
            i++;
        }
        full_path[i] = '\0';
    } else {
        // Relative path - prepend cwd
        char* cwd = get_cwd();
        int i = 0;
        while (cwd[i] && i < 200) {
            full_path[i] = cwd[i];
            i++;
        }
        // Add separator if needed
        if (i > 0 && full_path[i-1] != '/') {
            full_path[i++] = '/';
        }
        int j = 0;
        while (path[j] && i < 255) {
            full_path[i++] = path[j++];
        }
        full_path[i] = '\0';
    }
    
    struct vfs_node *result = ensure_path_exists(full_path);
    if (!result) {
        mt_print("mkdir: failed to create directory: ");
        mt_print(path);
        mt_print("\n");
        return 1;
    }
    return 0;
}

// External program execution
extern int exec_program(const char* path, char** args);

// ============================================================================
// Main shell
// ============================================================================

static char input_buffer[512];

// Cursor blink interval in PIT ticks (~18.2 ticks/sec, so 4 ≈ 0.22 sec)
#define CURSOR_BLINK_TICKS 4

// Simple line editor with cursor tracking, prompt-aware redraw, and blinking cursor.
static char* shell_read_line(void) {
    // Capture prompt end position
    int prompt_row, prompt_col;
    cursor_get(&prompt_row, &prompt_col);

    int len = 0;          // line length
    int pos = 0;          // cursor position within line
    int rendered_len = 0; // previously drawn length
    int cursor_visible = 1;
    uint64_t last_blink_tick = system_ticks;

    // Redraw line content (not cursor)
    void redraw_line(void) {
        set_cursor(prompt_row, prompt_col);
        for (int i = 0; i < len; i++) {
            print_char(input_buffer[i]);
        }
        for (int i = len; i < rendered_len; i++) {
            print_char(' ');
        }
        rendered_len = len;
    }

    // Draw cursor at current position
    void draw_cursor(int visible) {
        int abs_pos = prompt_col + pos;
        int row = prompt_row + (abs_pos / VGA_WIDTH);
        int col = abs_pos % VGA_WIDTH;
        volatile uint16_t *cell = VGA_BUFFER + (row * VGA_WIDTH) + col;
        uint16_t color = *cell & 0xFF00;
        char ch = (pos < len) ? input_buffer[pos] : ' ';

        *cell = color | (uint8_t)(visible ? '_' : ch);
        set_cursor(row, col);
    }

    // Initial draw
    draw_cursor(cursor_visible);

    while (1) {
        // Check for cursor blink
        uint64_t now = system_ticks;
        if (now - last_blink_tick >= CURSOR_BLINK_TICKS) {
            cursor_visible = !cursor_visible;
            draw_cursor(cursor_visible);
            last_blink_tick = now;
        }

        // Poll for keyboard event (non-blocking)
        struct key_event ev;
        if (!keyboard_poll_event(&ev)) {
            __asm__ volatile ("hlt");  // Wait for next interrupt
            continue;
        }
        if (!ev.pressed) continue;

        // Ctrl+C cancels line
        if ((ev.modifiers & MOD_CTRL) && (ev.key == 'c' || ev.key == 'C')) {
            draw_cursor(0);  // Hide cursor
            mt_print("^C\n");
            input_buffer[0] = '\0';
            return input_buffer;
        }

        if (ev.key == '\n') {
            // Accept line: hide cursor, move to end, newline
            draw_cursor(0);
            int end_abs = prompt_col + len;
            set_cursor(prompt_row + end_abs / VGA_WIDTH, end_abs % VGA_WIDTH);
            print_char('\n');
            input_buffer[len] = '\0';
            return input_buffer;
        }

        if (ev.key == '\b') {
            if (pos > 0) {
                draw_cursor(0);  // Hide before modifying
                for (int i = pos - 1; i < len - 1; i++) {
                    input_buffer[i] = input_buffer[i + 1];
                }
                len--;
                pos--;
                redraw_line();
            }
        } else if (ev.key == KEY_LEFT) {
            if (pos > 0) {
                draw_cursor(0);
                pos--;
            }
        } else if (ev.key == KEY_RIGHT) {
            if (pos < len) {
                draw_cursor(0);
                pos++;
            }
        } else if (ev.key >= 0x20 && ev.key < 0x7F) { // printable
            if (len < 510) {
                draw_cursor(0);  // Hide before modifying
                for (int i = len; i > pos; i--) {
                    input_buffer[i] = input_buffer[i - 1];
                }
                input_buffer[pos] = ev.key;
                len++;
                pos++;
                redraw_line();
            }
        } else {
            continue;  // Unknown key, don't reset blink
        }

        // Reset blink state on any input (cursor visible)
        cursor_visible = 1;
        last_blink_tick = system_ticks;
        draw_cursor(cursor_visible);
    }
}

int shell_main(void) {
    // cmd_clear();
    disable_hw_cursor();
    mt_print("phobos-shell v0.2 - PHOBOS\n");
    mt_print("Type 'help' for available commands\n\n");
    
    while (1) {
        // Print prompt
        char* cwd = get_cwd();
        mt_print(cwd);
        mt_print(" $ ");
        
        // Read input
        char* input = shell_read_line();
        
        // Skip empty input
        if (!input || input[0] == '\0') {
            continue;
        }
        
        // Parse into command and arguments
        parse_input(input);
        
        // Empty command (just whitespace)
        if (cmd_buf[0] == '\0') {
            continue;
        }
        
        // Handle built-in commands
        if (str_eq(cmd_buf, "exit")) {
            break;
        } else if (str_eq(cmd_buf, "help")) {
            cmd_help();
        } else if (str_eq(cmd_buf, "pwd")) {
            cmd_pwd();
        } else if (str_eq(cmd_buf, "ls")) {
            cmd_ls(args_buf);
        } else if (str_eq(cmd_buf, "cd")) {
            cmd_cd(args_buf);
        } else if (str_eq(cmd_buf, "cat")) {
            cmd_cat(args_buf);
        } else if (str_eq(cmd_buf, "echo")) {
            cmd_echo(args_buf);
        } else if (str_eq(cmd_buf, "clear")) {
            cmd_clear();
        } else {
            // Try to execute external program from /apps/<cmd>
            char path_buf[256];
            const char *path = cmd_buf;
            if (cmd_buf[0] != '/') {
                // default lookup in /apps
                int i = 0;
                const char *prefix = "/apps/";
                while (prefix[i]) { path_buf[i] = prefix[i]; i++; }
                int j = 0;
                while (cmd_buf[j] && i < 255) { path_buf[i++] = cmd_buf[j++]; }
                path_buf[i] = '\0';
                path = path_buf;
            }

            // Build argv: prog, optional single arg string, NULL
            char *argv[3];
            argv[0] = (char *)cmd_buf;
            argv[1] = (args_buf && args_buf[0] != '\0') ? (char *)args_buf : 0;
            argv[2] = 0;

            int r = exec_program(path, argv);
            if (r == -1) {
                mt_print("phobos-shell: command not found: ");
                mt_print(cmd_buf);
                mt_print("\n");
            } else if (r != 0) {} // let the command return an error rather than the shell
        }
    }
    
    mt_print("Goodbye!\n");
    return 0;
}
