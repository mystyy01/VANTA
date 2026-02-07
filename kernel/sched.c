#include "sched.h"
#include "paging.h"
#include "pmm.h"
#include "elf_loader.h"
#include "fs/vfs.h"
#include "isr.h"
#include "gdt.h"
#include "syscall.h"

#define MAX_TASKS 16
#define MAX_PIPES 16
#define KSTACK_SIZE (16 * 1024)
#define USTACK_SIZE (16 * 1024)
#define KSTACK_PAGES (KSTACK_SIZE / 4096)
#define USTACK_PAGES (USTACK_SIZE / 4096)

static struct task tasks[MAX_TASKS];
static struct task *runq = 0;
static struct task *current = 0;
static struct pipe pipes[MAX_PIPES];
static uint64_t next_task_id = 1;
static int sched_ready = 0;
static int sched_running = 0;
// Stacks are now allocated from PMM (above 1MB) to avoid ROM area
static uint8_t *kstacks[MAX_TASKS];  // Pointers to PMM-allocated stacks
static uint8_t *ustacks[MAX_TASKS];  // Pointers to PMM-allocated stacks

extern volatile int in_syscall;

static void mem_zero(void *dst, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = 0;
}

// Allocate contiguous pages for a stack from PMM
static uint8_t *alloc_stack(int num_pages) {
    // Allocate first page as base
    uint8_t *base = (uint8_t *)pmm_alloc_page();
    if (!base) return 0;

    // Allocate remaining pages (they should be contiguous in our simple PMM)
    for (int i = 1; i < num_pages; i++) {
        uint8_t *page = (uint8_t *)pmm_alloc_page();
        if (!page) return 0;  // Leak previous pages, but keeps it simple
    }
    return base;
}

void sched_init(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].state = TASK_STATE_UNUSED;
        tasks[i].next = 0;
        kstacks[i] = 0;
        ustacks[i] = 0;
    }
    runq = 0;
    current = 0;
    next_task_id = 1;
    sched_running = 0;
    sched_ready = 1;
}

static struct task *alloc_task(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_STATE_UNUSED) {
            tasks[i].state = TASK_STATE_RUNNABLE;
            tasks[i].id = next_task_id++;
            tasks[i].next = 0;
            tasks[i].cr3 = 0;
            tasks[i].rsp = 0;
            tasks[i].kernel_stack_base = 0;
            tasks[i].kernel_stack_top = 0;
            tasks[i].user_stack_top = 0;
            tasks[i].entry = 0;
            tasks[i].is_user = 0;
            tasks[i].is_idle = 0;
            task_fd_init(&tasks[i]);  // Initialize per-process FD table
            return &tasks[i];
        }
    }
    return 0;
}

static int task_index(struct task *t) {
    return (int)(t - tasks);
}

static void enqueue(struct task *t) {
    if (!runq) {
        runq = t->next = t;
    } else {
        t->next = runq->next;
        runq->next = t;
    }
}

void sched_bootstrap_current(void) {
    struct task *t = alloc_task();
    if (!t) return;
    t->is_user = 0;
    current = t;
    enqueue(t);
}

struct task *sched_create_kernel(void (*entry)(void)) {
    struct task *t = alloc_task();
    if (!t) return 0;

    int idx = task_index(t);
    // Allocate kernel stack from PMM (above ROM area)
    kstacks[idx] = alloc_stack(KSTACK_PAGES);
    if (!kstacks[idx]) {
        t->state = TASK_STATE_UNUSED;
        return 0;
    }
    t->kernel_stack_base = (uint64_t)kstacks[idx];
    t->kernel_stack_top = t->kernel_stack_base + KSTACK_SIZE;
    paging_mark_supervisor_region(t->kernel_stack_base, KSTACK_SIZE);

    struct irq_frame *frame = (struct irq_frame *)(t->kernel_stack_top - sizeof(struct irq_frame));
    mem_zero(frame, sizeof(*frame));
    frame->rip = (uint64_t)entry;
    frame->cs = 0x08;
    frame->rflags = 0x202;
    frame->int_no = 0;
    frame->err_code = 0;

    t->rsp = (uint64_t)frame;
    t->entry = (uint64_t)entry;
    t->is_user = 0;

    enqueue(t);
    return t;
}

struct task *sched_create_user(struct vfs_node *node, char **args) {
    (void)args;
    struct task *t = alloc_task();
    if (!t) return 0;

    uint64_t entry = 0;
    if (elf_load(node, &entry) < 0) {
        t->state = TASK_STATE_UNUSED;
        return 0;
    }

    int idx = task_index(t);
    // Allocate kernel stack from PMM (above ROM area)
    kstacks[idx] = alloc_stack(KSTACK_PAGES);
    if (!kstacks[idx]) {
        t->state = TASK_STATE_UNUSED;
        return 0;
    }
    t->kernel_stack_base = (uint64_t)kstacks[idx];
    t->kernel_stack_top = t->kernel_stack_base + KSTACK_SIZE;
    paging_mark_supervisor_region(t->kernel_stack_base, KSTACK_SIZE);

