#include "paging.h"
#include "pmm.h"

// Fresh 4 KiB page tables built in kernel .bss so we fully control them.
// Identity-map the first 2 MiB with 4 KiB pages.

#define PT_ENTRIES 512
#define NUM_PT 8  // map first 16 MiB

uint64_t pml4[PT_ENTRIES] __attribute__((aligned(4096)));  // Non-static for debug
uint64_t pdpt[PT_ENTRIES] __attribute__((aligned(4096)));  // Non-static for debug
uint64_t pd[PT_ENTRIES]   __attribute__((aligned(4096)));  // Non-static for debug
uint64_t pt[NUM_PT][PT_ENTRIES] __attribute__((aligned(4096)));  // Non-static for debug

// Keep a pointer to kernel PML4 to copy into user spaces
static uint64_t *kernel_pml4;

void paging_init(void) {
    // Initialize kernel_pml4 pointer
    kernel_pml4 = pml4;

    // Zero tables
    for (int i = 0; i < PT_ENTRIES; i++) {
        pml4[i] = 0;
        pdpt[i] = 0;
        pd[i] = 0;
    }
    for (int p = 0; p < NUM_PT; p++) {
        for (int i = 0; i < PT_ENTRIES; i++) {
            pt[p][i] = 0;
        }
    }

    uint64_t flags_user = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    uint64_t flags_sup  = PAGE_PRESENT | PAGE_WRITABLE;

    // Wire up hierarchy
    pml4[0] = ((uint64_t)pdpt) | flags_user;
    pdpt[0] = ((uint64_t)pd)   | flags_user;
    for (int p = 0; p < NUM_PT; p++) {
        pd[p] = ((uint64_t)pt[p]) | flags_user;
    }

    // Map first 2 MiB with 4 KiB pages
    // Below 1MB is supervisor-only (kernel), above 1MB is user-accessible
    for (int p = 0; p < NUM_PT; p++) {
        for (int i = 0; i < PT_ENTRIES; i++) {
            uint64_t addr = ((uint64_t)p * 0x200000) + ((uint64_t)i * 0x1000);
            uint64_t f = (addr >= 0x100000) ? flags_user : flags_sup;
            pt[p][i] = addr | f;
        }
    }

    // Page 0 is supervisor-only (null pointer protection for ring 3)
    pt[0][0] = 0 | flags_sup;

    // Protect the pages that contain the paging structures themselves
    uint64_t protect_pages[] = {
        (uint64_t)pml4, (uint64_t)pdpt, (uint64_t)pd,
        (uint64_t)&pt[0][0], (uint64_t)&pt[NUM_PT - 1][PT_ENTRIES - 1]
    };
    for (int i = 0; i < 5; i++) {
        uint64_t addr = protect_pages[i];
        uint64_t start = addr & ~0xFFFULL;
        uint64_t end = (addr + 0x1000 + 0xFFFULL) & ~0xFFFULL;
        for (uint64_t a = start; a < end; a += 0x1000) {
            uint64_t pd_idx = a >> 21;
            uint64_t pt_idx = (a >> 12) & 0x1FF;
            if (pd_idx < NUM_PT) {
                pt[pd_idx][pt_idx] &= ~PAGE_USER;
                pt[pd_idx][pt_idx] |= PAGE_PRESENT | PAGE_WRITABLE;
            }
        }
    }

    // Load new page tables
    uint64_t new_cr3 = (uint64_t)pml4;
    __asm__ volatile ("mov %0, %%cr3" : : "r"(new_cr3) : "memory");
}

static inline void invlpg(uint64_t addr) {
    __asm__ volatile ("invlpg (%0)" : : "r"(addr) : "memory");
}

void paging_mark_user_region(uint64_t addr, uint64_t size) {
    uint64_t start = addr & ~0xFFFULL;
    uint64_t end   = (addr + size + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t a = start; a < end; a += 0x1000) {
        uint64_t pd_idx = a >> 21;
        uint64_t pt_idx = (a >> 12) & 0x1FF;
        if (pd_idx < NUM_PT) {
            pt[pd_idx][pt_idx] |= PAGE_USER | PAGE_PRESENT;
            invlpg(a);  // Flush TLB for this page
        }
    }
}

