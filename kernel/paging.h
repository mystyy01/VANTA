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

// OS-defined bit: marks pages allocated for user processes (for cleanup)
#define PAGE_USERALLOC  (1ULL << 9)

// User virtual address layout (above identity-mapped kernel region)
#define USER_VADDR_BASE   0x1000000ULL   // 16 MB - user code starts here
#define USER_STACK_TOP    0x1200000ULL   // 18 MB - user stack top (grows down)
#define USER_STACK_SIZE   (16 * 1024)    // 16 KB

// Initialize paging with user-accessible memory for the bootstrap kernel
void paging_init(void);

// Mark an identity-mapped region as user-accessible (bootstrap tables only)
void paging_mark_user_region(uint64_t addr, uint64_t size);
// Mark an identity-mapped region as supervisor-only (bootstrap tables only)
void paging_mark_supervisor_region(uint64_t addr, uint64_t size);

// Create a fresh page table hierarchy with kernel identity map (supervisor-only).
// User regions are left unmapped — use paging_map_user_page() to populate.
// Returns pointer to PML4 (physical address). Returns 0 on failure.
uint64_t *paging_new_user_space(void);

// Map a single 4 KiB page (identity VA==PA) with flags into given PML4.
int paging_map_page(uint64_t *pml4, uint64_t addr, uint64_t flags);

// Map vaddr → paddr in the given page table. Creates intermediate entries
// as needed. Sets PAGE_USERALLOC on the leaf entry for cleanup tracking.
int paging_map_user_page(uint64_t *pml4, uint64_t vaddr, uint64_t paddr, uint64_t flags);

// Translate virtual address to physical address using given page tables.
// Returns physical address or 0 if not mapped.
uint64_t paging_virt_to_phys(uint64_t *pml4, uint64_t vaddr);

// Free all user-allocated pages (PAGE_USERALLOC) and the page table structure.
// Does NOT free identity-mapped kernel pages.
void paging_free_user_space(uint64_t *user_pml4);

// Deep-copy all user-allocated page mappings from src to dst.
// Allocates new physical pages and copies contents. For fork().
int paging_clone_user_pages(uint64_t *dst_pml4, uint64_t *src_pml4);

// Return pointer to the kernel's global PML4.
uint64_t *paging_kernel_pml4(void);

#endif
