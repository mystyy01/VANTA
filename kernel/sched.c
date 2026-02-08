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
static uint8_t *kstacks[MAX_TASKS];

// Global: current task's kernel stack top, used by syscall_entry.asm
uint64_t current_kernel_rsp = 0;

// User context saved by syscall_entry.asm (for fork)
extern uint64_t user_ctx_rsp;
extern uint64_t user_ctx_rip;
extern uint64_t user_ctx_rflags;
extern uint64_t user_ctx_rbx;
extern uint64_t user_ctx_rbp;
extern uint64_t user_ctx_r12;
extern uint64_t user_ctx_r13;
extern uint64_t user_ctx_r14;
extern uint64_t user_ctx_r15;

static void mem_zero(void *dst, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = 0;
}

static void mem_copy(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

// Allocate contiguous pages for a stack from PMM
static uint8_t *alloc_stack(int num_pages) {
    uint8_t *base = (uint8_t *)pmm_alloc_page();
    if (!base) return 0;
    for (int i = 1; i < num_pages; i++) {
        uint8_t *page = (uint8_t *)pmm_alloc_page();
        if (!page) return 0;
    }
    return base;
}

static void free_stack(uint8_t *base, int num_pages) {
    if (!base) return;
    for (int i = 0; i < num_pages; i++) {
        pmm_free_page(base + i * 4096);
    }
}

void sched_init(void) {
    for (int i = 0; i < MAX_TASKS; i++) {
        tasks[i].state = TASK_STATE_UNUSED;
        tasks[i].next = 0;
        kstacks[i] = 0;
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
            tasks[i].parent_id = 0;
            tasks[i].exit_code = 0;
            tasks[i].waiting_for = -1;
            task_fd_init(&tasks[i]);
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

static void dequeue(struct task *t) {
    if (!runq) return;
    if (runq == t && t->next == t) {
        runq = 0;
        return;
    }
    struct task *prev = runq;
    do {
        if (prev->next == t) {
            prev->next = t->next;
            if (runq == t) runq = t->next;
            return;
        }
        prev = prev->next;
    } while (prev != runq);
}

void sched_bootstrap_current(void) {
    struct task *t = alloc_task();
    if (!t) return;
    t->is_user = 0;
    t->cr3 = (uint64_t)paging_kernel_pml4();
    current = t;
    enqueue(t);
}

struct task *sched_create_kernel(void (*entry)(void)) {
    struct task *t = alloc_task();
    if (!t) return 0;

    int idx = task_index(t);
    kstacks[idx] = alloc_stack(KSTACK_PAGES);
    if (!kstacks[idx]) {
        t->state = TASK_STATE_UNUSED;
        return 0;
    }
    t->kernel_stack_base = (uint64_t)kstacks[idx];
    t->kernel_stack_top = t->kernel_stack_base + KSTACK_SIZE;
    t->cr3 = (uint64_t)paging_kernel_pml4();
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
    // Legacy wrapper — delegates to sched_spawn
    (void)args;
    if (!node || !node->name) return 0;

    // Build a path string for sched_spawn (best effort from node name)
    char path[VFS_MAX_PATH];
    int i = 0;
    const char *prefix = "/apps/";
    while (prefix[i]) { path[i] = prefix[i]; i++; }
    int j = 0;
    while (node->name[j] && i < VFS_MAX_PATH - 1) { path[i++] = node->name[j++]; }
    path[i] = '\0';

    int pid = sched_spawn(path, args, 0);
    if (pid < 0) return 0;
    return sched_get_task(pid);
}

// ============================================================================
// Scheduler tick — called from timer IRQ
// ============================================================================

struct irq_frame *sched_tick(struct irq_frame *frame) {
    if (!frame) return frame;
    if (!sched_ready || !runq || !current) return frame;
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

    // Update per-task kernel stack for TSS and syscall entry
    if (current->kernel_stack_top) {
        tss_set_rsp0(current->kernel_stack_top);
        current_kernel_rsp = current->kernel_stack_top;
    }

    // Switch address space
    if (current->cr3) {
        __asm__ volatile ("mov %0, %%cr3" : : "r"(current->cr3) : "memory");
    }

    return (struct irq_frame *)current->rsp;
}

void sched_start(void) {
    sched_running = 1;
}

void sched_yield(void) {
    (void)sched_tick(0);
}

// ============================================================================
// Exit and wait
// ============================================================================

void sched_exit(int code) {
    if (!current) return;
    current->exit_code = code;
    current->state = TASK_STATE_ZOMBIE;

    // Wake parent if it's waiting for us
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state == TASK_STATE_WAITING &&
            tasks[i].waiting_for == (int)current->id) {
            tasks[i].state = TASK_STATE_RUNNABLE;
            tasks[i].waiting_for = -1;
        }
    }

    while (1) {
        __asm__ volatile ("sti; hlt");
    }
}

static void task_reap(struct task *t) {
    int idx = task_index(t);

    // Remove from run queue
    dequeue(t);

    // Free per-process page tables and user pages
    if (t->cr3 && t->cr3 != (uint64_t)paging_kernel_pml4()) {
        paging_free_user_space((uint64_t *)t->cr3);
    }

    // Free kernel stack
    if (kstacks[idx]) {
        free_stack(kstacks[idx], KSTACK_PAGES);
        kstacks[idx] = 0;
    }

    t->state = TASK_STATE_UNUSED;
}

int sched_waitpid(int pid) {
    struct task *child = 0;
    for (int i = 0; i < MAX_TASKS; i++) {
        if ((int)tasks[i].id == pid) {
            child = &tasks[i];
            break;
        }
    }
    if (!child) return -1;

    // Already exited?
    if (child->state == TASK_STATE_ZOMBIE) {
        int code = child->exit_code;
        task_reap(child);
        return code;
    }

    // Block until child exits
    current->state = TASK_STATE_WAITING;
    current->waiting_for = pid;

    // Spin: timer interrupts will preempt us and run other tasks.
    // When the child calls sched_exit(), it wakes us by setting
    // our state back to RUNNABLE.
    while (current->state == TASK_STATE_WAITING) {
        __asm__ volatile ("sti; hlt");
    }

    // Child is now zombie — reap it
    int code = child->exit_code;
    task_reap(child);
    return code;
}

struct task *sched_current(void) {
    return current;
}

struct task *sched_get_task(int pid) {
    for (int i = 0; i < MAX_TASKS; i++) {
        if (tasks[i].state != TASK_STATE_UNUSED && (int)tasks[i].id == pid)
            return &tasks[i];
    }
    return 0;
}

// ============================================================================
// Spawn — create a new user process from an ELF file
// ============================================================================

extern void mt_print(const char *s);

int sched_spawn(const char *path, char **args, struct fd_entry *fd_overrides) {
    // Disable interrupts during path resolution + ELF loading to prevent
    // preemption from corrupting shared FAT32 buffers and ATA state.
    __asm__ volatile ("cli");

    struct vfs_node *node = vfs_resolve_path(path);
    if (!node || !(node->flags & VFS_FILE)) {
        __asm__ volatile ("sti");
        return -1;
    }

    struct task *t = alloc_task();
    if (!t) { __asm__ volatile ("sti"); return -1; }

    // Create per-process page tables (identity map kernel, user space empty)
    uint64_t *user_pml4 = paging_new_user_space();
    if (!user_pml4) { t->state = TASK_STATE_UNUSED; __asm__ volatile ("sti"); return -1; }
    t->cr3 = (uint64_t)user_pml4;

    // Load ELF into fresh pages mapped in the new address space
    uint64_t entry = 0;
    if (elf_load_into(node, user_pml4, &entry) < 0) {
        paging_free_user_space(user_pml4);
        t->state = TASK_STATE_UNUSED;
        __asm__ volatile ("sti");
        return -1;
    }

    // ELF loaded — safe to re-enable interrupts
    __asm__ volatile ("sti");

    // Allocate kernel stack (identity-mapped, supervisor-only)
    int idx = task_index(t);
    kstacks[idx] = alloc_stack(KSTACK_PAGES);
    if (!kstacks[idx]) {
        paging_free_user_space(user_pml4);
        t->state = TASK_STATE_UNUSED;
        return -1;
    }
    t->kernel_stack_base = (uint64_t)kstacks[idx];
    t->kernel_stack_top = t->kernel_stack_base + KSTACK_SIZE;
    paging_mark_supervisor_region(t->kernel_stack_base, KSTACK_SIZE);

    // Allocate user stack pages and map at USER_STACK virtual address
    uint64_t ustack_vaddr = USER_STACK_TOP - USER_STACK_SIZE;
    uint8_t *ustack_phys[USTACK_PAGES];
    for (int i = 0; i < USTACK_PAGES; i++) {
        ustack_phys[i] = (uint8_t *)pmm_alloc_page();
        if (!ustack_phys[i]) {
            t->state = TASK_STATE_UNUSED;
            return -1;
        }
        mem_zero(ustack_phys[i], 4096);
        paging_map_user_page(user_pml4, ustack_vaddr + i * 4096,
                             (uint64_t)ustack_phys[i],
                             PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER);
    }
    t->user_stack_top = USER_STACK_TOP;

    // --- Write exit stub and argv onto the user stack ---
    // We write to the PHYSICAL pages directly (kernel has identity map access)
    // but all POINTERS we push must be VIRTUAL addresses (what user code sees).

    // Exit stub goes at the very top of the stack
    uint64_t stub_vaddr = USER_STACK_TOP - 32;
    uint64_t stub_pa = paging_virt_to_phys(user_pml4, stub_vaddr);
    uint8_t *stub = (uint8_t *)stub_pa;
    uint32_t sys_exit = SYS_EXIT;
    stub[0] = 0xB8;  // mov eax, imm32
    stub[1] = (uint8_t)(sys_exit & 0xFF);
    stub[2] = (uint8_t)((sys_exit >> 8) & 0xFF);
    stub[3] = (uint8_t)((sys_exit >> 16) & 0xFF);
    stub[4] = (uint8_t)((sys_exit >> 24) & 0xFF);
    stub[5] = 0x31;  // xor edi, edi
    stub[6] = 0xFF;
    stub[7] = 0x0F;  // syscall
    stub[8] = 0x05;
    stub[9] = 0xF4;  // hlt (safety)

    // Build argv on user stack
    int argc = 0;
    if (args) { while (args[argc]) argc++; }
    if (argc == 0) {
        static char *default_args[] = { "prog", 0 };
        args = default_args;
        argc = 1;
    }
    if (argc > 16) argc = 16;

    // sp tracks the virtual address; we translate to physical to write
    uint64_t sp_v = stub_vaddr;

    // Copy argument strings
    uint64_t argv_vptrs[16];
    for (int i = argc - 1; i >= 0; i--) {
        int len = 0;
        while (args[i][len]) len++;
        sp_v -= (len + 1);
        uint64_t pa = paging_virt_to_phys(user_pml4, sp_v);
        mem_copy((void *)pa, args[i], len + 1);
        argv_vptrs[i] = sp_v;
    }

    // Align to 16 bytes
    sp_v &= ~0xFULL;

    // Push NULL terminator for argv
    sp_v -= 8;
    *(uint64_t *)paging_virt_to_phys(user_pml4, sp_v) = 0;

    // Push argv pointers (reverse order)
    for (int i = argc - 1; i >= 0; i--) {
        sp_v -= 8;
        *(uint64_t *)paging_virt_to_phys(user_pml4, sp_v) = argv_vptrs[i];
    }

    uint64_t argv_v = sp_v;  // argv points here

    // ABI alignment: (RSP + 8) % 16 == 0 at function entry
    sp_v -= 8;
    *(uint64_t *)paging_virt_to_phys(user_pml4, sp_v) = stub_vaddr;  // return addr

    // Set up interrupt frame on kernel stack for first iretq
    struct irq_frame_user *frame = (struct irq_frame_user *)
        (t->kernel_stack_top - sizeof(struct irq_frame_user));
    mem_zero(frame, sizeof(*frame));

    frame->base.rip    = entry;
    frame->base.cs     = 0x23;
    frame->base.rflags = 0x202;
    frame->base.rdi    = (uint64_t)argc;
    frame->base.rsi    = argv_v;
    frame->rsp         = sp_v;
    frame->ss          = 0x1B;

    t->rsp   = (uint64_t)frame;
    t->entry = entry;
    t->is_user = 1;
    t->parent_id = current ? (int)current->id : 0;

    // Inherit FD table
    if (fd_overrides) {
        for (int i = 0; i < MAX_FDS; i++)
            t->fd_table[i] = fd_overrides[i];
    }

    // Inherit cwd from parent
    if (current) {
        int k = 0;
        while (current->cwd[k] && k < VFS_MAX_PATH - 1) {
            t->cwd[k] = current->cwd[k];
            k++;
        }
        t->cwd[k] = '\0';
    }

    enqueue(t);
    return (int)t->id;
}

// ============================================================================
// Fork — clone current user task (called from SYS_FORK handler)
// Returns child PID to parent, 0 to child (via IRQ frame RAX).
// ============================================================================

int sched_fork(void) {
    struct task *parent = current;
    if (!parent || !parent->is_user) return -1;

    struct task *child = alloc_task();
    if (!child) return -1;

    // Clone page tables with deep copy of user pages
    uint64_t *child_pml4 = paging_new_user_space();
    if (!child_pml4) { child->state = TASK_STATE_UNUSED; return -1; }
    if (paging_clone_user_pages(child_pml4, (uint64_t *)parent->cr3) < 0) {
        paging_free_user_space(child_pml4);
        child->state = TASK_STATE_UNUSED;
        return -1;
    }
    child->cr3 = (uint64_t)child_pml4;

    // Allocate kernel stack for child
    int idx = task_index(child);
    kstacks[idx] = alloc_stack(KSTACK_PAGES);
    if (!kstacks[idx]) {
        paging_free_user_space(child_pml4);
        child->state = TASK_STATE_UNUSED;
        return -1;
    }
    child->kernel_stack_base = (uint64_t)kstacks[idx];
    child->kernel_stack_top = child->kernel_stack_base + KSTACK_SIZE;
    paging_mark_supervisor_region(child->kernel_stack_base, KSTACK_SIZE);

    // Build an IRQ frame on the child's kernel stack.
    // When the scheduler picks the child, irq_common will pop this frame
    // and iretq to user mode — resuming right after the fork() syscall
    // with RAX = 0.
    struct irq_frame_user *frame = (struct irq_frame_user *)
        (child->kernel_stack_top - sizeof(struct irq_frame_user));
    mem_zero(frame, sizeof(*frame));

    frame->base.rip    = user_ctx_rip;       // resume at instruction after SYSCALL
    frame->base.cs     = 0x23;               // user code segment
    frame->base.rflags = user_ctx_rflags;    // original flags
    frame->base.rax    = 0;                  // fork() returns 0 in child
    frame->base.rbx    = user_ctx_rbx;
    frame->base.rbp    = user_ctx_rbp;
    frame->base.r12    = user_ctx_r12;
    frame->base.r13    = user_ctx_r13;
    frame->base.r14    = user_ctx_r14;
    frame->base.r15    = user_ctx_r15;
    frame->rsp         = user_ctx_rsp;       // same user stack (copied via page clone)
    frame->ss          = 0x1B;               // user data segment

    child->rsp = (uint64_t)frame;
    child->entry = parent->entry;
    child->is_user = 1;
    child->user_stack_top = parent->user_stack_top;
    child->parent_id = (int)parent->id;

    // Copy FD table and cwd
    for (int i = 0; i < MAX_FDS; i++)
        child->fd_table[i] = parent->fd_table[i];
    for (int i = 0; i < VFS_MAX_PATH && parent->cwd[i]; i++)
        child->cwd[i] = parent->cwd[i];

    enqueue(child);

    // Parent gets child PID
    return (int)child->id;
}

// ============================================================================
// Per-process FD table helpers
// ============================================================================

void task_fd_init(struct task *t) {
    for (int i = 0; i < MAX_FDS; i++){
        t->fd_table[i].type = FD_UNUSED;
        t->fd_table[i].node = 0;
        t->fd_table[i].offset = 0;
        t->fd_table[i].flags = 0;
        t->fd_table[i].pipe = 0;
    }
    t->fd_table[0].type = FD_CONSOLE;
    t->fd_table[1].type = FD_CONSOLE;
    t->fd_table[2].type = FD_CONSOLE;

    t->cwd[0] = '/';
    t->cwd[1] = '\0';
}

int task_fd_alloc(struct task *t) {
    for (int i = 3; i < MAX_FDS; i++) {
        if (t->fd_table[i].type == FD_UNUSED) {
            return i;
        }
    }
    return -1;
}

void task_fd_free(struct task *t, int fd) {
    if (fd >= 3 && fd < MAX_FDS) {
        t->fd_table[fd].type = FD_UNUSED;
        t->fd_table[fd].node = 0;
        t->fd_table[fd].offset = 0;
        t->fd_table[fd].flags = 0;
        t->fd_table[fd].pipe = 0;
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
