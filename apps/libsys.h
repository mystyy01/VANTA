// PHOBOS Userspace Syscall Library
// Include this in your apps to use syscalls

#ifndef LIBSYS_H
#define LIBSYS_H

// ============================================================================
// Syscall Numbers (must match kernel/syscall.h)
// ============================================================================

#define SYS_EXIT      0   // exit(int code)
#define SYS_READ      1   // read(int fd, char *buf, int count) -> bytes read
#define SYS_WRITE     2   // write(int fd, char *buf, int count) -> bytes written
#define SYS_OPEN      3   // open(char *path, int flags) -> fd
#define SYS_CLOSE     4   // close(int fd) -> 0 or -1
#define SYS_STAT      5   // stat(char *path, struct stat *buf) -> 0 or -1
#define SYS_FSTAT     6   // fstat(int fd, struct stat *buf) -> 0 or -1
#define SYS_MKDIR     7   // mkdir(char *path) -> 0 or -1
#define SYS_RMDIR     8   // rmdir(char *path) -> 0 or -1  [TODO]
#define SYS_UNLINK    9   // unlink(char *path) -> 0 or -1 [TODO]
#define SYS_READDIR   10  // readdir(int fd, struct dirent *buf, int index) -> 0 or -1
#define SYS_CHDIR     11  // chdir(char *path) -> 0 or -1
#define SYS_GETCWD    12  // getcwd(char *buf, int size) -> length or -1
#define SYS_RENAME    13  // rename(char *old, char *new) -> 0 or -1 [TODO]
#define SYS_TRUNCATE  14  // truncate(char *path, int size) -> 0 or -1 [TODO]
#define SYS_CREATE    15  // create(char *path) -> fd or -1 [TODO]
#define SYS_SEEK      16  // seek(int fd, int offset, int whence) -> new offset or -1
#define SYS_YIELD     17  // yield()

// ============================================================================
// Open Flags
// ============================================================================

#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0100
#define O_TRUNC     0x0200
#define O_APPEND    0x0400

// ============================================================================
// Seek Whence
// ============================================================================

#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

// ============================================================================
// Standard File Descriptors
// ============================================================================

#define STDIN       0
#define STDOUT      1
#define STDERR      2

// ============================================================================
// Stat Structure
// ============================================================================

struct stat {
    unsigned int st_size;      // File size in bytes
    unsigned int st_mode;      // File type and permissions
    unsigned int st_ino;       // Inode number
};

#define S_IFREG     0x8000  // Regular file
#define S_IFDIR     0x4000  // Directory

// ============================================================================
// Directory Entry (for readdir)
// ============================================================================

struct dirent {
    char name[256];
    unsigned int type;  // 0 = file, 1 = directory
};

// ============================================================================
// Syscall Wrappers
// ============================================================================

static inline long syscall0(long num) {
    long result;
    __asm__ volatile (
        "syscall"
        : "=a"(result)
        : "a"(num)
        : "rcx", "r11", "memory"
    );
    return result;
}

static inline long syscall1(long num, long arg1) {
    long result;
    __asm__ volatile (
        "syscall"
        : "=a"(result)
        : "a"(num), "D"(arg1)
        : "rcx", "r11", "memory"
    );
    return result;
}

static inline long syscall2(long num, long arg1, long arg2) {
    long result;
    __asm__ volatile (
        "syscall"
        : "=a"(result)
        : "a"(num), "D"(arg1), "S"(arg2)
        : "rcx", "r11", "memory"
    );
    return result;
}

static inline long syscall3(long num, long arg1, long arg2, long arg3) {
    long result;
    __asm__ volatile (
        "syscall"
        : "=a"(result)
        : "a"(num), "D"(arg1), "S"(arg2), "d"(arg3)
        : "rcx", "r11", "memory"
    );
    return result;
}

// ============================================================================
// Convenience Functions
// ============================================================================

static inline void exit(int code) {
    syscall1(SYS_EXIT, code);
    while (1) __asm__ volatile ("hlt");
}

static inline int strlen(const char *s) {
    int len = 0;
    while (s[len]) len++;
    return len;
}

static inline void print(const char *s) {
    syscall3(SYS_WRITE, STDOUT, (long)s, strlen(s));
}

static inline void eprint(const char *s) {
    syscall3(SYS_WRITE, STDERR, (long)s, strlen(s));
}

static inline int open(const char *path, int flags) {
    return (int)syscall2(SYS_OPEN, (long)path, flags);
}

static inline int close(int fd) {
    return (int)syscall1(SYS_CLOSE, fd);
}

static inline int read(int fd, char *buf, int count) {
    return (int)syscall3(SYS_READ, fd, (long)buf, count);
}
static inline int write(int fd, const char *buf, int count) {
    return (int)syscall3(SYS_WRITE, fd, (long)buf, count);
}

static inline int stat(const char *path, struct stat *buf) {
    return (int)syscall2(SYS_STAT, (long)path, (long)buf);
}

static inline int fstat(int fd, struct stat *buf) {
    return (int)syscall2(SYS_FSTAT, fd, (long)buf);
}

static inline int mkdir(const char *path) {
    return (int)syscall1(SYS_MKDIR, (long)path);
}

static inline int rmdir(const char *path) {
    return (int)syscall1(SYS_RMDIR, (long)path);
}

static inline int unlink(const char *path) {
    return (int)syscall1(SYS_UNLINK, (long)path);
}

static inline int create(const char *path) {
    return (int)syscall1(SYS_CREATE, (long)path);
}

static inline int readdir(int fd, struct dirent *buf, int index) {
    return (int)syscall3(SYS_READDIR, fd, (long)buf, index);
}

static inline int chdir(const char *path) {
    return (int)syscall1(SYS_CHDIR, (long)path);
}

static inline int getcwd(char *buf, int size) {
    return (int)syscall2(SYS_GETCWD, (long)buf, size);
}

static inline int rename(const char *oldpath, const char *newpath) {
    return (int)syscall2(SYS_RENAME, (long)oldpath, (long)newpath);
}

static inline int seek(int fd, int offset, int whence) {
    return (int)syscall3(SYS_SEEK, fd, offset, whence);
}

static inline int yield(void) {
    return (int)syscall0(SYS_YIELD);
}

#endif // LIBSYS_H
