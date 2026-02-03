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

// Initialize paging with user-accessible memory
void paging_init(void);

// Mark an identity-mapped region as user-accessible (sets the U bit on
// the corresponding 4 KiB pages). Size rounds up to whole pages.
void paging_mark_user_region(uint64_t addr, uint64_t size);

#endif
