#ifndef ISR_H
#define ISR_H

#include <stdint.h>

void isr_handler(uint64_t int_no);
void irq_handler(uint64_t int_no);

#endif
