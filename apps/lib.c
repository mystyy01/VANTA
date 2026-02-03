// Userland library for mkdir command
// This gets linked with the mt-lang compiled code

// Syscall numbers (must match kernel/syscall.h)
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

// ============================================================================
// Syscall Interface
// ============================================================================

static long syscall(long num, long arg1, long arg2, long arg3, long arg4, long arg5) {
    long result;

    __asm__ volatile (
        "movq %1, %%rax\n"
        "movq %2, %%rdi\n"
        "movq %3, %%rsi\n"
        "movq %4, %%rdx\n"
        "movq %5, %%r10\n"
        "movq %6, %%r8\n"
        "syscall\n"
        "movq %%rax, %0\n"
        : "=r"(result)
        : "r"(num), "r"(arg1), "r"(arg2), "r"(arg3), "r"(arg4), "r"(arg5)
        : "rax", "rdi", "rsi", "rdx", "r10", "r8", "r11", "rcx", "memory"
    );

    return result;
}

// ============================================================================
// Functions exposed to mt-lang
// ============================================================================

int mkdir(const char *path) {
    return (int)syscall(SYS_MKDIR, (long)path, 0, 0, 0, 0);
}

void app_exit(int status) {
    syscall(SYS_EXIT, status, 0, 0, 0, 0);
    while (1) {}
}

// ============================================================================
// Memory (minimal bump allocator)
// ============================================================================

#define HEAP_SIZE 4096
static char heap[HEAP_SIZE];
static int heap_offset = 0;

char* malloc(int size) {
    int aligned = (size + 7) & ~7;
    if (heap_offset + aligned > HEAP_SIZE) return (char*)0;
    char* ptr = &heap[heap_offset];
    heap_offset += aligned;
    return ptr;
}

void free(void* ptr) { (void)ptr; }

void* memcpy(void* dst, const void* src, int n) {
    char* d = (char*)dst;
    const char* s = (const char*)src;
    while (n-- > 0) *d++ = *s++;
    return dst;
}

void* memset(void* s, int c, int n) {
    char* p = (char*)s;
    while (n-- > 0) *p++ = (char)c;
    return s;
}


void mt_print(const char *str) {
    syscall(SYS_WRITE, (long)str, 0, 0, 0, 0);
}

// ============================================================================
// Standard C functions expected by mt-lang runtime
// ============================================================================

// Simple printf that just prints the format string (no actual formatting)
int printf(const char *fmt, ...) {
    mt_print(fmt);
    return 0;
}

// Exit function
void exit(int status) {
    app_exit(status);
}
