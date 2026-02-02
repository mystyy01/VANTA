#include "elf_loader.h"

// Minimal ELF64 loader for VANTA OS.
// Assumptions:
// - Flat physical addressing; we copy segments to their p_paddr.
// - No paging; kernel and shell share address space, so caller must ensure
//   binaries are placed in a safe region (we pick a fixed load window).
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
static uint8_t elf_file_buf[ELF_MAX_SIZE];

// Execution stack for loaded program (16 KB)
static uint8_t elf_stack[16 * 1024] __attribute__((aligned(16)));

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
    const uint8_t *base = (const uint8_t *)eh;
    const uint8_t *ph_base = base + eh->e_phoff;

    for (uint16_t i = 0; i < eh->e_phnum; i++) {
        const Elf64_Phdr *ph = (const Elf64_Phdr *)(ph_base + i * eh->e_phentsize);
        if (ph->p_type != PT_LOAD) continue;

        // We treat p_paddr as the destination (flat physical address).
        uint8_t *dst = (uint8_t *)(uintptr_t)ph->p_paddr;
        const uint8_t *src = base + ph->p_offset;

        // Copy file-backed portion
        if (ph->p_filesz > 0) {
            memcpy_local(dst, src, (uint32_t)ph->p_filesz);
        }

        // Zero bss part
        if (ph->p_memsz > ph->p_filesz) {
            uint64_t diff = ph->p_memsz - ph->p_filesz;
            memset_local(dst + ph->p_filesz, 0, (uint32_t)diff);
        }
    }
    return 0;
}

// Execute loaded image: create a function pointer to entry and call it.
static int jump_to_entry(uint64_t entry, char **args) {
    // Prepare stack: place argv pointer array and a null terminator.
    // We keep it minimal: argv[0] = program name, argv[1] = 0.
    uint8_t *sp = elf_stack + sizeof(elf_stack);
    sp -= sizeof(char *); // argv[1] = 0
    *((char **)sp) = 0;
    sp -= sizeof(char *); // argv[0]
    *((char **)sp) = (char *)"prog";
    sp -= sizeof(int); // argc
    *((int *)sp) = 1;

    // Align stack to 16 bytes for ABI compliance
    uintptr_t sp_align = (uintptr_t)sp & ~0xF;
    sp = (uint8_t *)sp_align;

    typedef int (*entry_fn)(int, char **);
    entry_fn fn = (entry_fn)(uintptr_t)entry;
    return fn(1, (char **)(sp + sizeof(int))); // argc=1, argv after argc
}

int elf_execute(struct vfs_node *node, char **args) {
    (void)args; // args unused for now
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

    load_segments(eh);
    int ret = jump_to_entry(eh->e_entry, args);
    return ret;
}
