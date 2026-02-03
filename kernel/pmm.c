#include "pmm.h"

// Simple bitmap-based physical page allocator.
// Assumes a contiguous usable range; no NUMA; no highmem.

#define MAX_PAGES (1024 * 1024)  // supports up to 4 GiB with 4 KiB pages

static uint64_t bitmap[MAX_PAGES / 64];
static uint64_t page_base = 0;
static uint64_t total_pages = 0;

static inline void set_bit(uint64_t idx) { bitmap[idx >> 6] |= (1ULL << (idx & 63)); }
static inline void clear_bit(uint64_t idx) { bitmap[idx >> 6] &= ~(1ULL << (idx & 63)); }
static inline int test_bit(uint64_t idx) { return (bitmap[idx >> 6] >> (idx & 63)) & 1; }

void pmm_init(uint64_t start, uint64_t end) {
    // Align to pages
    start = (start + PMM_PAGE_SIZE - 1) & ~(PMM_PAGE_SIZE - 1);
    end   = end & ~(PMM_PAGE_SIZE - 1);
    if (end <= start) return;

    page_base = start;
    total_pages = (end - start) / PMM_PAGE_SIZE;
    if (total_pages > MAX_PAGES) total_pages = MAX_PAGES;

    // Mark all free initially
    for (uint64_t i = 0; i < (total_pages + 63) / 64; i++) {
        bitmap[i] = 0;
    }
}

void *pmm_alloc_page(void) {
    for (uint64_t i = 0; i < total_pages; i++) {
        if (!test_bit(i)) {
            set_bit(i);
            return (void *)(page_base + i * PMM_PAGE_SIZE);
        }
    }
    return 0;
}

void pmm_free_page(void *page) {
    uint64_t addr = (uint64_t)page;
    if (addr < page_base) return;
    uint64_t idx = (addr - page_base) / PMM_PAGE_SIZE;
    if (idx >= total_pages) return;
    clear_bit(idx);
}
