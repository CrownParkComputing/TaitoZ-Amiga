/* cc_dyntrans_hw.h -- helper/log surface for the future Chase HQ dyntrans
 * backend. The translated CPU path can record ordered hardware writes here and
 * use the reference bus helpers for reads. */
#ifndef CC_DYNTRANS_HW_H
#define CC_DYNTRANS_HW_H

#include <stdint.h>

enum {
    CC_DYN_MAIN = 0,
    CC_DYN_SUB = 1
};

void cc_dyn_hw_reset(void);
unsigned cc_dyn_hw_pending(void);
unsigned cc_dyn_hw_overflowed(void);
void cc_dyn_hw_flush(void);

uint8_t  cc_dyn_read8(unsigned cpu, unsigned addr);
uint16_t cc_dyn_read16(unsigned cpu, unsigned addr);
uint32_t cc_dyn_read32(unsigned cpu, unsigned addr);

void cc_dyn_write8(unsigned cpu, unsigned addr, unsigned data);
void cc_dyn_write16(unsigned cpu, unsigned addr, unsigned data);
void cc_dyn_write32(unsigned cpu, unsigned addr, unsigned data);

#endif
