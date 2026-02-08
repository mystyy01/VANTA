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
    mt_print("  pwd         - print working directory\n");
    mt_print("  clear       - clear screen\n");
    mt_print("  exit        - exit shell\n");
    mt_print("External: echo, cat, touch, rm, rmdir\n");
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
extern int exec_program_fd(const char* path, char** args, void *fds);

// Pipe + process support (kernel-level calls from the shell)
extern int sched_spawn(const char *path, char **args, void *fd_overrides);
extern int sched_waitpid(int pid);
extern void *pipe_alloc(void);
extern int task_fd_alloc(void *t);
extern void *sched_current(void);

// ============================================================================
// Pipe execution: cmd1 | cmd2 | ...
// ============================================================================

// Check if an input line contains a pipe character
static int has_pipe(const char *input) {
    for (int i = 0; input[i]; i++) {
        if (input[i] == '|') return 1;
    }
    return 0;
}

// FD table structures (mirror kernel definitions for direct manipulation)
#define SHELL_MAX_FDS 64
#define SHELL_FD_UNUSED  0
#define SHELL_FD_FILE    1
#define SHELL_FD_CONSOLE 3
#define SHELL_FD_PIPE    4

struct shell_pipe {
    char buffer[512];
    int read_pos, write_pos, count;
    int read_open, write_open;
};

struct shell_fd_entry {
    int type;
    void *node;
    unsigned int offset;
    int flags;
    struct shell_pipe *pipe;
};

// ============================================================================
// Output redirection: cmd > file, cmd >> file
// ============================================================================

extern int fat32_touch_path(const char *path);
extern int fat32_truncate(struct vfs_node *node, int size);
extern int fat32_flush_size(struct vfs_node *node);

// Check for redirect operator in input.
// Returns: 0 = none, 1 = overwrite (>), 2 = append (>>)
// Sets *redir_pos to the index of the first '>' character.
static int find_redirect(const char *input, int *redir_pos) {
    for (int i = 0; input[i]; i++) {
        if (input[i] == '>') {
            *redir_pos = i;
            if (input[i + 1] == '>') return 2; // append
            return 1; // overwrite
        }
    }
    return 0;
}

// Build an absolute path from a possibly-relative filename
static void shell_build_path(const char *name, char *out) {
    if (name[0] == '/') {
        int i = 0;
        while (name[i] && i < 255) { out[i] = name[i]; i++; }
        out[i] = '\0';
    } else {
        char *cwd = get_cwd();
        int i = 0;
        while (cwd[i] && i < 200) { out[i] = cwd[i]; i++; }
        if (i > 0 && out[i - 1] != '/') out[i++] = '/';
        int j = 0;
        while (name[j] && i < 255) { out[i++] = name[j++]; }
        out[i] = '\0';
    }
}

