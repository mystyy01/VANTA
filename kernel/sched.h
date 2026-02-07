#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include "fs/vfs.h"

struct irq_frame;

#define TASK_STATE_UNUSED   0
#define TASK_STATE_RUNNABLE 1
#define TASK_STATE_ZOMBIE   2

// ============================================================================
// Per-process file descriptor table
// ============================================================================

#define MAX_FDS 64
#define FD_UNUSED   0
#define FD_FILE     1
#define FD_DIR      2
#define FD_CONSOLE  3
#define FD_PIPE     4

#define PIPE_BUF_SIZE 512

struct pipe{
    char buffer[PIPE_BUF_SIZE];
    int read_pos;
    int write_pos;
    int count;
    int read_open;
    int write_open;
};

struct fd_entry {
    int type;
    struct vfs_node *node;
    uint32_t offset;
    int flags;
    struct pipe *pipe;
};

struct task {
    uint64_t id;
    uint64_t cr3;
    uint64_t rsp;
    uint64_t kernel_stack_base;
    uint64_t kernel_stack_top;
    uint64_t user_stack_top;
    uint64_t entry;
    int is_user;
    int is_idle;
    int state;
    struct task *next;

    // Per-process state
    struct fd_entry fd_table[MAX_FDS];
    char cwd[VFS_MAX_PATH];
};



// Initialize scheduler structures
void sched_init(void);

// Called from timer IRQ
struct irq_frame *sched_tick(struct irq_frame *frame);

// Enable scheduling (preemption)
void sched_start(void);

// Create a runnable kernel task
struct task *sched_create_kernel(void (*entry)(void));

// Create a runnable user task from an ELF file node (minimal stub for now)
struct task *sched_create_user(struct vfs_node *node, char **args);

// Bootstrap current kernel context as a task
void sched_bootstrap_current(void);

// Yield current task (syscall or cooperative)
void sched_yield(void);

// Exit current task with code
void sched_exit(int code);

// Current task pointer
struct task *sched_current(void);

// Per-process FD table helpers
void task_fd_init(struct task *t);
int task_fd_alloc(struct task *t);
void task_fd_free(struct task *t, int fd);
struct fd_entry *task_fd_get(struct task *t, int fd);

struct pipe *pipe_alloc(void);

#endif
