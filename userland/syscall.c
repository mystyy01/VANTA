#include "syscall.h"

// Raw syscall using the syscall instruction
// Arguments: RAX=num, RDI=arg1, RSI=arg2, RDX=arg3, R10=arg4, R8=arg5
long syscall(long num, long arg1, long arg2, long arg3, long arg4, long arg5) {
    long result;

    __asm__ volatile (
        "movq %1, %%rax\n"   // syscall number
        "movq %2, %%rdi\n"   // arg1
        "movq %3, %%rsi\n"   // arg2
        "movq %4, %%rdx\n"   // arg3
        "movq %5, %%r10\n"   // arg4 (r10 instead of rcx, which gets clobbered)
        "movq %6, %%r8\n"    // arg5
        "syscall\n"
        "movq %%rax, %0\n"   // result
        : "=r"(result)
        : "r"(num), "r"(arg1), "r"(arg2), "r"(arg3), "r"(arg4), "r"(arg5)
        : "rax", "rdi", "rsi", "rdx", "r10", "r8", "r11", "rcx", "memory"
    );

    return result;
}

// Create a directory (and all parent directories)
int mkdir(const char *path) {
    return (int)syscall(SYS_MKDIR, (long)path, 0, 0, 0, 0);
}

// Exit the program
void exit(int status) {
    syscall(SYS_EXIT, status, 0, 0, 0, 0);
    // Should never return, but just in case:
    while (1) {}
}
