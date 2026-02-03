#ifndef PMM_H
#define PMM_H

#include <stdint.h>

#define PMM_PAGE_SIZE 4096

// Initialize the physical memory manager over [start, end).
// Addresses must be physical and page-aligned.
void pmm_init(uint64_t start, uint64_t end);

// Allocate a single 4 KiB page. Returns physical address or 0 on exhaustion.
void *pmm_alloc_page(void);

// Free a previously allocated page (physical address).
void pmm_free_page(void *page);

#endif
