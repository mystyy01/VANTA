#ifndef SYSCALL_H
#define SYSCALL_H

#include <stdint.h>

// MSR addresses
#define MSR_EFER    0xC0000080
#define MSR_STAR    0xC0000081
#define MSR_LSTAR   0xC0000082
#define MSR_FMASK   0xC0000084

// EFER bits
#define EFER_SCE    (1 << 0)    // System Call Extensions enable

// Syscall numbers
#define SYS_EXIT      0   // exit(int code)
#define SYS_READ      1   // read(int fd, char *buf, int count) -> bytes read
#define SYS_WRITE     2   // write(int fd, char *buf, int count) -> bytes written
#define SYS_OPEN      3   // open(char *path, int flags) -> fd
#define SYS_CLOSE     4   // close(int fd) -> 0 or -1
#define SYS_STAT      5   // stat(char *path, struct stat *buf) -> 0 or -1
#define SYS_FSTAT     6   // fstat(int fd, struct stat *buf) -> 0 or -1
#define SYS_MKDIR     7   // mkdir(char *path) -> 0 or -1
#define SYS_RMDIR     8   // rmdir(char *path) -> 0 or -1
#define SYS_UNLINK    9   // unlink(char *path) -> 0 or -1
#define SYS_READDIR   10  // readdir(int fd, struct dirent *buf, int index) -> 0 or -1
#define SYS_CHDIR     11  // chdir(char *path) -> 0 or -1
#define SYS_GETCWD    12  // getcwd(char *buf, int size) -> buf or NULL
#define SYS_RENAME    13  // rename(char *old, char *new) -> 0 or -1
#define SYS_TRUNCATE  14  // truncate(char *path, int size) -> 0 or -1
#define SYS_CREATE    15  // create(char *path) -> fd or -1 (create new file)
#define SYS_SEEK      16  // seek(int fd, int offset, int whence) -> new offset or -1

// Open flags
#define O_RDONLY    0x0000
#define O_WRONLY    0x0001
#define O_RDWR      0x0002
#define O_CREAT     0x0100
#define O_TRUNC     0x0200
#define O_APPEND    0x0400

// Seek whence values
#define SEEK_SET    0
#define SEEK_CUR    1
#define SEEK_END    2

// Stat structure (simplified)
struct stat {
    uint32_t st_size;      // File size in bytes
    uint32_t st_mode;      // File type and permissions
    uint32_t st_ino;       // Inode number (cluster for FAT32)
};

// File types for st_mode
#define S_IFREG     0x8000  // Regular file
#define S_IFDIR     0x4000  // Directory

// Dirent structure for readdir
struct user_dirent {
    char name[256];        // File name
    uint32_t type;         // 0 = file, 1 = directory
};

// Standard file descriptors
#define STDIN_FD    0
#define STDOUT_FD   1
#define STDERR_FD   2

// Initialize syscall mechanism (call once at boot)
void syscall_init(void);

// The syscall handler (called from assembly)
uint64_t syscall_handler(uint64_t syscall_num, uint64_t arg1, uint64_t arg2,
                         uint64_t arg3, uint64_t arg4, uint64_t arg5);

#endif
