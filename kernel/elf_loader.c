#include "elf_loader.h"
#include "syscall.h"
#include "paging.h"

// Minimal ELF64 loader for PHOBOS with user-mode execution.
// Assumptions:
// - Flat physical addressing; we copy segments to their p_paddr.
// - User programs run in ring 3 via iretq; syscalls return via SYS_EXIT.
// - No dynamic linking; only ET_EXEC static binaries are supported.

// Simple local helpers (freestanding, no libc)
static void *memcpy_local(void *dst, const void *src, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
    return dst;
}

static void *memset_local(void *dst, int val, uint32_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)val;
    return dst;
}

// ============================================================================
// User mode support: setjmp/longjmp style context for returning from user mode
// ============================================================================

// Saved kernel context (callee-saved registers + stack)
static uint64_t saved_rbx;
static uint64_t saved_rbp;
static uint64_t saved_r12;
static uint64_t saved_r13;
static uint64_t saved_r14;
static uint64_t saved_r15;
static uint64_t saved_rsp;
static uint64_t saved_rip;

// Exit code from user program
static volatile int user_exit_code;

// Called by SYS_EXIT syscall handler to return to kernel
void kernel_return_from_user(int exit_code) {
    user_exit_code = exit_code;

    // Restore callee-saved registers and jump back to saved return point
    __asm__ volatile (
        "mov %0, %%rbx\n"
        "mov %1, %%rbp\n"
        "mov %2, %%r12\n"
        "mov %3, %%r13\n"
        "mov %4, %%r14\n"
        "mov %5, %%r15\n"
        "mov %6, %%rsp\n"
        "jmp *%7\n"
        :
        : "m"(saved_rbx), "m"(saved_rbp), "m"(saved_r12), "m"(saved_r13),
          "m"(saved_r14), "m"(saved_r15), "m"(saved_rsp), "m"(saved_rip)
        : "memory"
    );
    __builtin_unreachable();
}

// ELF definitions (subset)
typedef struct {
    uint8_t  e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
} Elf64_Ehdr;

typedef struct {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
} Elf64_Phdr;

#define EI_MAG0 0
#define EI_MAG1 1
#define EI_MAG2 2
#define EI_MAG3 3
#define EI_CLASS 4
#define EI_DATA 5
#define EI_VERSION 6

#define ELFMAG0 0x7f
#define ELFMAG1 'E'
#define ELFMAG2 'L'
#define ELFMAG3 'F'

#define ELFCLASS64 2
#define ELFDATA2LSB 1

#define ET_EXEC 2
#define EM_X86_64 62

#define PT_LOAD 1

// Loader workspace: a fixed 512 KB staging buffer in .bss to read the file
// before mapping segments. This limits executable size accordingly.
#define ELF_MAX_SIZE (512 * 1024)
// Keep large buffers in .lbss so they don't overlap scheduler state.
static uint8_t elf_file_buf[ELF_MAX_SIZE] __attribute__((section(".lbss")));

// Execution stack for loaded program (16 KB)
static uint8_t elf_stack[16 * 1024] __attribute__((aligned(16), section(".lbss")));

// Basic mt-shell print hook (declared in lib.c)
extern void mt_print(const char *s);
extern void print_int(int n);

static void print_str(const char *s) { mt_print(s); }

// Validate ELF header
static int validate_header(const Elf64_Ehdr *eh) {
    if (eh->e_ident[EI_MAG0] != ELFMAG0 ||
        eh->e_ident[EI_MAG1] != ELFMAG1 ||
        eh->e_ident[EI_MAG2] != ELFMAG2 ||
        eh->e_ident[EI_MAG3] != ELFMAG3) {
        return -1; // Bad magic
    }
    if (eh->e_ident[EI_CLASS] != ELFCLASS64) return -2; // Not 64-bit
    if (eh->e_ident[EI_DATA] != ELFDATA2LSB) return -3; // Not little endian
    if (eh->e_type != ET_EXEC) return -4; // Only ET_EXEC
    if (eh->e_machine != EM_X86_64) return -5; // Wrong arch
    return 0;
}

// Load PT_LOAD segments into memory
static int load_segments(const Elf64_Ehdr *eh) {
    const uint64_t USER_LOAD_MIN = 0x00200000;  // keep away from kernel/BSS
    const uint64_t USER_LOAD_MAX = 0x01000000;  // 16 MiB identity-mapped
    const uint8_t *base = (const uint8_t *)eh;
    const uint8_t *ph_base = base + eh->e_phoff;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(ph_base + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;

        // We treat p_paddr as the destination (flat physical address).
        uint8_t *dst = (uint8_t *)(uintptr_t)ph->p_paddr;
        const uint8_t *src = base + ph->p_offset;

        // Basic bounds check to prevent overwriting kernel memory.
        if (ph->p_paddr < USER_LOAD_MIN) return -20;
        if (ph->p_memsz == 0) return -21;
        if ((ph->p_paddr + ph->p_memsz) > USER_LOAD_MAX) return -22;

        // Copy file-backed portion
        if (ph->p_filesz > 0) {
            memcpy_local(dst, src, (uint32_t)ph->p_filesz);
        }

        // Zero bss part
        if (ph->p_memsz > ph->p_filesz) {
            uint64_t diff = ph->p_memsz - ph->p_filesz;
            memset_local(dst + ph->p_filesz, 0, (uint32_t)diff);
        }

        // Mark segment pages as user-accessible
        paging_mark_user_region((uint64_t)dst, ph->p_memsz);
    }
    return 0;
}

