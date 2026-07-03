#include "cc_xlate_plan.h"

static unsigned rd16(const uint8_t *g, uint32_t gsize, uint32_t a)
{
    if (a + 1 >= gsize) return 0xffff;
    return ((unsigned)g[a] << 8) | g[a + 1];
}

static int32_t sx16(unsigned v)
{
    return (int32_t)(int16_t)v;
}

const char *ccxl_region_name(unsigned r)
{
    static const char *names[] = {
        "rom", "main_ram", "shared", "main_extra", "ioc", "cpu_ctrl", "sound",
        "palette_port", "tile_ram", "tile_ctrl", "sprite_ram", "motor",
        "sub_ram", "road_ram", "unknown", "pcrel_data"
    };
    return r < CCXL_REG_COUNT ? names[r] : "?";
}

unsigned ccxl_region_for_addr(uint32_t a)
{
    a &= 0xffffff;
    if (a < 0x080000) return CCXL_REG_ROM;
    if (a >= 0x100000 && a < 0x108000) return CCXL_REG_MAIN_RAM;
    if (a >= 0x108000 && a < 0x10c000) return CCXL_REG_SHARED;
    if (a >= 0x10c000 && a < 0x110000) return CCXL_REG_MAIN_EXTRA;
    if (a >= 0x400000 && a < 0x400008) return CCXL_REG_IOC;
    if (a == 0x800000 || a == 0x800001) return CCXL_REG_CPU_CTRL;
    if (a >= 0x820000 && a < 0x820004) return CCXL_REG_SOUND;
    if (a >= 0xa00000 && a < 0xa00008) return CCXL_REG_PALETTE;
    if (a >= 0xc00000 && a < 0xc10000) return CCXL_REG_TILE_RAM;
    if (a >= 0xc20000 && a < 0xc20010) return CCXL_REG_TILE_CTRL;
    if (a >= 0xd00000 && a < 0xd00800) return CCXL_REG_SPRITE_RAM;
    if (a >= 0xe00000 && a < 0xe00400) return CCXL_REG_MOTOR;
    if (a >= 0x800000 && a < 0x802000) return CCXL_REG_ROAD_RAM;
    return CCXL_REG_UNKNOWN;
}

int ccxl_region_needs_helper(unsigned region)
{
    return region == CCXL_REG_IOC ||
           region == CCXL_REG_CPU_CTRL ||
           region == CCXL_REG_SOUND ||
           region == CCXL_REG_PALETTE;
}

uint32_t ccxl_abs_target(const uint8_t *rom, uint32_t rom_size, uint32_t pc,
                         const xdec *d, int i)
{
    uint32_t boff = d->abs[i].woff;
    switch (d->abs[i].kind) {
    case XF_ABSW:
        return (uint32_t)sx16(rd16(rom, rom_size, pc + boff)) & 0xffffffu;
    case XF_ABSL:
    case XF_IMM_AN:
        return ((rd16(rom, rom_size, pc + boff) << 16) |
                rd16(rom, rom_size, pc + boff + 2)) & 0xffffffu;
    case XF_IMM_AN_W:
        return (uint32_t)sx16(rd16(rom, rom_size, pc + boff)) & 0xffffffu;
    case XF_PCREL16:
        return (pc + boff + (uint32_t)sx16(rd16(rom, rom_size, pc + boff))) & 0xffffffu;
    default:
        return 0xffffffffu;
    }
}

int ccxl_is_addr_reg_load(uint16_t opcode, const xdec *d, int abs_index, unsigned *areg)
{
    unsigned op = (opcode >> 12) & 0xf;
    if (abs_index < 0 || abs_index >= d->nabs) return 0;
    if (d->abs[abs_index].kind == XF_IMM_AN || d->abs[abs_index].kind == XF_IMM_AN_W) {
        if (areg) *areg = (opcode >> 9) & 7;
        return 1;
    }
    if ((opcode & 0xf1c0) == 0x41c0) { /* LEA <ea>,An */
        if (areg) *areg = (opcode >> 9) & 7;
        return 1;
    }
    if ((op == 0x2 || op == 0x3) && ((opcode >> 6) & 7) == 1) { /* MOVEA */
        if (areg) *areg = (opcode >> 9) & 7;
        return 1;
    }
    return 0;
}
