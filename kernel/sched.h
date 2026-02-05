#ifndef SCHED_H
#define SCHED_H

#include <stdint.h>
#include "fs/vfs.h"

struct irq_frame;

#define TASK_STATE_UNUSED   0
#define TASK_STATE_RUNNABLE 1
#define TASK_STATE_ZOMBIE   2

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

#endif
