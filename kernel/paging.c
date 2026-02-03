#include "paging.h"

// Fresh 4 KiB page tables built in kernel .bss so we fully control them.
// Identity-map the first 2 MiB with 4 KiB pages.

#define PT_ENTRIES 512

static uint64_t pml4[PT_ENTRIES] __attribute__((aligned(4096)));
static uint64_t pdpt[PT_ENTRIES] __attribute__((aligned(4096)));
static uint64_t pd[PT_ENTRIES]   __attribute__((aligned(4096)));
static uint64_t pt0[PT_ENTRIES]  __attribute__((aligned(4096)));

void paging_init(void) {
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
    // Below 1MB is supervisor-only (kernel), above 1MB is user-accessible
    for (int i = 0; i < PT_ENTRIES; i++) {
        uint64_t addr = (uint64_t)i * 0x1000;
        uint64_t f = (addr >= 0x100000) ? flags_user : flags_sup;
        pt0[i] = addr | f;
    }

    // Page 0 is supervisor-only (null pointer protection for ring 3)
    pt0[0] = 0 | flags_sup;

    // Protect the pages that contain the paging structures themselves
    uint64_t protect_pages[] = {
        (uint64_t)pml4, (uint64_t)pdpt, (uint64_t)pd, (uint64_t)pt0
    };
    for (int i = 0; i < 4; i++) {
        uint64_t idx = protect_pages[i] >> 12;
        if (idx < PT_ENTRIES) {
            pt0[idx] &= ~PAGE_USER;
            pt0[idx] |= PAGE_PRESENT | PAGE_WRITABLE;
        }
    }

    // Load new page tables
    uint64_t new_cr3 = (uint64_t)pml4;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(new_cr3) : "memory");
}

void paging_mark_user_region(uint64_t addr, uint64_t size) {
    uint64_t start = addr & ~0xFFFULL;
    uint64_t end   = (addr + size + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t a = start; a < end; a += 0x1000) {
        uint64_t idx = a >> 12;
        if (idx < PT_ENTRIES) {
            pt0[idx] |= PAGE_USER | PAGE_PRESENT;
        }
    }
}
