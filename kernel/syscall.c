#include "syscall.h"
#include "fs/fat32.h"
#include "fs/vfs.h"
#include "elf_loader.h"

// ============================================================================
// Console I/O
// ============================================================================

static volatile unsigned short *video = (volatile unsigned short *)0xB8000;
static int cursor_row = 10;
static int cursor_col = 0;

static void console_putchar(char c) {
    if (c == '\n') {
        cursor_row++;
        cursor_col = 0;
    } else if (c == '\r') {
        cursor_col = 0;
    } else if (c == '\b') {
        if (cursor_col > 0) {
            cursor_col--;
            video[cursor_row * 80 + cursor_col] = (0x0F << 8) | ' ';
        }
    } else {
        video[cursor_row * 80 + cursor_col] = (0x0F << 8) | c;
        cursor_col++;
        if (cursor_col >= 80) {
            cursor_col = 0;
            cursor_row++;
        }
    }
    // Simple scroll - just wrap for now
    if (cursor_row >= 25) {
        cursor_row = 10;
    }
}

static int console_write(const char *buf, int count) {
    for (int i = 0; i < count && buf[i]; i++) {
        console_putchar(buf[i]);
    }
    return count;
}

// ============================================================================
// File Descriptor Table
// ============================================================================

#define MAX_FDS 64
#define FD_UNUSED   0
#define FD_FILE     1
#define FD_DIR      2
#define FD_CONSOLE  3

struct fd_entry {
    int type;                   // FD_UNUSED, FD_FILE, FD_DIR, FD_CONSOLE
    struct vfs_node *node;      // VFS node (for files/dirs)
    uint32_t offset;            // Current read/write position
    int flags;                  // Open flags
};

static struct fd_entry fd_table[MAX_FDS];
static int fd_initialized = 0;

static void fd_init(void) {
    if (fd_initialized) return;

    // Clear all entries
    for (int i = 0; i < MAX_FDS; i++) {
        fd_table[i].type = FD_UNUSED;
        fd_table[i].node = 0;
        fd_table[i].offset = 0;
        fd_table[i].flags = 0;
    }

    // Set up standard file descriptors
    fd_table[STDIN_FD].type = FD_CONSOLE;   // stdin
    fd_table[STDOUT_FD].type = FD_CONSOLE;  // stdout
    fd_table[STDERR_FD].type = FD_CONSOLE;  // stderr

    fd_initialized = 1;
}

static int fd_alloc(void) {
    fd_init();
    // Start from 3 (skip stdin/stdout/stderr)
    for (int i = 3; i < MAX_FDS; i++) {
        if (fd_table[i].type == FD_UNUSED) {
            return i;
        }
    }
    return -1;  // No free fd
}

static void fd_free(int fd) {
    if (fd >= 3 && fd < MAX_FDS) {
        fd_table[fd].type = FD_UNUSED;
        fd_table[fd].node = 0;
        fd_table[fd].offset = 0;
        fd_table[fd].flags = 0;
    }
}

static struct fd_entry *fd_get(int fd) {
    fd_init();
    if (fd < 0 || fd >= MAX_FDS) return 0;
    if (fd_table[fd].type == FD_UNUSED) return 0;
    return &fd_table[fd];
}

// ============================================================================
// Current Working Directory (per-process, but we only have one process)
// ============================================================================

static char current_dir[VFS_MAX_PATH] = "/";

// ============================================================================
// MSR helpers
// ============================================================================

static inline void wrmsr(uint32_t msr, uint64_t value) {
    uint32_t low = value & 0xFFFFFFFF;
    uint32_t high = value >> 32;
    __asm__ volatile ("wrmsr" : : "c"(msr), "a"(low), "d"(high));
}

static inline uint64_t rdmsr(uint32_t msr) {
    uint32_t low, high;
    __asm__ volatile ("rdmsr" : "=a"(low), "=d"(high) : "c"(msr));
    return ((uint64_t)high << 32) | low;
}

// ============================================================================
// String helpers
// ============================================================================

