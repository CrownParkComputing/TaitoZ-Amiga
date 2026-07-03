/* cc_dyntrans_hw.c -- ordered helper/write-log layer for Chase HQ dyntrans. */
#include "cc_dyntrans_hw.h"
#include "cc_state.h"

#define CC_DYN_HW_LOG_CAP 8192

struct hwop {
    uint8_t cpu;
    uint8_t size;
    uint16_t pad;
    uint32_t addr;
    uint32_t data;
};

static struct hwop hwlog[CC_DYN_HW_LOG_CAP];
static unsigned hwlog_n;
static unsigned hwlog_overflow;

void cc_dyn_hw_reset(void)
{
    hwlog_n = 0;
    hwlog_overflow = 0;
}

unsigned cc_dyn_hw_pending(void)
{
    return hwlog_n;
}

unsigned cc_dyn_hw_overflowed(void)
{
    return hwlog_overflow;
}

static void log_write(unsigned cpu, unsigned size, unsigned addr, unsigned data)
{
    if (hwlog_n >= CC_DYN_HW_LOG_CAP) {
        hwlog_overflow = 1;
        return;
    }
    hwlog[hwlog_n].cpu = (uint8_t)(cpu ? CC_DYN_SUB : CC_DYN_MAIN);
    hwlog[hwlog_n].size = (uint8_t)size;
    hwlog[hwlog_n].addr = addr & 0xffffffu;
    hwlog[hwlog_n].data = data;
    hwlog_n++;
}

void cc_dyn_hw_flush(void)
{
    for (unsigned i = 0; i < hwlog_n; i++) {
        const struct hwop *op = &hwlog[i];
        if (op->size == 1) cc_bus_write8(op->cpu, op->addr, op->data);
        else if (op->size == 2) cc_bus_write16(op->cpu, op->addr, op->data);
        else cc_bus_write32(op->cpu, op->addr, op->data);
    }
    hwlog_n = 0;
}

uint8_t cc_dyn_read8(unsigned cpu, unsigned addr)
{
    if (hwlog_n) cc_dyn_hw_flush();
    return cc_bus_read8(cpu, addr);
}

uint16_t cc_dyn_read16(unsigned cpu, unsigned addr)
{
    if (hwlog_n) cc_dyn_hw_flush();
    return cc_bus_read16(cpu, addr);
}

uint32_t cc_dyn_read32(unsigned cpu, unsigned addr)
{
    if (hwlog_n) cc_dyn_hw_flush();
    return cc_bus_read32(cpu, addr);
}

void cc_dyn_write8(unsigned cpu, unsigned addr, unsigned data)
{
    log_write(cpu, 1, addr, data);
}

void cc_dyn_write16(unsigned cpu, unsigned addr, unsigned data)
{
    log_write(cpu, 2, addr, data);
}

void cc_dyn_write32(unsigned cpu, unsigned addr, unsigned data)
{
    log_write(cpu, 4, addr, data);
}