void paging_mark_supervisor_region(uint64_t addr, uint64_t size) {
    uint64_t start = addr & ~0xFFFULL;
    uint64_t end   = (addr + size + 0xFFFULL) & ~0xFFFULL;
    for (uint64_t a = start; a < end; a += 0x1000) {
        uint64_t pd_idx = a >> 21;
        uint64_t pt_idx = (a >> 12) & 0x1FF;
        if (pd_idx < NUM_PT) {
            pt[pd_idx][pt_idx] &= ~PAGE_USER;
            pt[pd_idx][pt_idx] |= PAGE_PRESENT | PAGE_WRITABLE;
            invlpg(a);  // Flush TLB for this page
        }
    }
}

// ---------------------------------------------------------------------------
// Helpers for per-task address spaces (identity mapping for now)
// ---------------------------------------------------------------------------

// Allocate and zero a new page table level
static uint64_t *alloc_pt_page(void) {
    extern void *pmm_alloc_page(void);
    uint64_t *page = (uint64_t *)pmm_alloc_page();
    if (!page) return 0;
    for (int i = 0; i < PT_ENTRIES; i++) page[i] = 0;
    return page;
}

uint64_t *paging_new_user_space(void) {
    uint64_t *new_pml4 = alloc_pt_page();
    uint64_t *new_pdpt = alloc_pt_page();
    uint64_t *new_pd   = alloc_pt_page();
    if (!new_pml4 || !new_pdpt || !new_pd) {
        return 0;
    }

    uint64_t flags_user = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    uint64_t flags_sup  = PAGE_PRESENT | PAGE_WRITABLE;

    new_pml4[0] = ((uint64_t)new_pdpt) | flags_user;
    new_pdpt[0] = ((uint64_t)new_pd)   | flags_user;

    uint64_t new_pt_addrs[NUM_PT];
    for (int p = 0; p < NUM_PT; p++) {
        uint64_t *new_pt = alloc_pt_page();
        if (!new_pt) return 0;
        new_pt_addrs[p] = (uint64_t)new_pt;
        new_pd[p] = new_pt_addrs[p] | flags_user;
        for (int i = 0; i < PT_ENTRIES; i++) {
            uint64_t addr = ((uint64_t)p * 0x200000) + ((uint64_t)i * 0x1000);
            uint64_t f = (addr >= 0x100000) ? flags_user : flags_sup;
            new_pt[i] = addr | f;
        }
    }

    // Protect the page tables themselves from user access
    uint64_t protect_pages[] = {
        (uint64_t)new_pml4, (uint64_t)new_pdpt, (uint64_t)new_pd,
        new_pt_addrs[0], new_pt_addrs[NUM_PT - 1]
    };
    for (int i = 0; i < 5; i++) {
        uint64_t addr = protect_pages[i];
        uint64_t start = addr & ~0xFFFULL;
        uint64_t end = (addr + 0x1000 + 0xFFFULL) & ~0xFFFULL;
        for (uint64_t a = start; a < end; a += 0x1000) {
            uint64_t pd_idx = a >> 21;
            uint64_t pt_idx = (a >> 12) & 0x1FF;
            if (pd_idx < NUM_PT) {
                uint64_t *pt_l = (uint64_t *)new_pt_addrs[pd_idx];
                pt_l[pt_idx] &= ~PAGE_USER;
                pt_l[pt_idx] |= PAGE_PRESENT | PAGE_WRITABLE;
            }
        }
    }

    return new_pml4;
}

int paging_map_page(uint64_t *pml4, uint64_t addr, uint64_t flags) {
    uint64_t pml4_idx = (addr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (addr >> 30) & 0x1FF;
    uint64_t pd_idx   = (addr >> 21) & 0x1FF;
    uint64_t pt_idx   = (addr >> 12) & 0x1FF;

    uint64_t *pdpt_l = (uint64_t *)(pml4[pml4_idx] & ~0xFFFULL);
    if (!pdpt_l) return -1;
    uint64_t *pd_l = (uint64_t *)(pdpt_l[pdpt_idx] & ~0xFFFULL);
    if (!pd_l) return -1;
    uint64_t *pt_l = (uint64_t *)(pd_l[pd_idx] & ~0xFFFULL);
    if (!pt_l) return -1;

    pt_l[pt_idx] = (addr & ~0xFFFULL) | flags | PAGE_PRESENT;
    return 0;
}