static int str_len(const char *s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static void str_copy(char *dst, const char *src, int max) {
    int i = 0;
    while (src[i] && i < max - 1) {
        dst[i] = src[i];
        i++;
    }
    dst[i] = '\0';
}

// Build absolute path from current_dir and relative path
static void build_path(const char *path, char *out) {
    if (path[0] == '/') {
        // Absolute path
        str_copy(out, path, VFS_MAX_PATH);
    } else {
        // Relative path
        int cwd_len = str_len(current_dir);
        str_copy(out, current_dir, VFS_MAX_PATH);

        // Add separator if needed
        if (cwd_len > 0 && current_dir[cwd_len-1] != '/') {
            out[cwd_len] = '/';
            out[cwd_len + 1] = '\0';
            cwd_len++;
        }

        // Append relative path
        int i = 0;
        while (path[i] && cwd_len + i < VFS_MAX_PATH - 1) {
            out[cwd_len + i] = path[i];
            i++;
        }
        out[cwd_len + i] = '\0';
    }
}

// ============================================================================
// Syscall initialization
// ============================================================================

extern void syscall_entry(void);

void syscall_init(void) {
    fd_init();

    // Enable System Call Extensions in EFER
    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);

    // Set up STAR register (segment selectors)
    // STAR layout (64-bit mode):
    //   bits 47:32 - kernel CS selector (SS = CS + 8)
    //   bits 63:48 - user   CS selector base; SYSRET sets CS = base+16, SS = base+8, RPL=3
    // GDT layout from bootloader:
    //   0x08 = kernel code, 0x10 = kernel data, 0x18 = user data, 0x20 = user code
    // To land on user CS=0x20 and SS=0x18 after SYSRET, program STAR high with 0x0010.
    uint64_t star = ((uint64_t)0x0008 << 32) | ((uint64_t)0x0010 << 48);
    wrmsr(MSR_STAR, star);

    // Set LSTAR to our syscall entry point
    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);

    // Set FMASK - clear interrupt flag during syscall
    wrmsr(MSR_FMASK, 0x200);
}

// ============================================================================
// Syscall handler
// ============================================================================

