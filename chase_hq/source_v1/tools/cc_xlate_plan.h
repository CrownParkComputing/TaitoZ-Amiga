/* cc_xlate_plan.h -- Chase HQ-specific classification for the shared 68000
 * translator. The shared Shinobi xlate core handles generic 68000 decode/emit;
 * this layer marks which absolute hardware references need semantic helpers. */
#ifndef CC_XLATE_PLAN_H
#define CC_XLATE_PLAN_H

#include <stdint.h>
#include "shinobi_xlate.h"

enum ccxl_region {
    CCXL_REG_ROM = 0,
    CCXL_REG_MAIN_RAM,
    CCXL_REG_SHARED,
    CCXL_REG_MAIN_EXTRA,
    CCXL_REG_IOC,
    CCXL_REG_CPU_CTRL,
    CCXL_REG_SOUND,
    CCXL_REG_PALETTE,
    CCXL_REG_TILE_RAM,
    CCXL_REG_TILE_CTRL,
    CCXL_REG_SPRITE_RAM,
    CCXL_REG_MOTOR,
    CCXL_REG_SUB_RAM,
    CCXL_REG_ROAD_RAM,
    CCXL_REG_UNKNOWN,
    CCXL_REG_PCREL_DATA,
    CCXL_REG_COUNT
};

const char *ccxl_region_name(unsigned region);
unsigned ccxl_region_for_addr(uint32_t addr);
int ccxl_region_needs_helper(unsigned region);
uint32_t ccxl_abs_target(const uint8_t *rom, uint32_t rom_size, uint32_t pc,
                         const xdec *d, int index);
int ccxl_is_addr_reg_load(uint16_t opcode, const xdec *d, int abs_index, unsigned *areg);

#endif
