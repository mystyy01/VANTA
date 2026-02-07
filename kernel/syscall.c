#include "syscall.h"
#include "fs/fat32.h"
#include "fs/vfs.h"
#include "elf_loader.h"
#include "sched.h"

// ============================================================================
// Console I/O (reuse mt-shell VGA writer so cursor stays consistent)
// ============================================================================

extern void print_char(int c); // defined in mt-shell/lib.c

static int console_write(const char *buf, int count) {
    for (int i = 0; i < count && buf[i]; i++) {
        print_char(buf[i]);
    }
    return count;
}

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

static void build_path(const char *path, char *out) {
    struct task *t = sched_current();
    if (path[0] == '/') {
        str_copy(out, path, VFS_MAX_PATH);
    } else {
        int cwd_len = str_len(t->cwd);
        str_copy(out, t->cwd, VFS_MAX_PATH);

        if (cwd_len > 0 && t->cwd[cwd_len-1] != '/') {
            out[cwd_len] = '/';
            out[cwd_len + 1] = '\0';
            cwd_len++;
        }

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

    uint64_t efer = rdmsr(MSR_EFER);
    efer |= EFER_SCE;
    wrmsr(MSR_EFER, efer);

    // STAR: kernel CS = 0x08, user CS base = 0x10 (so SYSRET gives CS=0x20|3, SS=0x18|3)
    uint64_t star = ((uint64_t)0x0008 << 32) | ((uint64_t)0x0010 << 48);
    wrmsr(MSR_STAR, star);

    wrmsr(MSR_LSTAR, (uint64_t)syscall_entry);
    wrmsr(MSR_FMASK, 0x200);
}

// ============================================================================
// Syscall handler
// ============================================================================

volatile int in_syscall = 0;

uint64_t syscall_handler(uint64_t num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5) {
    (void)arg4; (void)arg5;

    switch (num) {

        case SYS_EXIT: {
            int exit_code = (int)arg1;
            struct task *t = sched_current();
            if (t && t->is_user) {
                sched_exit(exit_code);
                __builtin_unreachable();
            }
            kernel_return_from_user(exit_code);
            __builtin_unreachable();
            return 0;
        }

        case SYS_READ: {
            int fd = (int)arg1;
            char *buf = (char *)arg2;
            int count = (int)arg3;

            struct task *t = sched_current();
            struct fd_entry *entry = task_fd_get(t, fd);
            if (!entry) return -1;

            if (entry->type == FD_CONSOLE) {
                return 0;  
            }

            if (entry->type == FD_FILE && entry->node) {
                int bytes = vfs_read(entry->node, entry->offset, count, (uint8_t *)buf);
                if (bytes > 0) {
                    entry->offset += bytes;
                }
                return bytes;
            }
            if (entry->type == FD_PIPE){
                struct pipe *p = entry->pipe;
                if (p->count == 0) return 0;
                int to_read = p->count;
                if (to_read > count) to_read = count;
                for (int i = 0; i < to_read; i++){
                    buf[i] = p->buffer[p->read_pos];
                    p->read_pos = (p->read_pos + 1) % PIPE_BUF_SIZE;
                    p->count--;
                }
                return to_read;
            }

            return -1;
        }

        case SYS_WRITE: {
            int fd = (int)arg1;
            const char *buf = (const char *)arg2;
            int count = (int)arg3;
            struct task *t = sched_current();
            struct fd_entry *entry = task_fd_get(t, fd);
            if (!entry) return -1;

            if (entry->type == FD_CONSOLE) {
                return console_write(buf, count);
            }
            if (entry->type == FD_PIPE){
                struct pipe *p = entry->pipe;
                if (p->count >= PIPE_BUF_SIZE) return 0;
                int free_space = PIPE_BUF_SIZE - p->count;
                int to_write = count;
                if (to_write > free_space) to_write = free_space;
                for (int i = 0; i < to_write; i++){
                    p->buffer[p->write_pos] = buf[i];
                    p->write_pos = (p->write_pos + 1) % PIPE_BUF_SIZE;
                    p->count++;
                }
                return to_write;
            }
            return -1;
        }

        case SYS_OPEN: {
            const char *path = (const char *)arg1;
            int flags = (int)arg2;

            char full_path[VFS_MAX_PATH];
            build_path(path, full_path);

            struct vfs_node *node = vfs_resolve_path(full_path);
            if (!node) return -1;
            struct task *t = sched_current();
            int fd = task_fd_alloc(t);

            if (fd < 0) return -1;

            t->fd_table[fd].node = node;
            t->fd_table[fd].offset = 0;
            t->fd_table[fd].flags = flags;

            if (node->flags & VFS_DIRECTORY) {
                t->fd_table[fd].type = FD_DIR;
            } else {
                t->fd_table[fd].type = FD_FILE;
            }

            if ((flags & O_APPEND) && (t->fd_table[fd].type == FD_FILE)) {
                t->fd_table[fd].offset = node->size;
            }

            return fd;
        }

        case SYS_CLOSE: {
            int fd = (int)arg1;
            struct task *t = sched_current();
            if (fd < 0) return -1;
            struct fd_entry *entry = task_fd_get(t, fd);
            if (!entry || fd < 3) return -1;

            task_fd_free(t, fd);
            return 0;
        }

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

        case SYS_FSTAT: {
            int fd = (int)arg1;
            if (fd < 0) return -1;
            struct stat *buf = (struct stat *)arg2;
            struct task *t = sched_current();
            struct fd_entry *entry = task_fd_get(t, fd);
            if (!entry || !entry->node) return -1;

            buf->st_size = entry->node->size;
            buf->st_ino = entry->node->inode;
            buf->st_mode = (entry->node->flags & VFS_DIRECTORY) ? S_IFDIR : S_IFREG;

            return 0;
        }

        case SYS_MKDIR: {
            const char *path = (const char *)arg1;

            char full_path[VFS_MAX_PATH];
            build_path(path, full_path);

            struct vfs_node *result = ensure_path_exists(full_path);
            return result ? 0 : -1;
        }

        case SYS_RMDIR: {
            const char *path = (const char *)arg1;
            char full_path[VFS_MAX_PATH];
            build_path(path, full_path);
            int res = fat32_rmdir_path(full_path);
            return (res == 0) ? 0 : -1;
        }

        case SYS_UNLINK: {
            const char *path = (const char *)arg1;
            char full_path[VFS_MAX_PATH];
            build_path(path, full_path);
            int res = fat32_rm_path(full_path);
            return (res == 0) ? 0 : -1;
        }

        case SYS_READDIR: {
            int fd = (int)arg1;
            if (fd < 0) return -1;
            struct user_dirent *buf = (struct user_dirent *)arg2;
            int index = (int)arg3;

            struct task *t = sched_current();

            struct fd_entry *entry = task_fd_get(t, fd);
            if (!entry || entry->type != FD_DIR || !entry->node) return -1;

            struct dirent *dent = vfs_readdir(entry->node, index);
            if (!dent) return -1;

            str_copy(buf->name, dent->name, 256);

            struct vfs_node *child = vfs_finddir(entry->node, dent->name);
            buf->type = (child && (child->flags & VFS_DIRECTORY)) ? 1 : 0;

            return 0;
        }

        case SYS_CHDIR: {
            struct task *t = sched_current();
            const char *path = (const char *)arg1;

            char full_path[VFS_MAX_PATH];
            build_path(path, full_path);

            struct vfs_node *node = vfs_resolve_path(full_path);
            if (!node) return -1;
            if (!(node->flags & VFS_DIRECTORY)) return -1;

            str_copy(t->cwd, full_path, VFS_MAX_PATH);
            return 0;
        }

        case SYS_GETCWD: {
            struct task *t = sched_current();
            char *buf = (char *)arg1;
            int size = (int)arg2;

            int len = str_len(t->cwd);
            if (len >= size) return -1;

            str_copy(buf, t->cwd, size);
            return len;
        }

        case SYS_RENAME:
            // rename(old, new)
        {
            const char *oldp = (const char *)arg1;
            const char *newp = (const char *)arg2;
            char full_old[VFS_MAX_PATH];
            char full_new[VFS_MAX_PATH];
            build_path(oldp, full_old);
            build_path(newp, full_new);
            int res = fat32_mv_path(full_old, full_new);
            return (res == 0) ? 0 : -1;
        }

        case SYS_TRUNCATE: {
            const char *path = (const char *)arg1;
            int size = (int)arg2;
            char full_path[VFS_MAX_PATH];
            build_path(path, full_path);
            struct vfs_node *node = vfs_resolve_path(full_path);
            if (!node) return -1;
            return (fat32_truncate(node, size) == 0) ? 0 : -1;
        }

        case SYS_CREATE: {
            const char *path = (const char *)arg1;
            char full_path[VFS_MAX_PATH];
            build_path(path, full_path);
            int res = fat32_touch_path(full_path);
            return (res == 0) ? 0 : -1;
        }

        case SYS_SEEK: {
            struct task *t = sched_current();
            int fd = (int)arg1;
            struct fd_entry *entry = task_fd_get(t, fd);
            if (!entry || entry->type == FD_CONSOLE || entry->type == FD_DIR) return -1;
            int offset = (int)arg2;
            int whence = (int)arg3;
            int new_offset = 0;
            if (whence == SEEK_SET) new_offset = offset;
            if (whence == SEEK_CUR) new_offset = entry->offset + offset;
            if (whence == SEEK_END) new_offset = entry->node->size + offset;
            if (new_offset < 0) return -1;
            entry->offset = new_offset;
            return new_offset;
        }

        case SYS_YIELD: {
            sched_yield();
            return 0;
        }
        case SYS_PIPE: {
            int *fds = (int *)arg1;

            struct task *t = sched_current();
            struct pipe *pipe = pipe_alloc();
            if (!pipe) return -1;
            int read_fd = task_fd_alloc(t);
            if (read_fd < 0) return -1;

            int write_fd = task_fd_alloc(t);
            if (write_fd < 0){
                task_fd_free(t, read_fd);
                return -1;
            }
            // setup read_fd 
            t->fd_table[read_fd].type = FD_PIPE;
            t->fd_table[read_fd].pipe = pipe;
            t->fd_table[read_fd].flags = O_RDONLY;

            // setup write_fd
            t->fd_table[write_fd].type = FD_PIPE;
            t->fd_table[write_fd].pipe = pipe;
            t->fd_table[write_fd].flags = O_WRONLY;

            fds[0] = read_fd;
            fds[1] = write_fd;

            return 0;
        }

        default:
            return -1;
    }
}