// Execute a command with stdout redirected to a file.
// mode: 1 = overwrite (>), 2 = append (>>)
static int exec_redirect(const char *input, int redir_pos, int mode) {
    // Split into command part and filename part
    char cmd_part[256];
    char filename[256];

    // Copy command part (before >), trim trailing whitespace
    int cmd_len = redir_pos;
    while (cmd_len > 0 && (input[cmd_len - 1] == ' ' || input[cmd_len - 1] == '\t'))
        cmd_len--;
    for (int i = 0; i < cmd_len && i < 255; i++) cmd_part[i] = input[i];
    cmd_part[cmd_len] = '\0';

    // Extract filename (after > or >>), trim whitespace
    int fi = redir_pos + 1;
    if (mode == 2) fi++; // skip second '>'
    while (input[fi] == ' ' || input[fi] == '\t') fi++;
    int fn_len = 0;
    while (input[fi] && input[fi] != ' ' && input[fi] != '\t' && fn_len < 255) {
        filename[fn_len++] = input[fi++];
    }
    filename[fn_len] = '\0';

    if (fn_len == 0 || cmd_len == 0) {
        mt_print("syntax error near '>'\n");
        return -1;
    }

    // Resolve the output file path
    char full_file_path[256];
    shell_build_path(filename, full_file_path);

    // Ensure the file exists (create if needed)
    fat32_touch_path(full_file_path);

    struct vfs_node *file_node = vfs_resolve_path(full_file_path);
    if (!file_node) {
        mt_print("redirect: cannot open ");
        mt_print(filename);
        mt_print("\n");
        return -1;
    }

    // Truncate for overwrite mode
    if (mode == 1) {
        fat32_truncate(file_node, 0);
    }

    // Parse command part into cmd + args
    char r_cmd[64];
    char r_args[256];
    int ci = 0, ai = 0, pi = 0;

    while (cmd_part[pi] == ' ' || cmd_part[pi] == '\t') pi++;
    while (cmd_part[pi] && cmd_part[pi] != ' ' && cmd_part[pi] != '\t' && ci < 63)
        r_cmd[ci++] = cmd_part[pi++];
    r_cmd[ci] = '\0';

    while (cmd_part[pi] == ' ' || cmd_part[pi] == '\t') pi++;
    while (cmd_part[pi] && ai < 255)
        r_args[ai++] = cmd_part[pi++];
    r_args[ai] = '\0';

    // Build program path
    char path_buf[256];
    const char *path = r_cmd;
    if (r_cmd[0] != '/') {
        int i = 0;
        const char *prefix = "/apps/";
        while (prefix[i]) { path_buf[i] = prefix[i]; i++; }
        int j = 0;
        while (r_cmd[j] && i < 255) { path_buf[i++] = r_cmd[j++]; }
        path_buf[i] = '\0';
        path = path_buf;
    }

    // Build argv
    char *argv[3];
    argv[0] = r_cmd;
    argv[1] = (r_args[0] != '\0') ? r_args : 0;
    argv[2] = 0;

    // Build custom FD table with stdout -> file
    struct shell_fd_entry fds[SHELL_MAX_FDS];
    for (int f = 0; f < SHELL_MAX_FDS; f++) {
        fds[f].type = SHELL_FD_UNUSED;
        fds[f].node = 0;
        fds[f].offset = 0;
        fds[f].flags = 0;
        fds[f].pipe = 0;
    }

    // fd 0 = stdin (console)
    fds[0].type = SHELL_FD_CONSOLE;
    // fd 1 = stdout -> file
    fds[1].type = SHELL_FD_FILE;
    fds[1].node = file_node;
    fds[1].offset = (mode == 2) ? file_node->size : 0; // append: start at end
    fds[1].flags = 0;
    // fd 2 = stderr (console)
    fds[2].type = SHELL_FD_CONSOLE;

    int pid = sched_spawn(path, argv, (void *)fds);
    if (pid < 0) {
        mt_print("redirect: command not found: ");
        mt_print(r_cmd);
        mt_print("\n");
        return -1;
    }

    sched_waitpid(pid);

    // Flush the file size to disk
    fat32_flush_size(file_node);

    return 0;
}

