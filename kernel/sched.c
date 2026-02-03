#include "sched.h"
#include "pmm.h"
#include "paging.h"
#include "elf_loader.h"
#include "fs/vfs.h"

#define MAX_TASKS 16
#define KSTACK_SIZE (16 * 1024)
#define USTACK_SIZE (16 * 1024)

static struct task tasks[MAX_TASKS];
static struct task *runq = 0;
static struct task *current = 0;
static uint64_t next_task_id = 1;
static int sched_ready = 0;

extern void ctx_switch(uint64_t *old_rsp, uint64_t new_rsp, uint64_t new_cr3);
extern void user_mode_enter(uint64_t rip, uint64_t rsp);

void sched_init(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].state = TASK_STATE_UNUSED;
        tasks[i].next = 0;
    }
    sched_ready = 1;
}

static struct task *alloc_task(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_STATE_UNUSED) {
            tasks[i].state = TASK_STATE_RUNNABLE;
            tasks[i].id = next_task_id++;
            tasks[i].next = 0;
            return &tasks[i];
        }
    }
    return 0;
}

static void enqueue(struct task *t) {
    if (!runq) {
        runq = t->next = t;
    } else {
        t->next = runq->next;
        runq->next = t;
    }
}

struct task *sched_create_user(struct vfs_node *node, char **args) {
    (void)args; // Not implemented yet
    struct task *t = alloc_task();
    if (!t) return 0;

    // Build per-task address space
    uint64_t *pml4 = paging_new_user_space();
    if (!pml4) { t->state = TASK_STATE_UNUSED; return 0; }
    t->cr3 = (uint64_t)pml4;

    // TODO: load ELF into pages and map
    // For now, fail gracefully
    t->state = TASK_STATE_UNUSED;
    return 0;
}

void sched_tick(void) {
    if (!sched_ready || !runq || !current) return;
    struct task *prev = current;
    current = current->next;
    if (current == prev) return;
    ctx_switch(&prev->rsp, current->rsp, current->cr3);
}

void sched_yield(void) {
    sched_tick();
}

void sched_exit(int code) {
    (void)code;
    if (!current) return;
    current->state = TASK_STATE_ZOMBIE;
    // Remove from run queue
    if (current->next == current) {
        runq = current = 0;
    } else {
        struct task *p = current;
        while (p->next != current) p = p->next;
        p->next = current->next;
        if (runq == current) runq = current->next;
        current = current->next;
        ctx_switch(&p->rsp, current->rsp, current->cr3);
    }
}

struct task *sched_current(void) {
    return current;
}
