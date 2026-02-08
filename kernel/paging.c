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

    // Intermediate table entries need USER bit so user-mapped pages deeper
    // in the hierarchy are reachable.  The leaf PT entries for the identity
    // map are supervisor-only, which is what actually blocks user access.
    uint64_t flags_hier = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;
    uint64_t flags_sup  = PAGE_PRESENT | PAGE_WRITABLE;

    new_pml4[0] = ((uint64_t)new_pdpt) | flags_hier;
    new_pdpt[0] = ((uint64_t)new_pd)   | flags_hier;

    for (int p = 0; p < NUM_PT; p++) {
        uint64_t *new_pt = alloc_pt_page();
        if (!new_pt) return 0;
        new_pd[p] = (uint64_t)new_pt | flags_hier;
        for (int i = 0; i < PT_ENTRIES; i++) {
            uint64_t addr = ((uint64_t)p * 0x200000) + ((uint64_t)i * 0x1000);
            // All identity-mapped pages are supervisor-only.
            // User code gets access only via paging_map_user_page().
            new_pt[i] = addr | flags_sup;
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

// ---------------------------------------------------------------------------
// Per-process virtual memory helpers
// ---------------------------------------------------------------------------

static void memcpy_pg(void *dst, const void *src, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    const uint8_t *s = (const uint8_t *)src;
    while (n--) *d++ = *s++;
}

static void memset_pg(void *dst, int val, uint64_t n) {
    uint8_t *d = (uint8_t *)dst;
    while (n--) *d++ = (uint8_t)val;
}

int paging_map_user_page(uint64_t *target_pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags) {
    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    uint64_t pdpt_idx = (vaddr >> 30) & 0x1FF;
    uint64_t pd_idx   = (vaddr >> 21) & 0x1FF;
    uint64_t pt_idx   = (vaddr >> 12) & 0x1FF;

    uint64_t hier = PAGE_PRESENT | PAGE_WRITABLE | PAGE_USER;

    // Get or create PDPT
    uint64_t *pdpt_l;
    if (!(target_pml4[pml4_idx] & PAGE_PRESENT)) {
        pdpt_l = alloc_pt_page();
        if (!pdpt_l) return -1;
        target_pml4[pml4_idx] = (uint64_t)pdpt_l | hier;
    } else {
        pdpt_l = (uint64_t *)(target_pml4[pml4_idx] & ~0xFFFULL);
    }

    // Get or create PD
    uint64_t *pd_l;
    if (!(pdpt_l[pdpt_idx] & PAGE_PRESENT)) {
        pd_l = alloc_pt_page();
        if (!pd_l) return -1;
        pdpt_l[pdpt_idx] = (uint64_t)pd_l | hier;
    } else {
        pd_l = (uint64_t *)(pdpt_l[pdpt_idx] & ~0xFFFULL);
    }

    // Get or create PT
    uint64_t *pt_l;
    if (!(pd_l[pd_idx] & PAGE_PRESENT)) {
        pt_l = alloc_pt_page();
        if (!pt_l) return -1;
        pd_l[pd_idx] = (uint64_t)pt_l | hier;
    } else {
        pt_l = (uint64_t *)(pd_l[pd_idx] & ~0xFFFULL);
    }

    // Map the page, marking it as user-allocated for later cleanup
    pt_l[pt_idx] = (paddr & ~0xFFFULL) | flags | PAGE_PRESENT | PAGE_USERALLOC;
    invlpg(vaddr);
    return 0;
}

uint64_t paging_virt_to_phys(uint64_t *target_pml4, uint64_t vaddr) {
    uint64_t pml4_idx = (vaddr >> 39) & 0x1FF;
    if (!(target_pml4[pml4_idx] & PAGE_PRESENT)) return 0;
    uint64_t *pdpt_l = (uint64_t *)(target_pml4[pml4_idx] & ~0xFFFULL);

    uint64_t pdpt_idx = (vaddr >> 30) & 0x1FF;
    if (!(pdpt_l[pdpt_idx] & PAGE_PRESENT)) return 0;
    uint64_t *pd_l = (uint64_t *)(pdpt_l[pdpt_idx] & ~0xFFFULL);

    uint64_t pd_idx = (vaddr >> 21) & 0x1FF;
    if (!(pd_l[pd_idx] & PAGE_PRESENT)) return 0;
    uint64_t *pt_l = (uint64_t *)(pd_l[pd_idx] & ~0xFFFULL);

    uint64_t pt_idx = (vaddr >> 12) & 0x1FF;
    if (!(pt_l[pt_idx] & PAGE_PRESENT)) return 0;

    return (pt_l[pt_idx] & ~0xFFFULL) | (vaddr & 0xFFF);
}

void paging_free_user_space(uint64_t *user_pml4) {
    if (!user_pml4) return;

    // Walk all four levels.  Free any leaf page that carries PAGE_USERALLOC
    // (these are pages we allocated for user code/data/stack).
    // Also free ALL intermediate page table pages (PML4, PDPT, PD, PT)
    // since they were all allocated from PMM per-process.
    for (int i = 0; i < PT_ENTRIES; i++) {
        if (!(user_pml4[i] & PAGE_PRESENT)) continue;
        uint64_t *pdpt_l = (uint64_t *)(user_pml4[i] & ~0xFFFULL);

        for (int j = 0; j < PT_ENTRIES; j++) {
            if (!(pdpt_l[j] & PAGE_PRESENT)) continue;
            uint64_t *pd_l = (uint64_t *)(pdpt_l[j] & ~0xFFFULL);

            for (int k = 0; k < PT_ENTRIES; k++) {
                if (!(pd_l[k] & PAGE_PRESENT)) continue;
                uint64_t *pt_l = (uint64_t *)(pd_l[k] & ~0xFFFULL);

                // Free user-allocated leaf pages (code, data, stack)
                for (int l = 0; l < PT_ENTRIES; l++) {
                    if ((pt_l[l] & PAGE_PRESENT) && (pt_l[l] & PAGE_USERALLOC)) {
                        pmm_free_page((void *)(pt_l[l] & ~0xFFFULL));
                        pt_l[l] = 0;
                    }
                }

                // Free the PT page itself (allocated per-process)
                pmm_free_page(pt_l);
            }

            // Free PD page
            pmm_free_page(pd_l);
        }

        // Free PDPT page
        pmm_free_page(pdpt_l);
    }

    // Free PML4 itself
    pmm_free_page(user_pml4);
}

int paging_clone_user_pages(uint64_t *dst_pml4, uint64_t *src_pml4) {
    // Walk src page tables.  For every leaf entry with PAGE_USERALLOC,
    // allocate a fresh physical page, copy contents, and map it at the
    // same virtual address in dst.
    for (int i = 0; i < PT_ENTRIES; i++) {
        if (!(src_pml4[i] & PAGE_PRESENT)) continue;
        uint64_t *pdpt_s = (uint64_t *)(src_pml4[i] & ~0xFFFULL);

        for (int j = 0; j < PT_ENTRIES; j++) {
            if (!(pdpt_s[j] & PAGE_PRESENT)) continue;
            uint64_t *pd_s = (uint64_t *)(pdpt_s[j] & ~0xFFFULL);

            for (int k = 0; k < PT_ENTRIES; k++) {
                if (!(pd_s[k] & PAGE_PRESENT)) continue;
                uint64_t *pt_s = (uint64_t *)(pd_s[k] & ~0xFFFULL);

                for (int l = 0; l < PT_ENTRIES; l++) {
                    if (!(pt_s[l] & PAGE_PRESENT)) continue;
                    if (!(pt_s[l] & PAGE_USERALLOC)) continue;

                    uint64_t src_pa = pt_s[l] & ~0xFFFULL;
                    uint64_t flags  = pt_s[l] & 0xFFFULL;

                    // Reconstruct the virtual address from indices
                    uint64_t vaddr = ((uint64_t)i << 39) | ((uint64_t)j << 30) |
                                     ((uint64_t)k << 21) | ((uint64_t)l << 12);

                    // Allocate fresh page and copy
                    void *new_page = pmm_alloc_page();
                    if (!new_page) return -1;
                    memcpy_pg(new_page, (void *)src_pa, 4096);

                    // Map in destination
                    if (paging_map_user_page(dst_pml4, vaddr, (uint64_t)new_page, flags) < 0)
                        return -1;
                }
            }
        }
    }
    return 0;
}

uint64_t *paging_kernel_pml4(void) {
    return kernel_pml4;
}
