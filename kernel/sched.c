#include "sched.h"
#include "paging.h"
#include "elf_loader.h"
#include "fs/vfs.h"
#include "isr.h"
#include "gdt.h"
#include "syscall.h"

#define MAX_TASKS 16
#define KSTACK_SIZE (16 * 1024)
#define USTACK_SIZE (16 * 1024)

static struct task tasks[MAX_TASKS];
static struct task *runq = 0;
static struct task *current = 0;
static uint64_t next_task_id = 1;
static int sched_ready = 0;
static int sched_running = 0;

static uint8_t kstacks[MAX_TASKS][KSTACK_SIZE] __attribute__((aligned(4096)));
static uint8_t ustacks[MAX_TASKS][USTACK_SIZE] __attribute__((aligned(4096)));
volatile uint64_t sched_last_next_cs = 0;
volatile int sched_last_next_is_user = 0;
volatile int sched_dbg_runq = 0;
volatile int sched_dbg_user = 0;
volatile int sched_dbg_ready = 0;
volatile int sched_dbg_running = 0;
volatile int sched_dbg_has_runq = 0;
volatile int sched_dbg_has_current = 0;
volatile int sched_dbg_in_syscall = 0;
volatile int sched_dbg_inited = 0;
volatile int sched_dbg_bootstrap = 0;
volatile int sched_dbg_created_user = 0;
volatile int sched_dbg_created_kernel = 0;
extern volatile int in_syscall;
extern void print_color(const char *str, int row, unsigned char color);

static void dbg_sched_marker(char c) {
    volatile unsigned short *vga = (volatile unsigned short *)0xB8000;
    vga[18 * 80 + 0] = (0x0E << 8) | (unsigned short)c;
}

static void mem_zero(void *dst, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = 0;
}

void sched_init(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].state = TASK_STATE_UNUSED;
        tasks[i].next = 0;
    }
    runq = 0;
    current = 0;
    next_task_id = 1;
    sched_running = 0;
    sched_ready = 1;
    sched_dbg_inited = 1;
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
    sched_dbg_bootstrap = 1;
}

struct task *sched_create_kernel(void (*entry)(void)) {
    struct task *t = alloc_task();
    if (!t) return 0;

    int idx = task_index(t);
    t->kernel_stack_base = (uint64_t)&kstacks[idx][0];
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
    sched_dbg_created_kernel++;
    return t;
}

struct task *sched_create_user(struct vfs_node *node, char **args) {
    (void)args;
    struct task *t = alloc_task();
    if (!t) return 0;

    uint64_t entry = 0;
    if (elf_load(node, &entry) < 0) {
        print_color("elf_load fail", 20, 0x0C);
        t->state = TASK_STATE_UNUSED;
        return 0;
    }
    print_color("elf_load ok", 20, 0x0A);

    int idx = task_index(t);
    t->kernel_stack_base = (uint64_t)&kstacks[idx][0];
    t->kernel_stack_top = t->kernel_stack_base + KSTACK_SIZE;
    paging_mark_supervisor_region(t->kernel_stack_base, KSTACK_SIZE);

    t->user_stack_top = (uint64_t)&ustacks[idx][0] + USTACK_SIZE;
    paging_mark_user_region(t->user_stack_top - USTACK_SIZE, USTACK_SIZE);
    print_color("ustack ok", 21, 0x0A);

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

    struct irq_frame_user *frame = (struct irq_frame_user *)(t->kernel_stack_top - sizeof(struct irq_frame_user));
    mem_zero(frame, sizeof(*frame));
    frame->base.rip = entry;
    frame->base.cs = 0x23;
    frame->base.rflags = 0x202;
    frame->base.int_no = 0;
    frame->base.err_code = 0;
    frame->rsp = (uint64_t)sp;
    frame->ss = 0x1B;

    t->rsp = (uint64_t)frame;
    t->entry = entry;
    t->is_user = 1;
    print_color("frame ok", 22, 0x0A);

    enqueue(t);
    print_color("enq ok", 23, 0x0A);
    sched_dbg_created_user++;
    return t;
}

struct irq_frame *sched_tick(struct irq_frame *frame) {
    if (!frame) return frame;
    sched_dbg_ready = sched_ready ? 1 : 0;
    sched_dbg_running = sched_running ? 1 : 0;
    sched_dbg_has_runq = runq ? 1 : 0;
    sched_dbg_has_current = current ? 1 : 0;
    sched_dbg_in_syscall = in_syscall ? 1 : 0;

    if (!sched_ready || !runq || !current) return frame;
    if (in_syscall) return frame;
    if (!sched_running) return frame;

    current->rsp = (uint64_t)frame;

    // Debug: count runnable tasks and user tasks in the run queue.
    {
        int count = 0;
        int user = 0;
        struct task *t = runq;
        if (t) {
            do {
                if (t->state == TASK_STATE_RUNNABLE) {
                    count++;
                    if (t->is_user) user++;
                }
                t = t->next;
            } while (t && t != runq && count < (MAX_TASKS + 1));
        }
        sched_dbg_runq = count;
        sched_dbg_user = user;
    }

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
    {
        struct irq_frame *next = (struct irq_frame *)current->rsp;
        sched_last_next_is_user = current->is_user;
        sched_last_next_cs = next ? next->cs : 0;
    }
    dbg_sched_marker(current->is_user ? 'U' : 'K');
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