// Execute a pipeline: "cmd1 | cmd2 | cmd3"
static int exec_pipeline(const char *input) {
    // Split input on '|' into segments
    char segments[4][128];  // max 4 commands in pipeline
    int seg_count = 0;
    int si = 0, di = 0;

    for (int i = 0; input[i] && seg_count < 4; i++) {
        if (input[i] == '|') {
            segments[seg_count][di] = '\0';
            seg_count++;
            di = 0;
            si = i + 1;
            // skip leading whitespace of next segment
            while (input[si] == ' ' || input[si] == '\t') si++;
            i = si - 1;
        } else {
            if (di < 127) segments[seg_count][di++] = input[i];
        }
    }
    segments[seg_count][di] = '\0';
    seg_count++;

    if (seg_count < 2) return -1; // not a real pipeline

    // Parse each segment into command + args
    char cmds[4][64];
    char argss[4][128];
    for (int i = 0; i < seg_count; i++) {
        int ci = 0, ai = 0, j = 0;
        // skip whitespace
        while (segments[i][j] == ' ' || segments[i][j] == '\t') j++;
        // command
        while (segments[i][j] && segments[i][j] != ' ' && segments[i][j] != '\t' && ci < 63) {
            cmds[i][ci++] = segments[i][j++];
        }
        cmds[i][ci] = '\0';
        // skip whitespace
        while (segments[i][j] == ' ' || segments[i][j] == '\t') j++;
        // args
        while (segments[i][j] && ai < 127) {
            argss[i][ai++] = segments[i][j++];
        }
        argss[i][ai] = '\0';
    }

    // Create pipes between consecutive commands
    // For N commands we need N-1 pipes
    struct shell_pipe *pipes[3]; // max 3 pipes for 4 commands
    int pipe_rfd[3], pipe_wfd[3]; // conceptual read/write FD indices
    (void)pipe_rfd; (void)pipe_wfd;

    for (int i = 0; i < seg_count - 1; i++) {
        pipes[i] = (struct shell_pipe *)pipe_alloc();
        if (!pipes[i]) {
            mt_print("pipe: allocation failed\n");
            return -1;
        }
    }

    // Spawn each command with appropriate FD redirections
    int pids[4];
    for (int i = 0; i < seg_count; i++) {
        // Build path
        char path_buf[256];
        const char *path = cmds[i];
        if (cmds[i][0] != '/') {
            int pi = 0;
            const char *prefix = "/apps/";
            while (prefix[pi]) { path_buf[pi] = prefix[pi]; pi++; }
            int pj = 0;
            while (cmds[i][pj] && pi < 255) { path_buf[pi++] = cmds[i][pj++]; }
            path_buf[pi] = '\0';
            path = path_buf;
        }

        // Build argv
        char *argv[3];
        argv[0] = cmds[i];
        argv[1] = (argss[i][0] != '\0') ? argss[i] : 0;
        argv[2] = 0;

        // Build custom FD table
        struct shell_fd_entry fds[SHELL_MAX_FDS];
        for (int f = 0; f < SHELL_MAX_FDS; f++) {
            fds[f].type = SHELL_FD_UNUSED;
            fds[f].node = 0;
            fds[f].offset = 0;
            fds[f].flags = 0;
            fds[f].pipe = 0;
        }

        // fd 0 = stdin
        if (i == 0) {
            // First command: stdin is console
            fds[0].type = SHELL_FD_CONSOLE;
        } else {
            // Read from previous pipe
            fds[0].type = SHELL_FD_PIPE;
            fds[0].pipe = pipes[i - 1];
            fds[0].flags = 0; // O_RDONLY
        }

        // fd 1 = stdout
        if (i == seg_count - 1) {
            // Last command: stdout is console
            fds[1].type = SHELL_FD_CONSOLE;
        } else {
            // Write to next pipe
            fds[1].type = SHELL_FD_PIPE;
            fds[1].pipe = pipes[i];
            fds[1].flags = 1; // O_WRONLY
        }

        // fd 2 = stderr (always console)
        fds[2].type = SHELL_FD_CONSOLE;

        pids[i] = sched_spawn(path, argv, (void *)fds);
        if (pids[i] < 0) {
            mt_print("pipe: failed to spawn: ");
            mt_print(cmds[i]);
            mt_print("\n");
        }
    }

    // Wait for all commands to finish
    for (int i = 0; i < seg_count; i++) {
        if (pids[i] > 0) {
            sched_waitpid(pids[i]);
        }
    }

    return 0;
}

// ============================================================================
// Main shell
// ============================================================================

static char input_buffer[512];

// Cursor blink interval in PIT ticks (~18.2 ticks/sec, so 9 ≈ 0.5 sec)
#define CURSOR_BLINK_TICKS 9

// Simple line editor with cursor tracking, prompt-aware redraw, and blinking cursor.
static char* shell_read_line(void) {
    // Capture prompt end position
    int prompt_row, prompt_col;
    cursor_get(&prompt_row, &prompt_col);

    int len = 0;          // line length
    int pos = 0;          // cursor position within line
    int rendered_len = 0; // previously drawn length

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

    // Draw cursor at current position using color inversion
    void draw_cursor(int visible) {
        int abs_pos = prompt_col + pos;
        int row = prompt_row + (abs_pos / VGA_WIDTH);
        int col = abs_pos % VGA_WIDTH;
        volatile uint16_t *cell = VGA_BUFFER + (row * VGA_WIDTH) + col;
        char ch = (pos < len) ? input_buffer[pos] : ' ';

        if (visible) {
            // Inverted colors: white background (0x70), black text
            *cell = (0x70 << 8) | (uint8_t)ch;
        } else {
            // Normal colors: black background (0x00), white text (0x0F)
            *cell = (0x0F << 8) | (uint8_t)ch;
        }
        set_cursor(row, col);
    }

    // Initial draw
    draw_cursor(1);

    while (1) {
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
            continue;  // Unknown key
        }

        // Redraw cursor after input
        draw_cursor(1);
    }
}

int shell_main(void) {
    cmd_clear();
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
        
        // Check for output redirection before pipeline/builtins
        {
            int redir_pos = 0;
            int redir_mode = find_redirect(input, &redir_pos);
            if (redir_mode > 0) {
                exec_redirect(input, redir_pos, redir_mode);
                continue;
            }
        }

        // Check for pipeline before built-in command dispatch
        if (has_pipe(input)) {
            exec_pipeline(input);
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
