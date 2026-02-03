#include "paging.h"

// Fresh 4 KiB page tables built in kernel .bss so we fully control them.
// Identity-map the first 2 MiB with 4 KiB pages. The four pages that hold
// the paging structures themselves are supervisor-only; the rest are
// user-accessible so ring 3 code can run.

#define PT_ENTRIES 512

static uint64_t pml4[PT_ENTRIES] __attribute__((aligned(4096)));
static uint64_t pdpt[PT_ENTRIES] __attribute__((aligned(4096)));
static uint64_t pd[PT_ENTRIES]   __attribute__((aligned(4096)));
static uint64_t pt0[PT_ENTRIES]  __attribute__((aligned(4096)));

// VGA for debug
static volatile unsigned short *vga = (volatile unsigned short *)0xB8000;

void paging_init(void) {
    // Debug: show we're here
    vga[79] = (0x0E << 8) | 'P';  // Yellow 'P' at end of first line

    // Zero tables
    for (int i = 0; i < PT_ENTRIES; i++) {
        pml4[i] = 0;
        pdpt[i] = 0;
        pd[i] = 0;
        pt0[i] = 0;
    }

    uint64_t flags_user = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    uint64_t flags_sup  = PAGE_PRESENT | PAGE_WRITABLE;

    // Wire up hierarchy
    pml4[0] = ((uint64_t)pdpt) | flags_user;
    pdpt[0] = ((uint64_t)pd)   | flags_user;
    pd[0]   = ((uint64_t)pt0)  | flags_user;

    // Map first 2 MiB with 4 KiB pages
    for (int i = 0; i < PT_ENTRIES; i++) {
        uint64_t addr = (uint64_t)i * 0x1000;
        uint64_t f = (addr >= 0x100000) ? flags_user : flags_sup;
        pt0[i] = addr | f;
    }

    // Map page 0 for kernel (supervisor). User bit remains clear so ring3
    // null derefs still fault, but kernel null accesses won't crash the system.
    pt0[0] = 0 | flags_sup;

    // Protect the pages that contain the paging structures themselves
    uint64_t protect_pages[] = {
        (uint64_t)pml4, (uint64_t)pdpt, (uint64_t)pd, (uint64_t)pt0
    };
    for (int i = 0; i < 4; i++) {
        uint64_t idx = protect_pages[i] >> 12;
        pt0[idx] &= ~PAGE_USER;
        pt0[idx] |= PAGE_PRESENT | PAGE_WRITABLE; // ensure present
    }

    // Load new page tables
    uint64_t new_cr3 = (uint64_t)pml4;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(new_cr3) : "memory");

    // Debug: show we finished
    vga[78] = (0x0A << 8) | 'K';  // Green 'K' = OK
}

void paging_mark_user_region(uint64_t addr, uint64_t size) {
    // Identity-mapped only; align to pages
    uint64_t start = addr & ~0xFFFULL;
    uint64_t end   = (addr + size + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t a = start; a < end; a += 0x1000) {
        uint64_t idx = a >> 12;
        if (idx < PT_ENTRIES) {
            pt0[idx] |= PAGE_USER | PAGE_PRESENT;
        }
    }
}