// Temporary storage for iret parameters (avoids register pressure in inline asm)
static uint64_t iret_sp;
static uint64_t iret_entry;
static uint64_t iret_argc;
static uint64_t iret_argv;

// Execute loaded image by entering ring 3 (user mode) via iret.
// Returns when the program calls SYS_EXIT.
static int jump_to_entry(uint64_t entry, char **args) {
    // Count argc from null-terminated args array
    int argc = 0;
    if (args) {
        while (args[argc]) argc++;
    }
    if (argc == 0) {
        // Fallback: at least provide a program name
        static char *default_args[] = { "prog", 0 };
        args = default_args;
        argc = 1;
    }

    // Prepare user stack
    uint8_t *sp = elf_stack + sizeof(elf_stack);

    // Copy argument strings onto user stack and collect their user pointers
    // Limit argc to a small safe number to avoid overflow
    if (argc > 32) argc = 32;
    char *argv_ptrs[32];
    for (int i = argc - 1; i >= 0; i--) {
        int len = 0;
        while (args[i][len]) len++;
        // include null terminator
        sp -= (len + 1);
        memcpy_local(sp, args[i], (uint32_t)(len + 1));
        argv_ptrs[i] = (char *)sp;
    }

    // Align stack to 16 bytes for ABI compliance
    sp = (uint8_t *)((uintptr_t)sp & ~0xF);

    // Push null terminator for argv
    sp -= sizeof(char *);
    *((char **)sp) = 0;

    // Push argv pointers in reverse order
    for (int i = argc - 1; i >= 0; i--) {
        sp -= sizeof(char *);
        *((char **)sp) = argv_ptrs[i];
    }

    char **argv_ptr = (char **)sp;

    // Additional 8-byte adjustment: System V ABI requires (RSP + 8) % 16 == 0
    // at function entry because 'call' pushes 8-byte return address.
    // Since we're using iret (no call), we simulate it.
    sp -= 8;

    user_exit_code = -1;  // Default if something goes wrong

    // Make stack pages user-accessible
    paging_mark_user_region((uint64_t)elf_stack, sizeof(elf_stack));

    // Store parameters in static variables to reduce register pressure
    iret_sp = (uint64_t)sp;
    iret_entry = entry;
    iret_argc = (uint64_t)argc;
    iret_argv = (uint64_t)argv_ptr;

    // Step 1: Save kernel context (callee-saved registers)
    __asm__ volatile (
        "mov %%rbx, %0\n"
        "mov %%rbp, %1\n"
        "mov %%r12, %2\n"
        "mov %%r13, %3\n"
        : "=m"(saved_rbx), "=m"(saved_rbp), "=m"(saved_r12), "=m"(saved_r13)
        :
        : "memory"
    );

    __asm__ volatile (
        "mov %%r14, %0\n"
        "mov %%r15, %1\n"
        "mov %%rsp, %2\n"
        : "=m"(saved_r14), "=m"(saved_r15), "=m"(saved_rsp)
        :
        : "memory"
    );

    // Step 2: Save return address and enter user mode
    __asm__ volatile (
        // Save address of return point
        "lea 1f(%%rip), %%rax\n"
        "mov %%rax, %0\n"

        // Load parameters into registers
        "mov %1, %%r8\n"           // user stack
        "mov %2, %%r9\n"           // entry point
        "mov %3, %%rdi\n"          // argc
        "mov %4, %%rsi\n"          // argv

        // Push iret frame
        "push $0x1B\n"             // SS = user data | ring 3
        "push %%r8\n"              // RSP = user stack
        "push $0x202\n"            // RFLAGS = IF enabled
        "push $0x23\n"             // CS = user code | ring 3
        "push %%r9\n"              // RIP = entry point

        // Clear other registers
        "xor %%rax, %%rax\n"
        "xor %%rdx, %%rdx\n"
        "xor %%rcx, %%rcx\n"
        "xor %%r8, %%r8\n"
        "xor %%r9, %%r9\n"
        "xor %%r10, %%r10\n"
        "xor %%r11, %%r11\n"

        // Enter user mode
        "iretq\n"

        // Return point from kernel_return_from_user()
        "1:\n"
        // Re-enable interrupts (disabled by syscall instruction)
        "sti\n"

        : "=m"(saved_rip)
        : "m"(iret_sp), "m"(iret_entry), "m"(iret_argc), "m"(iret_argv)
        : "memory", "rax", "rcx", "rdx", "rdi", "rsi",
          "r8", "r9", "r10", "r11"
    );

    // We reach here when kernel_return_from_user() restores context
    return user_exit_code;
}

int elf_execute(struct vfs_node *node, char **args) {
    uint64_t entry = 0;
    int ret = elf_load(node, &entry);
    if (ret < 0) return ret;
    ret = jump_to_entry(entry, args);
    return ret;
}

int elf_load(struct vfs_node *node, uint64_t *entry_out) {
    if (!node || !(node->flags & VFS_FILE)) return -10;

    if (node->size > ELF_MAX_SIZE) {
        print_str("exec: file too large\n");
        return -11;
    }

    int read = vfs_read(node, 0, node->size, elf_file_buf);
    if (read < 0 || (uint32_t)read < node->size) {
        print_str("exec: read failed\n");
        return -12;
    }

    Elf64_Ehdr *eh = (Elf64_Ehdr *)elf_file_buf;
    int hv = validate_header(eh);
    if (hv != 0) {
        print_str("exec: invalid ELF\n");
        print_int(hv);
        print_str("\n");
        return hv;
    }

    if (load_segments(eh) < 0) {
        print_str("exec: bad segment\n");
        return -13;
    }
    if (entry_out) *entry_out = eh->e_entry;
    return 0;
}
