#ifndef USERLAND_SYSCALL_H
#define USERLAND_SYSCALL_H

// Syscall numbers (must match kernel/syscall.h)
#define SYS_MKDIR   1
#define SYS_READ    2
#define SYS_WRITE   3
#define SYS_OPEN    4
#define SYS_CLOSE   5
#define SYS_EXIT    6

// Raw syscall - takes syscall number and up to 5 arguments
long syscall(long num, long arg1, long arg2, long arg3, long arg4, long arg5);

// Convenience wrappers
int mkdir(const char *path);
void exit(int status);

#endif
