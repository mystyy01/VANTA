#ifndef GDT_H
#define GDT_H

#include <stdint.h>

// Initialize GDT with TSS for ring 3 support
void gdt_init(void);

// Set RSP0 in TSS (kernel stack for ring 3 -> ring 0 transitions)
void tss_set_rsp0(uint64_t rsp0);

#endif