    // Allocate user stack from PMM (above ROM area)
    ustacks[idx] = alloc_stack(USTACK_PAGES);
    if (!ustacks[idx]) {
        t->state = TASK_STATE_UNUSED;
        return 0;
    }
    t->user_stack_top = (uint64_t)ustacks[idx] + USTACK_SIZE;
    paging_mark_user_region(t->user_stack_top - USTACK_SIZE, USTACK_SIZE);

    // Set up exit stub on user stack
    uint8_t *stack_top = (uint8_t *)t->user_stack_top;
    uint8_t *stub = stack_top - 32;
    uint32_t sys_exit = SYS_EXIT;
    stub[0] = 0xB8; // mov eax, imm32
    stub[1] = (uint8_t)(sys_exit & 0xFF);
    stub[2] = (uint8_t)((sys_exit >> 8) & 0xFF);
    stub[3] = (uint8_t)((sys_exit >> 16) & 0xFF);
    stub[4] = (uint8_t)((sys_exit >> 24) & 0xFF);
    stub[5] = 0x31; // xor edi, edi
    stub[6] = 0xFF;
    stub[7] = 0x0F; // syscall
    stub[8] = 0x05;
    stub[9] = 0xF4; // hlt

    uint8_t *sp = stack_top;
    sp = (uint8_t *)((uint64_t)sp & ~0xFULL);
    sp -= 8;
    *((uint64_t *)sp) = (uint64_t)stub;

    // Set up interrupt frame for returning to user mode
    struct irq_frame_user *frame = (struct irq_frame_user *)(t->kernel_stack_top - sizeof(struct irq_frame_user));
    for (int i = 0; i < (int)(sizeof(struct irq_frame_user) / 8); i++) {
        ((uint64_t *)frame)[i] = 0;
    }

    frame->base.rip = entry;
    frame->base.cs = 0x23;       // User code segment
    frame->base.rflags = 0x202;  // IF set
    frame->rsp = (uint64_t)sp;
    frame->ss = 0x1B;            // User data segment

    t->rsp = (uint64_t)frame;
    t->entry = entry;
    t->is_user = 1;

    enqueue(t);
    return t;
}

struct irq_frame *sched_tick(struct irq_frame *frame) {
    if (!frame) return frame;
    if (!sched_ready || !runq || !current) return frame;
    if (in_syscall) return frame;
    if (!sched_running) return frame;

    current->rsp = (uint64_t)frame;

    struct task *start = current;
    struct task *idle = 0;
    do {
        current = current->next;
        if (!current || current->state != TASK_STATE_RUNNABLE) continue;
        if (!current->is_idle) break;
        if (!idle) idle = current;
    } while (current && current != start);

    if (!current || current->state != TASK_STATE_RUNNABLE) {
        if (idle) {
            current = idle;
        } else {
            return frame;
        }
    }
    if (current->rsp == 0) return frame;
    if (current->kernel_stack_top) {
        tss_set_rsp0(current->kernel_stack_top);
    }
    return (struct irq_frame *)current->rsp;
}

void sched_start(void) {
    sched_running = 1;
}

void sched_yield(void) {
    (void)sched_tick(0);
}

void sched_exit(int code) {
    (void)code;
    if (!current) return;
    current->state = TASK_STATE_ZOMBIE;
    while (1) {
        __asm__ volatile ("sti; hlt");
    }
}

struct task *sched_current(void) {
    return current;
}

// ============================================================================
// Per-process FD table helpers
// ============================================================================

void task_fd_init(struct task *t) {
    // - Clear all 64 entries to FD_UNUSED
    for (int i = 0; i < MAX_FDS; i++){
        t->fd_table[i].type = FD_UNUSED;
    }
    // - Set up fd 0, 1, 2 as FD_CONSOLE (stdin, stdout, stderr)
    t->fd_table[0].type = FD_CONSOLE;
    t->fd_table[1].type = FD_CONSOLE;
    t->fd_table[2].type = FD_CONSOLE;

    // - Initialize cwd to "/"
    t->cwd[0] = '/';
    t->cwd[1] = '\0';
}

int task_fd_alloc(struct task *t) {
    for (int i = 3; i < MAX_FDS; i++) {
        if (t->fd_table[i].type == FD_UNUSED) {
            return i;
        }
    }
    return -1;  // No free FDs
}

void task_fd_free(struct task *t, int fd) {
    if (fd >= 3 && fd < MAX_FDS) {
        t->fd_table[fd].type = FD_UNUSED;
        t->fd_table[fd].node = 0;
        t->fd_table[fd].offset = 0;
        t->fd_table[fd].flags = 0;
    }
}

struct fd_entry *task_fd_get(struct task *t, int fd) {
    if (fd < 0 || fd >= MAX_FDS) return 0;
    if (t->fd_table[fd].type == FD_UNUSED) return 0;
    return &t->fd_table[fd];
}

struct pipe *pipe_alloc(void){
    for (int i = 0; i < MAX_PIPES; i++){
        if (pipes[i].read_open == 0 && pipes[i].write_open == 0){
            // found and empty spot for a pipe
            pipes[i].read_pos = 0;
            pipes[i].write_pos = 0;
            pipes[i].count = 0;

            pipes[i].read_open = 1;
            pipes[i].write_open = 1;

            return &pipes[i];
        }   
    }
    return 0; 
}