uint64_t syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    (void)arg4; (void)arg5;  // Unused for now

    switch (num) {

        // --------------------------------------------------------------------
        // SYS_EXIT: exit(int code)
        // --------------------------------------------------------------------
        case SYS_EXIT: {
            int exit_code = (int)arg1;
            kernel_return_from_user(exit_code);
            __builtin_unreachable();
            return 0;
        }

        // --------------------------------------------------------------------
        // SYS_READ: read(int fd, char *buf, int count) -> bytes read
        // --------------------------------------------------------------------
        case SYS_READ: {
            int fd = (int)arg1;
            char *buf = (char *)arg2;
            int count = (int)arg3;

            struct fd_entry *entry = fd_get(fd);
            if (!entry) return -1;

            if (entry->type == FD_CONSOLE) {
                // TODO: Read from keyboard
                return 0;
            }

            if (entry->type == FD_FILE && entry->node) {
                int bytes = vfs_read(entry->node, entry->offset, count, (uint8_t *)buf);
                if (bytes > 0) {
                    entry->offset += bytes;
                }
                return bytes;
            }

            return -1;
        }

        // --------------------------------------------------------------------
        // SYS_WRITE: write(int fd, char *buf, int count) -> bytes written
        // --------------------------------------------------------------------
        case SYS_WRITE: {
            int fd = (int)arg1;
            const char *buf = (const char *)arg2;
            int count = (int)arg3;

            struct fd_entry *entry = fd_get(fd);
            if (!entry) return -1;

            if (entry->type == FD_CONSOLE) {
                return console_write(buf, count);
            }

            if (entry->type == FD_FILE && entry->node) {
                int bytes = vfs_write(entry->node, entry->offset, count, (const uint8_t *)buf);
                if (bytes > 0) {
                    entry->offset += bytes;
                }
                return bytes;
            }

            return -1;
        }

        // --------------------------------------------------------------------
        // SYS_OPEN: open(char *path, int flags) -> fd
        // --------------------------------------------------------------------
        case SYS_OPEN: {
            const char *path = (const char *)arg1;
            int flags = (int)arg2;

            char full_path[VFS_MAX_PATH];
            build_path(path, full_path);

            struct vfs_node *node = vfs_resolve_path(full_path);

            // Handle O_CREAT
            if (!node && (flags & O_CREAT)) {
                // Parse path to get parent and name
                char parent_path[VFS_MAX_PATH], name[VFS_MAX_PATH];
                int len = str_len(full_path);
                int last_slash = len - 1;
                while (last_slash > 0 && full_path[last_slash] != '/') last_slash--;

                if (last_slash == 0) {
                    parent_path[0] = '/';
                    parent_path[1] = '\0';
                    str_copy(name, full_path + 1, VFS_MAX_PATH);
                } else {
                    str_copy(parent_path, full_path, last_slash + 1);
                    parent_path[last_slash] = '\0';
                    str_copy(name, full_path + last_slash + 1, VFS_MAX_PATH);
                }

                struct vfs_node *parent = vfs_resolve_path(parent_path);
                if (parent) {
                    node = fat32_create_file(parent, name);
                }
            }

            if (!node) return -1;

            int fd = fd_alloc();
            if (fd < 0) return -1;

            fd_table[fd].node = node;
            fd_table[fd].offset = 0;
            fd_table[fd].flags = flags;

            if (node->flags & VFS_DIRECTORY) {
                fd_table[fd].type = FD_DIR;
            } else {
                fd_table[fd].type = FD_FILE;
            }

            // Handle O_TRUNC
            if ((flags & O_TRUNC) && (fd_table[fd].type == FD_FILE)) {
                // TODO: Truncate file
            }

            // Handle O_APPEND
            if ((flags & O_APPEND) && (fd_table[fd].type == FD_FILE)) {
                fd_table[fd].offset = node->size;
            }

            return fd;
        }

        // --------------------------------------------------------------------
        // SYS_CLOSE: close(int fd) -> 0 or -1
        // --------------------------------------------------------------------
        case SYS_CLOSE: {
            int fd = (int)arg1;
            struct fd_entry *entry = fd_get(fd);
            if (!entry || fd < 3) return -1;  // Can't close stdin/stdout/stderr

            fd_free(fd);
            return 0;
        }

        // --------------------------------------------------------------------
        // SYS_STAT: stat(char *path, struct stat *buf) -> 0 or -1
        // --------------------------------------------------------------------
        case SYS_STAT: {
            const char *path = (const char *)arg1;
            struct stat *buf = (struct stat *)arg2;

            char full_path[VFS_MAX_PATH];
            build_path(path, full_path);

            struct vfs_node *node = vfs_resolve_path(full_path);
            if (!node) return -1;

            buf->st_size = node->size;
            buf->st_ino = node->inode;
            buf->st_mode = (node->flags & VFS_DIRECTORY) ? S_IFDIR : S_IFREG;

            return 0;
        }

        // --------------------------------------------------------------------
        // SYS_FSTAT: fstat(int fd, struct stat *buf) -> 0 or -1
        // --------------------------------------------------------------------
        case SYS_FSTAT: {
            int fd = (int)arg1;
            struct stat *buf = (struct stat *)arg2;

            struct fd_entry *entry = fd_get(fd);
            if (!entry || !entry->node) return -1;

            buf->st_size = entry->node->size;
            buf->st_ino = entry->node->inode;
            buf->st_mode = (entry->node->flags & VFS_DIRECTORY) ? S_IFDIR : S_IFREG;

            return 0;
        }

        // --------------------------------------------------------------------
        // SYS_MKDIR: mkdir(char *path) -> 0 or -1
        // --------------------------------------------------------------------
        case SYS_MKDIR: {
            const char *path = (const char *)arg1;

            char full_path[VFS_MAX_PATH];
            build_path(path, full_path);

            struct vfs_node *result = ensure_path_exists(full_path);
            return result ? 0 : -1;
        }

        // --------------------------------------------------------------------
        // SYS_RMDIR: rmdir(char *path) -> 0 or -1
        // --------------------------------------------------------------------
        case SYS_RMDIR: {
            const char *path = (const char *)arg1;

            char full_path[VFS_MAX_PATH];
            build_path(path, full_path);

            // Find parent directory and name
            char parent_path[VFS_MAX_PATH];
            char name[VFS_MAX_PATH];

            // Extract parent path and name
            int len = str_len(full_path);
            int last_slash = len - 1;
            while (last_slash > 0 && full_path[last_slash] != '/') last_slash--;

            if (last_slash == 0) {
                parent_path[0] = '/';
                parent_path[1] = '\0';
                str_copy(name, full_path + 1, VFS_MAX_PATH);
            } else {
                str_copy(parent_path, full_path, last_slash + 1);
                parent_path[last_slash] = '\0';
                str_copy(name, full_path + last_slash + 1, VFS_MAX_PATH);
            }

            struct vfs_node *parent = vfs_resolve_path(parent_path);
            if (!parent) return -1;

            return fat32_rmdir(parent, name);
        }

        // --------------------------------------------------------------------
        // SYS_UNLINK: unlink(char *path) -> 0 or -1
        // --------------------------------------------------------------------
        case SYS_UNLINK: {
            const char *path = (const char *)arg1;

            char full_path[VFS_MAX_PATH];
            build_path(path, full_path);

            // Find parent directory and name
            char parent_path[VFS_MAX_PATH];
            char name[VFS_MAX_PATH];

            int len = str_len(full_path);
            int last_slash = len - 1;
            while (last_slash > 0 && full_path[last_slash] != '/') last_slash--;

            if (last_slash == 0) {
                parent_path[0] = '/';
                parent_path[1] = '\0';
                str_copy(name, full_path + 1, VFS_MAX_PATH);
            } else {
                str_copy(parent_path, full_path, last_slash + 1);
                parent_path[last_slash] = '\0';
                str_copy(name, full_path + last_slash + 1, VFS_MAX_PATH);
            }

            struct vfs_node *parent = vfs_resolve_path(parent_path);
            if (!parent) return -1;

            return fat32_unlink(parent, name);
        }

        // --------------------------------------------------------------------
        // SYS_READDIR: readdir(int fd, struct user_dirent *buf, int index) -> 0 or -1
        // --------------------------------------------------------------------
        case SYS_READDIR: {
            int fd = (int)arg1;
            struct user_dirent *buf = (struct user_dirent *)arg2;
            int index = (int)arg3;

            struct fd_entry *entry = fd_get(fd);
            if (!entry || entry->type != FD_DIR || !entry->node) return -1;

            struct dirent *dent = vfs_readdir(entry->node, index);
            if (!dent) return -1;

            str_copy(buf->name, dent->name, 256);

            // Check if it's a directory by trying to find it
            struct vfs_node *child = vfs_finddir(entry->node, dent->name);
            buf->type = (child && (child->flags & VFS_DIRECTORY)) ? 1 : 0;

            return 0;
        }

        // --------------------------------------------------------------------
        // SYS_CHDIR: chdir(char *path) -> 0 or -1
        // --------------------------------------------------------------------
        case SYS_CHDIR: {
            const char *path = (const char *)arg1;

            char full_path[VFS_MAX_PATH];
            build_path(path, full_path);

            struct vfs_node *node = vfs_resolve_path(full_path);
            if (!node) return -1;
            if (!(node->flags & VFS_DIRECTORY)) return -1;

            str_copy(current_dir, full_path, VFS_MAX_PATH);
            return 0;
        }

        // --------------------------------------------------------------------
        // SYS_GETCWD: getcwd(char *buf, int size) -> length or -1
        // --------------------------------------------------------------------
        case SYS_GETCWD: {
            char *buf = (char *)arg1;
            int size = (int)arg2;

            int len = str_len(current_dir);
            if (len >= size) return -1;

            str_copy(buf, current_dir, size);
            return len;
        }

        // --------------------------------------------------------------------
        // SYS_RENAME: rename(char *old, char *new) -> 0 or -1
        // --------------------------------------------------------------------
        case SYS_RENAME: {
            const char *old_path = (const char *)arg1;
            const char *new_path = (const char *)arg2;

            char old_full[VFS_MAX_PATH], new_full[VFS_MAX_PATH];
            build_path(old_path, old_full);
            build_path(new_path, new_full);

            // Parse old path
            char old_parent_path[VFS_MAX_PATH], old_name[VFS_MAX_PATH];
            int len = str_len(old_full);
            int last_slash = len - 1;
            while (last_slash > 0 && old_full[last_slash] != '/') last_slash--;

            if (last_slash == 0) {
                old_parent_path[0] = '/';
                old_parent_path[1] = '\0';
                str_copy(old_name, old_full + 1, VFS_MAX_PATH);
            } else {
                str_copy(old_parent_path, old_full, last_slash + 1);
                old_parent_path[last_slash] = '\0';
                str_copy(old_name, old_full + last_slash + 1, VFS_MAX_PATH);
            }

            // Parse new path
            char new_parent_path[VFS_MAX_PATH], new_name[VFS_MAX_PATH];
            len = str_len(new_full);
            last_slash = len - 1;
            while (last_slash > 0 && new_full[last_slash] != '/') last_slash--;

            if (last_slash == 0) {
                new_parent_path[0] = '/';
                new_parent_path[1] = '\0';
                str_copy(new_name, new_full + 1, VFS_MAX_PATH);
            } else {
                str_copy(new_parent_path, new_full, last_slash + 1);
                new_parent_path[last_slash] = '\0';
                str_copy(new_name, new_full + last_slash + 1, VFS_MAX_PATH);
            }

            struct vfs_node *old_parent = vfs_resolve_path(old_parent_path);
            struct vfs_node *new_parent = vfs_resolve_path(new_parent_path);

            if (!old_parent || !new_parent) return -1;

            return fat32_rename(old_parent, old_name, new_parent, new_name);
        }

        // --------------------------------------------------------------------
        // SYS_TRUNCATE: truncate(char *path, int size) -> 0 or -1
        // --------------------------------------------------------------------
        case SYS_TRUNCATE: {
            const char *path = (const char *)arg1;
            int size = (int)arg2;

            char full_path[VFS_MAX_PATH];
            build_path(path, full_path);

            struct vfs_node *node = vfs_resolve_path(full_path);
            if (!node) return -1;

            return fat32_truncate(node, size);
        }

        // --------------------------------------------------------------------
        // SYS_CREATE: create(char *path) -> fd or -1
        // --------------------------------------------------------------------
        case SYS_CREATE: {
            const char *path = (const char *)arg1;

            char full_path[VFS_MAX_PATH];
            build_path(path, full_path);

            // Parse path to get parent and name
            char parent_path[VFS_MAX_PATH], name[VFS_MAX_PATH];
            int len = str_len(full_path);
            int last_slash = len - 1;
            while (last_slash > 0 && full_path[last_slash] != '/') last_slash--;

            if (last_slash == 0) {
                parent_path[0] = '/';
                parent_path[1] = '\0';
                str_copy(name, full_path + 1, VFS_MAX_PATH);
            } else {
                str_copy(parent_path, full_path, last_slash + 1);
                parent_path[last_slash] = '\0';
                str_copy(name, full_path + last_slash + 1, VFS_MAX_PATH);
            }

            struct vfs_node *parent = vfs_resolve_path(parent_path);
            if (!parent) return -1;

            struct vfs_node *node = fat32_create_file(parent, name);
            if (!node) return -1;

            // Open the newly created file
            int fd = fd_alloc();
            if (fd < 0) return -1;

            fd_table[fd].type = FD_FILE;
            fd_table[fd].node = node;
            fd_table[fd].offset = 0;
            fd_table[fd].flags = O_RDWR;

            return fd;
        }

        // --------------------------------------------------------------------
        // SYS_SEEK: seek(int fd, int offset, int whence) -> new offset or -1
        // --------------------------------------------------------------------
        case SYS_SEEK: {
            int fd = (int)arg1;
            int offset = (int)arg2;
            int whence = (int)arg3;

            struct fd_entry *entry = fd_get(fd);
            if (!entry || entry->type != FD_FILE || !entry->node) return -1;

            uint32_t new_offset;
            switch (whence) {
                case SEEK_SET:
                    new_offset = offset;
                    break;
                case SEEK_CUR:
                    new_offset = entry->offset + offset;
                    break;
                case SEEK_END:
                    new_offset = entry->node->size + offset;
                    break;
                default:
                    return -1;
            }

            // Don't allow seeking past end
            if (new_offset > entry->node->size) {
                new_offset = entry->node->size;
            }

            entry->offset = new_offset;
            return new_offset;
        }

        // --------------------------------------------------------------------
        // Unknown syscall
        // --------------------------------------------------------------------
        default:
            return -1;
    }
}
