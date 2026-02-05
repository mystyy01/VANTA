#ifndef PAGING_H
#define PAGING_H

#include <stdint.h>

// Page table entry flags (64-bit)
#define PAGE_PRESENT    (1ULL << 0)
#define PAGE_WRITABLE   (1ULL << 1)
#define PAGE_USER       (1ULL << 2)
#define PAGE_PWT        (1ULL << 3)
#define PAGE_PCD        (1ULL << 4)
#define PAGE_ACCESSED   (1ULL << 5)
#define PAGE_DIRTY      (1ULL << 6)
#define PAGE_PSE        (1ULL << 7)   // 2MB page if set in PDE
#define PAGE_GLOBAL     (1ULL << 8)
#define PAGE_PAT        (1ULL << 7)   // For 4KB PTEs, PAT shares bit 7

// Initialize paging with user-accessible memory for the bootstrap kernel
void paging_init(void);

// Mark an identity-mapped region as user-accessible (bootstrap tables only)
void paging_mark_user_region(uint64_t addr, uint64_t size);
// Mark an identity-mapped region as supervisor-only (bootstrap tables only)
void paging_mark_supervisor_region(uint64_t addr, uint64_t size);

// Create a fresh user page table hierarchy with kernel identity maps copied.
// Returns pointer to PML4 (physical address). Returns 0 on failure.
uint64_t *paging_new_user_space(void);

// Map a single 4 KiB page (identity VA==PA) with flags into given PML4.
int paging_map_page(uint64_t *pml4, uint64_t addr, uint64_t flags);

#endif
