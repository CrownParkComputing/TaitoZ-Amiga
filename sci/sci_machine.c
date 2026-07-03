/* cc_machine.c -- S.C.I. dual MC68000 machine model.
 * Forked from the Continental Circus Taito Z host/Amiga shell and updated to
 * S.C.I.'s main/sub maps. */
#include "m68k.h"
#include "sci_state.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef __amigaos__
#define SCI_TRACE_ENV(name) 0
#else
#define SCI_TRACE_ENV(name) getenv(name)
#endif

extern uint8_t cc_rom_main[];  /* 0x80000 maincpu */
extern uint8_t cc_rom_sub[];   /* 0x20000 sub */

/* ---- video/CPU RAM (shared via cc_state.h) ---- */
uint8_t  cc_scn[0x14000];
uint8_t  cc_scnctl[0x10];
uint8_t  cc_road[0x2000];
uint8_t  cc_spr[0x4000];
uint16_t cc_pal[0x1000];
int      cc_pal_dirty = 1;   /* set on any palette write; presenter clears (Power Drift pattern) */
int      cc_road_palbank = 3;

static uint8_t ram_main[0x8000], ram_main_extra[0x4000], ram_sub[0x4000], shared[0x4000];
static unsigned pal_addr;
static unsigned sci_spriteframe;
static int sub_enabled;
static unsigned char ctx_main[4096], ctx_sub[4096];
static int g_cpu;   /* 0=main, 1=sub */
static unsigned ctrl_count, ctrl_last;
static unsigned irq_count_main, irq_count_sub;
static unsigned cycles_main, cycles_sub;
static unsigned idle_skip_frames;
static unsigned sci_int6_toggle;

/* ---- TC0220IOC inputs (active-low; default idle, overridable via cc_set_inputs) ---- */
static uint8_t io_dswa=0xff, io_dswb=0xff, io_in0=0xff, io_in1=0xff, io_in2=0xff;
/* IOC ports 8-0xb: in MAME sci these fall through to tc0040ioc portreg_r
 * (not inputs) -- reading back register state, 0 by default, NOT 0xff. */
static uint8_t io_unk[4] = { 0x00, 0x00, 0x00, 0x00 };
static int     io_steer=0;            /* signed-ish, centered 0 */
static uint8_t ioc_mport=0, ioc_regs[8];
static unsigned ioc_read_counts[16];
static unsigned ioc_trace_count;
static unsigned ioc_write_trace_count;
static uint8_t motor_ram[0x200];
static unsigned motor_trace_count;
static unsigned pc_trace_count;
static unsigned pal_trace_count;

#ifdef SCI_PC_COVERAGE
static uint8_t cov_main[0x80000 / 2];
static uint8_t cov_sub[0x20000 / 2];
#endif

static unsigned load_be16(const uint8_t *p){ return ((unsigned)p[0] << 8) | p[1]; }
static unsigned load_be32(const uint8_t *p){ return ((unsigned)p[0] << 24) | ((unsigned)p[1] << 16) | ((unsigned)p[2] << 8) | p[3]; }
static void store_be16(uint8_t *p, unsigned v){ p[0] = (uint8_t)(v >> 8); p[1] = (uint8_t)v; }
static unsigned steer_word(void){ return (0xff80u + (0x80u + (unsigned)(io_steer & 0xff))) & 0xffffu; }

void cc_set_inputs(uint8_t in0, uint8_t in1, uint8_t dswa, uint8_t dswb, int steer){
    io_in0=in0; io_in1=in1; io_dswa=dswa; io_dswb=dswb; io_steer=steer;
}

static unsigned ioc_read_selected(void){
    unsigned raw = ioc_mport;
    unsigned p=ioc_mport & 15;
    unsigned ret;
    ioc_read_counts[p]++;
    if(p>=0x08&&p<=0x0b) ret = io_unk[p - 0x08];
    else if(p==0x0c||p==0x0d){ unsigned s=(0xff80 + (0x80 + io_steer)) & 0xffff; ret = (p==0x0c)?(s&0xff):(s>>8); }
    else switch(p){ case 0:ret=io_dswa; break; case 1:ret=io_dswb; break; case 2:ret=io_in0; break;
               case 3:ret=io_in1; break; case 4:ret=ioc_regs[4]; break; case 7:ret=io_in2; break; default:ret=0xff; break; }
    if(SCI_TRACE_ENV("SCI_IOCTRACE") && ioc_trace_count < 256){
        ioc_trace_count++;
        printf("IOCR pc=%06x raw=%02x port=%x ret=%02x\n", m68k_get_reg(NULL, M68K_REG_PC), raw, p, ret);
    }
    return ret;
}

static unsigned ioc_read_offset(unsigned off){
    unsigned p = off & 7;
    ioc_read_counts[p]++;
    switch(p){
        case 0: return io_dswa;
        case 1: return io_dswb;
        case 2: return io_in0;
        case 3: return io_in1;
        case 4: return ioc_regs[4];
        case 7: return io_in2;
        default: return 0xff;
    }
}

static void ioc_write_offset(unsigned off, unsigned v){
    unsigned p = off & 7;
    ioc_regs[p] = (uint8_t)v;
    if(SCI_TRACE_ENV("SCI_IOCTRACE") && ioc_write_trace_count < 256){
        ioc_write_trace_count++;
        printf("IOCW pc=%06x off=%x data=%02x\n", m68k_get_reg(NULL, M68K_REG_PC), p, v & 0xff);
    }
}

static unsigned motor_rd8(unsigned a){
    unsigned off = (a - 0xe00000) >> 1;
    unsigned word = 0;
    /* MAME sci_motor_r: offset 0 = rand()&0xff (motor position feedback --
     * the game expects it to VARY; a frozen value reads as a stuck motor),
     * offset 0x101 = 0x55 (motor CPU alive), everything else = 0.  Our old
     * stub returned a constant 0x55 at offset 0 and RAM readback elsewhere. */
    if(off == 0x000){
        static unsigned lfsr = 0x2463;
        lfsr = (lfsr >> 1) ^ ((0u - (lfsr & 1u)) & 0xB400u);
        word = lfsr & 0xff;
    }
    else if(off == 0x101) word = 0x0055;
    if(SCI_TRACE_ENV("SCI_MOTORTRACE") && motor_trace_count < 256){
        motor_trace_count++;
        printf("MOTORR pc=%06x a=%06x off=%03x ret=%02x\n",
               m68k_get_reg(NULL, M68K_REG_PC), a & 0xffffff, off, word & 0xff);
    }
    return (a & 1) ? (word & 0xff) : (word >> 8);
}

static void motor_wr8(unsigned a, unsigned v){
    unsigned off = (a - 0xe00000) >> 1;
    if(off < 0x200) motor_ram[off] = v & 0xff;
    if(SCI_TRACE_ENV("SCI_MOTORTRACE") && motor_trace_count < 256){
        motor_trace_count++;
        printf("MOTORW pc=%06x a=%06x off=%03x data=%02x\n",
               m68k_get_reg(NULL, M68K_REG_PC), a & 0xffffff, off, v & 0xff);
    }
}

static void cpu_ctrl_w(unsigned data){
    data &= 0xff;
    ctrl_count++;
    ctrl_last = data;
    if(SCI_TRACE_ENV("SCI_CTRLTRACE") && ctrl_count<=20)
        printf("CTRLW #%u pc=%06x ppc=%06x data=%02x\n", ctrl_count,
               m68k_get_reg(NULL, M68K_REG_PC), m68k_get_reg(NULL, M68K_REG_PPC), data);
    int was = sub_enabled;
    sub_enabled = data & 0x01;
    cc_audio_reset_line((data & 0x04) ? 1 : 0);
    if(sub_enabled && !was){
        int old_cpu = g_cpu;
        if(old_cpu==0) m68k_get_context(ctx_main);
        else m68k_get_context(ctx_sub);
        g_cpu=1;
        m68k_set_context(ctx_sub);
        m68k_pulse_reset();
        m68k_get_context(ctx_sub);
        g_cpu=old_cpu;
        m68k_set_context(old_cpu==0 ? ctx_main : ctx_sub);
    }
}

static unsigned rd8(unsigned a){
    a&=0xffffff;
    if(g_cpu==0){
        if(a<0x080000) return cc_rom_main[a];
        if(a>=0x100000&&a<0x108000) return ram_main[a-0x100000];
        if(a>=0x108000&&a<0x10c000) return shared[a-0x108000];
        if(a>=0x10c000&&a<0x110000) return ram_main_extra[a-0x10c000];
        if(a>=0x200000&&a<0x200010) return (a&1) ? ioc_read_offset((a-0x200000)>>1) : 0;
        if(a>=0x200010&&a<0x200020){
            unsigned off=(a-0x200010)>>1, s=steer_word();
            if(!(a&1)) return 0;
            if(off==4) return s & 0xff;
            if(off==5) return (s >> 8) & 0xff;
            return 0xff;
        }
        if(a==0x420003) return cc_syt_master_comm_r();   /* TC0140SYT master comm */
        if(a>=0x800000&&a<0x802000){ unsigned v=cc_pal[((a-0x800000)>>1)&0xfff]; return (a&1)?(v&0xff):(v>>8); }
        if(a>=0xa00000&&a<0xa10000) return cc_scn[a-0xa00000];
        if(a>=0xa20000&&a<0xa20010) return cc_scnctl[a-0xa20000];
        if(a>=0xc00000&&a<0xc04000) return cc_spr[a-0xc00000];
        if(a==0xc08000) return (sci_spriteframe >> 8) & 0xff;
        if(a==0xc08001) return sci_spriteframe & 0xff;
        return 0xff;
    } else {
        if(a<0x020000) return cc_rom_sub[a];
        if(a>=0x200000&&a<0x204000) return ram_sub[a-0x200000];
        if(a>=0x208000&&a<0x20c000) return shared[a-0x208000];
        if(a>=0xa00000&&a<0xa02000) return cc_road[a-0xa00000];
        return 0xff;
    }
}
static void wr8(unsigned a, unsigned v){
    a&=0xffffff; v&=0xff;
    if(g_cpu==0){
        if(a>=0x100000&&a<0x108000){ ram_main[a-0x100000]=v; return; }
        if(a>=0x108000&&a<0x10c000){ shared[a-0x108000]=v; return; }
        if(a>=0x10c000&&a<0x110000){ ram_main_extra[a-0x10c000]=v; return; }
        if(a>=0x200000&&a<0x200010){ if(a&1) ioc_write_offset((a-0x200000)>>1, v); return; }
        if(a==0x400000 || a==0x400001){
            cpu_ctrl_w(v);
            return;
        }
        if(a==0x420001){ cc_syt_master_port_w(v); return; }   /* TC0140SYT master port */
        if(a==0x420003){ cc_syt_master_comm_w(v); return; }   /* TC0140SYT master comm */
        if(a>=0x800000&&a<0x802000){ unsigned i=((a-0x800000)>>1)&0xfff;
            if(a&1)cc_pal[i]=(cc_pal[i]&0xff00)|v; else cc_pal[i]=(cc_pal[i]&0x00ff)|(v<<8);
            cc_pal_dirty=1; return; }
        if(a>=0xa00000&&a<0xa10000){ cc_scn[a-0xa00000]=v; return; }
        if(a>=0xa20000&&a<0xa20010){ cc_scnctl[a-0xa20000]=v; return; }
        if(a>=0xc00000&&a<0xc04000){ cc_spr[a-0xc00000]=v; return; }
        if(a==0xc08000){ sci_spriteframe=(sci_spriteframe&0x00ff)|(v<<8); return; }
        if(a==0xc08001){ sci_spriteframe=(sci_spriteframe&0xff00)|v; return; }
        return;
    } else {
        if(a>=0x200000&&a<0x204000){ ram_sub[a-0x200000]=v; return; }
        if(a>=0x208000&&a<0x20c000){ shared[a-0x208000]=v; return; }
        if(a>=0xa00000&&a<0xa02000){ cc_road[a-0xa00000]=v; return; }
        return;
    }
}

/* Musashi big-endian glue */
unsigned int m68k_read_memory_8 (unsigned int a){ return rd8(a); }
unsigned int m68k_read_memory_16(unsigned int a){
    a &= 0xffffff;
    if(g_cpu==0){
        if(a + 1 < 0x080000) return load_be16(&cc_rom_main[a]);
        if(a>=0x100000&&a+1<0x108000) return load_be16(&ram_main[a-0x100000]);
        if(a>=0x108000&&a+1<0x10c000) return load_be16(&shared[a-0x108000]);
        if(a>=0x10c000&&a+1<0x110000) return load_be16(&ram_main_extra[a-0x10c000]);
        if(a>=0x200000&&a<0x200010) return ioc_read_offset((a-0x200000)>>1) & 0xff;
        if(a>=0x200010&&a<0x200020){
            unsigned off=(a-0x200010)>>1, s=steer_word();
            if(off==4) return s & 0xff;
            if(off==5) return (s >> 8) & 0xff;
            return 0xff;
        }
        if(a==0x420002) return cc_syt_master_comm_r() & 0xff;
        if(a>=0x800000&&a+1<0x802000) return cc_pal[((a-0x800000)>>1)&0xfff];
        if(a>=0xa00000&&a+1<0xa10000) return load_be16(&cc_scn[a-0xa00000]);
        if(a>=0xa20000&&a+1<0xa20010) return load_be16(&cc_scnctl[a-0xa20000]);
        if(a>=0xc00000&&a+1<0xc04000) return load_be16(&cc_spr[a-0xc00000]);
        if(a==0xc08000) return sci_spriteframe & 0xffff;
    } else {
        if(a + 1 < 0x020000) return load_be16(&cc_rom_sub[a]);
        if(a>=0x200000&&a+1<0x204000) return load_be16(&ram_sub[a-0x200000]);
        if(a>=0x208000&&a+1<0x20c000) return load_be16(&shared[a-0x208000]);
        if(a>=0xa00000&&a+1<0xa02000) return load_be16(&cc_road[a-0xa00000]);
    }
    return (rd8(a)<<8)|rd8(a+1);
}
unsigned int m68k_read_memory_32(unsigned int a){
    a &= 0xffffff;
    if(g_cpu==0){
        if(a + 3 < 0x080000) return load_be32(&cc_rom_main[a]);
        if(a>=0x100000&&a+3<0x108000) return load_be32(&ram_main[a-0x100000]);
        if(a>=0x108000&&a+3<0x10c000) return load_be32(&shared[a-0x108000]);
        if(a>=0x10c000&&a+3<0x110000) return load_be32(&ram_main_extra[a-0x10c000]);
        if(a>=0xa00000&&a+3<0xa10000) return load_be32(&cc_scn[a-0xa00000]);
        if(a>=0xc00000&&a+3<0xc04000) return load_be32(&cc_spr[a-0xc00000]);
    } else {
        if(a + 3 < 0x020000) return load_be32(&cc_rom_sub[a]);
        if(a>=0x200000&&a+3<0x204000) return load_be32(&ram_sub[a-0x200000]);
        if(a>=0x208000&&a+3<0x20c000) return load_be32(&shared[a-0x208000]);
        if(a>=0xa00000&&a+3<0xa02000) return load_be32(&cc_road[a-0xa00000]);
    }
    return (m68k_read_memory_16(a)<<16)|m68k_read_memory_16(a+2);
}

unsigned int m68k_read_immediate_16(unsigned int a){
    a &= 0xffffff;
    if(g_cpu==0){
        if(a + 1 < 0x080000) return load_be16(&cc_rom_main[a]);
        if(a>=0x100000&&a+1<0x108000) return load_be16(&ram_main[a-0x100000]);
        if(a>=0x108000&&a+1<0x10c000) return load_be16(&shared[a-0x108000]);
        if(a>=0x10c000&&a+1<0x110000) return load_be16(&ram_main_extra[a-0x10c000]);
    } else {
        if(a + 1 < 0x020000) return load_be16(&cc_rom_sub[a]);
        if(a>=0x200000&&a+1<0x204000) return load_be16(&ram_sub[a-0x200000]);
        if(a>=0x208000&&a+1<0x20c000) return load_be16(&shared[a-0x208000]);
    }
    return m68k_read_memory_16(a);
}

unsigned int m68k_read_immediate_32(unsigned int a){
    a &= 0xffffff;
    if(g_cpu==0){
        if(a + 3 < 0x080000) return load_be32(&cc_rom_main[a]);
        if(a>=0x100000&&a+3<0x108000) return load_be32(&ram_main[a-0x100000]);
        if(a>=0x108000&&a+3<0x10c000) return load_be32(&shared[a-0x108000]);
        if(a>=0x10c000&&a+3<0x110000) return load_be32(&ram_main_extra[a-0x10c000]);
    } else {
        if(a + 3 < 0x020000) return load_be32(&cc_rom_sub[a]);
        if(a>=0x200000&&a+3<0x204000) return load_be32(&ram_sub[a-0x200000]);
        if(a>=0x208000&&a+3<0x20c000) return load_be32(&shared[a-0x208000]);
    }
    return m68k_read_memory_32(a);
}

unsigned int m68k_read_pcrelative_8(unsigned int a){ return rd8(a); }
unsigned int m68k_read_pcrelative_16(unsigned int a){ return m68k_read_immediate_16(a); }
unsigned int m68k_read_pcrelative_32(unsigned int a){ return m68k_read_immediate_32(a); }

void m68k_write_memory_8 (unsigned int a, unsigned int v){ wr8(a,v); }
void m68k_write_memory_16(unsigned int a, unsigned int v){
    a &= 0xffffff;
    /* 0x800000 is the CPU-control register ONLY on the MAIN cpu's bus; on the
     * SUB it is the first word of TC0150ROD road RAM.  Without the g_cpu test
     * the sub's own road-RAM clear (move.l D1,(A4)+ at sub 0x9c8) was misrouted
     * here as cpu_ctrl_w(0) -- the sub RESET ITSELF the moment it initialised
     * its road buffer, so road RAM stayed empty = black road in demo and race. */
    if(g_cpu==0 && a == 0x400000){
        cpu_ctrl_w((v >> 8) ? (v >> 8) : v);
        return;
    }
    if(g_cpu==0){
        if(a>=0x100000&&a+1<0x108000){ store_be16(&ram_main[a-0x100000], v); return; }
        if(a>=0x108000&&a+1<0x10c000){ store_be16(&shared[a-0x108000], v); return; }
        if(a>=0x10c000&&a+1<0x110000){ store_be16(&ram_main_extra[a-0x10c000], v); return; }
        if(a>=0x200000&&a<0x200010){ ioc_write_offset((a-0x200000)>>1, v & 0xff); return; }
        if(a==0x420000 || a==0x420001){ cc_syt_master_port_w(v & 0xff); return; }
        if(a==0x420002 || a==0x420003){ cc_syt_master_comm_w(v & 0xff); return; }
        if(a>=0x800000&&a+1<0x802000){ cc_pal[((a-0x800000)>>1)&0xfff] = (uint16_t)v; cc_pal_dirty=1; return; }
        if(a>=0xa00000&&a+1<0xa10000){ store_be16(&cc_scn[a-0xa00000], v); return; }
        if(a>=0xa20000&&a+1<0xa20010){ store_be16(&cc_scnctl[a-0xa20000], v); return; }
        if(a>=0xc00000&&a+1<0xc04000){ store_be16(&cc_spr[a-0xc00000], v); return; }
        if(a==0xc08000){ sci_spriteframe = v & 0xffff; return; }
    } else {
        if(a>=0x200000&&a+1<0x204000){ store_be16(&ram_sub[a-0x200000], v); return; }
        if(a>=0x208000&&a+1<0x20c000){ store_be16(&shared[a-0x208000], v); return; }
        if(a>=0xa00000&&a+1<0xa02000){ store_be16(&cc_road[a-0xa00000], v); return; }
    }
    wr8(a,v>>8); wr8(a+1,v);
}
void m68k_write_memory_32(unsigned int a, unsigned int v){
    m68k_write_memory_16(a, v >> 16);
    m68k_write_memory_16(a + 2, v);
}
unsigned int m68k_read_disassembler_8 (unsigned int a){ return rd8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a){ return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a){ return m68k_read_memory_32(a); }

static int cc_int_ack(int level){
    (void)level;
    if(g_cpu==0) irq_count_main++;
    else irq_count_sub++;
    m68k_set_irq(0);
    return M68K_INT_ACK_AUTOVECTOR;
}

/* ---- busy-wait spin-skip (Power Drift wait-hook pattern) --------------------
 * Both CPUs park in a wait-for-vblank loop once their frame work is done (main
 * 0x252, sub 0x4de/0x4d8).  The instruction hook ends the timeslice the moment
 * a CPU arrives there, and run_frame pre-checks the saved PC so a parked CPU
 * costs zero interpreted cycles until the next vblank IRQ (m68k_set_irq moves
 * the PC into the handler, so the pre-check never blocks IRQ delivery). */
static int cpu_waiting_at(int cpu, unsigned pc){
    if(SCI_TRACE_ENV("SCI_NOSPIN")) return 0;            /* host bisect: disable spin-skip */
    if(cpu==0) return pc==0x000252;                      /* 60fe = bra.s self: pure IRQ wait */
    if(pc==0x0004d8 || pc==0x0004de)                     /* tst.w $100804 / beq.s -8 */
        return ram_sub[0x804]==0 && ram_sub[0x805]==0;   /* parked only while the IRQ flag is clear */
    return 0;
}
static int spin_tripped;

static void instr_hook(unsigned pc){
    if(cpu_waiting_at(g_cpu, pc)){
        spin_tripped = 1;
        m68k_end_timeslice();
    }
#ifdef SCI_PC_COVERAGE
    if(g_cpu==0 && pc < 0x80000) cov_main[pc >> 1] = 1;
    else if(g_cpu==1 && pc < 0x20000) cov_sub[pc >> 1] = 1;
#endif
#ifndef __amigaos__
    /* host-only PC watchpoint tracer: SCI_PCTRACE=<hex pc> prints regs each
     * time the main CPU passes that address (replaces the old hardcoded
     * bring-up PC list). */
    if(pc_trace_count >= 512) return;
    {
        const char *w = SCI_TRACE_ENV("SCI_PCTRACE");
        if(w && g_cpu==0 && pc == (unsigned)strtoul(w, 0, 16)){
            pc_trace_count++;
            unsigned a7 = m68k_get_reg(NULL, M68K_REG_A7);
            printf("PCTRACE cpu=%d pc=%06x sr=%04x d0=%08x a0=%08x a5=%08x a7=%08x ret=%08x\n",
                   g_cpu, pc, m68k_get_reg(NULL, M68K_REG_SR), m68k_get_reg(NULL, M68K_REG_D0),
                   m68k_get_reg(NULL, M68K_REG_A0), m68k_get_reg(NULL, M68K_REG_A5),
                   a7, m68k_read_memory_32(a7));
        }
    }
#endif
}

void cc_machine_init(void){
    memset(ram_main,0,sizeof ram_main); memset(ram_main_extra,0,sizeof ram_main_extra);
    memset(ram_sub,0,sizeof ram_sub); memset(shared,0,sizeof shared);
    memset(cc_scn,0,sizeof cc_scn); memset(cc_road,0,sizeof cc_road); memset(cc_spr,0,sizeof cc_spr);
    memset(cc_pal,0,sizeof cc_pal); memset(cc_scnctl,0,sizeof cc_scnctl);
    memset(motor_ram,0,sizeof motor_ram);
#ifdef SCI_PC_COVERAGE
    memset(cov_main,0,sizeof cov_main);
    memset(cov_sub,0,sizeof cov_sub);
#endif
    memset(ioc_regs,0,sizeof ioc_regs);
    ioc_regs[4]=0x03;  /* S.C.I. reads back coin/lockout output state during POST/input poll. */
    sci_spriteframe=0;
    sub_enabled=0; ctrl_count=0; ctrl_last=0; irq_count_main=0; irq_count_sub=0; pc_trace_count=0;
    pal_trace_count=0;
    cycles_main=0; cycles_sub=0; idle_skip_frames=0; sci_int6_toggle=0;
    ioc_write_trace_count=0; ioc_trace_count=0; motor_trace_count=0;
    memset(ioc_read_counts,0,sizeof ioc_read_counts);
    m68k_init();
    m68k_set_int_ack_callback(cc_int_ack);
    m68k_set_instr_hook_callback(instr_hook);   /* spin-skip needs it on Amiga too */
    g_cpu=0; m68k_set_cpu_type(M68K_CPU_TYPE_68000); m68k_pulse_reset(); m68k_get_context(ctx_main);
    g_cpu=1; m68k_set_cpu_type(M68K_CPU_TYPE_68000); m68k_pulse_reset(); m68k_get_context(ctx_sub);
}

void cc_machine_debug_ioc(uint8_t *port, unsigned counts[16]){
    if(port) *port = ioc_mport;
    if(counts) memcpy(counts, ioc_read_counts, sizeof ioc_read_counts);
}

void cc_machine_coverage_dump(const char *path){
#ifdef SCI_PC_COVERAGE
    FILE *f = fopen(path ? path : "cc_pc_coverage.txt", "w");
    if(!f) return;
    unsigned main_count=0, sub_count=0;
    for(unsigned i=0;i<sizeof cov_main;i++) if(cov_main[i]) main_count++;
    for(unsigned i=0;i<sizeof cov_sub;i++) if(cov_sub[i]) sub_count++;
    fprintf(f, "# S.C.I. executed PC coverage: main=%u sub=%u\n", main_count, sub_count);
    fprintf(f, "[main]\n");
    for(unsigned i=0;i<sizeof cov_main;i++) if(cov_main[i]) fprintf(f, "%06x\n", i << 1);
    fprintf(f, "[sub]\n");
    for(unsigned i=0;i<sizeof cov_sub;i++) if(cov_sub[i]) fprintf(f, "%06x\n", i << 1);
    fclose(f);
#else
    (void)path;
#endif
}

static int bus_select_cpu(unsigned cpu){
    int old = g_cpu;
    if(cpu != (unsigned)old){
        if(old==0) m68k_get_context(ctx_main);
        else m68k_get_context(ctx_sub);
        g_cpu = cpu ? 1 : 0;
        m68k_set_context(g_cpu==0 ? ctx_main : ctx_sub);
    }
    return old;
}

static void bus_restore_cpu(int old){
    if(g_cpu != old){
        if(g_cpu==0) m68k_get_context(ctx_main);
        else m68k_get_context(ctx_sub);
        g_cpu = old;
        m68k_set_context(old==0 ? ctx_main : ctx_sub);
    }
}

uint8_t cc_bus_read8(unsigned cpu, unsigned addr){
    int old = bus_select_cpu(cpu);
    uint8_t v = (uint8_t)rd8(addr);
    bus_restore_cpu(old);
    return v;
}

uint16_t cc_bus_read16(unsigned cpu, unsigned addr){
    int old = bus_select_cpu(cpu);
    uint16_t v = (uint16_t)m68k_read_memory_16(addr);
    bus_restore_cpu(old);
    return v;
}

uint32_t cc_bus_read32(unsigned cpu, unsigned addr){
    int old = bus_select_cpu(cpu);
    uint32_t v = m68k_read_memory_32(addr);
    bus_restore_cpu(old);
    return v;
}

void cc_bus_write8(unsigned cpu, unsigned addr, unsigned data){
    int old = bus_select_cpu(cpu);
    wr8(addr, data);
    bus_restore_cpu(old);
}

void cc_bus_write16(unsigned cpu, unsigned addr, unsigned data){
    int old = bus_select_cpu(cpu);
    m68k_write_memory_16(addr, data);
    bus_restore_cpu(old);
}

void cc_bus_write32(unsigned cpu, unsigned addr, unsigned data){
    int old = bus_select_cpu(cpu);
    m68k_write_memory_32(addr, data);
    bus_restore_cpu(old);
}

void cc_machine_debug(unsigned *main_pc, unsigned *sub_pc, int *sub_on, unsigned *count, unsigned *last){
    int old = g_cpu;
    g_cpu=0; m68k_set_context(ctx_main); if(main_pc) *main_pc = m68k_get_reg(NULL, M68K_REG_PC); m68k_get_context(ctx_main);
    g_cpu=1; m68k_set_context(ctx_sub);  if(sub_pc)  *sub_pc  = m68k_get_reg(NULL, M68K_REG_PC); m68k_get_context(ctx_sub);
    g_cpu=old; if(old==0) m68k_set_context(ctx_main); else m68k_set_context(ctx_sub);
    if(sub_on) *sub_on = sub_enabled;
    if(count) *count = ctrl_count;
    if(last) *last = ctrl_last;
}

void cc_machine_debug_irq(unsigned *main_irqs, unsigned *sub_irqs){
    if(main_irqs) *main_irqs = irq_count_main;
    if(sub_irqs) *sub_irqs = irq_count_sub;
}

void cc_machine_debug_cpu(unsigned *main_sr, unsigned *sub_sr, unsigned *main_cycles, unsigned *sub_cycles){
    int old = g_cpu;
    g_cpu=0; m68k_set_context(ctx_main); if(main_sr) *main_sr = m68k_get_reg(NULL, M68K_REG_SR); m68k_get_context(ctx_main);
    g_cpu=1; m68k_set_context(ctx_sub);  if(sub_sr)  *sub_sr  = m68k_get_reg(NULL, M68K_REG_SR); m68k_get_context(ctx_sub);
    g_cpu=old; if(old==0) m68k_set_context(ctx_main); else m68k_set_context(ctx_sub);
    if(main_cycles) *main_cycles = cycles_main;
    if(sub_cycles) *sub_cycles = cycles_sub;
}

void cc_machine_debug_main_regs(unsigned regs[4]){
    if(!regs) return;
    int old = g_cpu;
    g_cpu=0; m68k_set_context(ctx_main);
    regs[0] = m68k_get_reg(NULL, M68K_REG_D0);
    regs[1] = m68k_get_reg(NULL, M68K_REG_A0);
    regs[2] = m68k_get_reg(NULL, M68K_REG_A5);
    regs[3] = m68k_get_reg(NULL, M68K_REG_A7);
    m68k_get_context(ctx_main);
    g_cpu=old; if(old==0) m68k_set_context(ctx_main); else m68k_set_context(ctx_sub);
}

static uint16_t be16(uint8_t *p){ return ((uint16_t)p[0] << 8) | p[1]; }

void cc_machine_debug_tasks(unsigned *active, uint16_t first_words[8]){
    unsigned n = 0;
    for(unsigned off=0; off<0x100; off+=0x10){
        if(be16(&ram_main[off])) n++;
    }
    if(active) *active = n;
    if(first_words){
        for(unsigned i=0; i<8; i++) first_words[i] = be16(&ram_main[i*0x10]);
    }
}

uint16_t cc_machine_debug_main_word(unsigned off){
    if(off + 1 >= sizeof ram_main) return 0xffff;
    return be16(&ram_main[off]);
}

uint32_t cc_machine_debug_main_long(unsigned off){
    if(off + 3 >= sizeof ram_main) return 0xffffffffu;
    return ((uint32_t)ram_main[off] << 24) | ((uint32_t)ram_main[off+1] << 16) |
           ((uint32_t)ram_main[off+2] << 8) | ram_main[off+3];
}

void cc_machine_debug_bus(uint16_t *shared_1a, uint16_t *shared_1e, uint16_t *shared_a6,
                          uint16_t *sub_0802, uint16_t *sub_0804, uint16_t *sub_080a,
                          uint16_t *sub_1a58, uint16_t *sub_1a5a){
    if(shared_1a) *shared_1a = be16(&shared[0x001a]);
    if(shared_1e) *shared_1e = be16(&shared[0x001e]);
    if(shared_a6) *shared_a6 = be16(&shared[0x00a6]);
    if(sub_0802) *sub_0802 = be16(&ram_sub[0x0802]);
    if(sub_0804) *sub_0804 = be16(&ram_sub[0x0804]);
    if(sub_080a) *sub_080a = be16(&ram_sub[0x080a]);
    if(sub_1a58) *sub_1a58 = be16(&ram_sub[0x1a58]);
    if(sub_1a5a) *sub_1a5a = be16(&ram_sub[0x1a5a]);
}

/* Run one CPU for up to `per` cycles with spin-skip: costs zero if it is
 * already parked in its vblank-wait loop, and the instruction hook ends the
 * slice the moment it parks.  Returns 1 if the CPU is parked afterwards.
 * allow_park must be 0 for the first slice after m68k_set_irq: this Musashi
 * only services a raised IRQ inside m68k_execute (entry check), so skipping
 * the execute call would leave the interrupt pending forever.
 * Caller guarantees g_cpu/context selection. */
static int exec_cpu_slice(int cpu, int per, unsigned *cyc, int allow_park){
    if(allow_park){
        unsigned pc = m68k_get_reg(NULL, M68K_REG_PC);
        if(cpu_waiting_at(cpu, pc)) return 1;      /* parked: skip entirely */
    }
    spin_tripped = 0;
    *cyc += (unsigned)m68k_execute(per);
    return spin_tripped;
}

void cc_machine_run_frame(void){
    const int CYC_FRAME = 12000000/60;         /* 200000 per cpu per frame */
    const int CYC_VBL   = CYC_FRAME * 39/262;  /* vblank tail: lines 223-261 */
    const int CYC_VIS   = CYC_FRAME - CYC_VBL; /* visible period: lines 0-222 */
    int main_parked=0, sub_parked=0;

    /* ---- visible phase (no IRQ): like real hardware, the CPUs run the frame
     * BEFORE vblank fires.  Firing IRQ4 at frame start (the old model) made
     * every IRQ-handler deadline effectively one frame shorter -- the sub's
     * boot init lost its razor-thin race against the main's 6-frame watchdog
     * (MAME oracle: ctrl is written once at F106 and the sub lives forever;
     * our old model killed it at +6 frames every retry, so road RAM stayed
     * empty = black road).  Parked CPUs cost nothing here. */
    {
        const int S = 6, per = CYC_VIS / S;
        for(int s=0;s<S;s++){
            if(!main_parked){
                g_cpu=0; m68k_set_context(ctx_main);
                main_parked = exec_cpu_slice(0, per, &cycles_main, 1);
                m68k_get_context(ctx_main);
            }
            /* sub_enabled can flip ON mid-phase (main writes CPU-ctrl): the
             * freshly reset sub must run in this same frame's remaining slices. */
            if(sub_enabled && !sub_parked){
                g_cpu=1; m68k_set_context(ctx_sub);
                sub_parked = exec_cpu_slice(1, per, &cycles_sub, 1);
                m68k_get_context(ctx_sub);
            }
            if(main_parked && (sub_parked || !sub_enabled)){
                idle_skip_frames++;
                break;
            }
        }
    }

    /* ---- vblank: raise IRQ4 on both CPUs and run the frame tail.  The first
     * tail slice must not pre-park (this Musashi services a raised IRQ only at
     * m68k_execute entry).  S.C.I. also needs an alternating late IRQ6; MAME's
     * driver notes that two IRQ4s per IRQ6 are required for the sprite/game
     * task state to become valid. */
    sci_int6_toggle ^= 1;
    g_cpu=0; m68k_set_context(ctx_main); m68k_set_irq(4); m68k_get_context(ctx_main);
    if(sub_enabled){ g_cpu=1; m68k_set_context(ctx_sub);  m68k_set_irq(4); m68k_get_context(ctx_sub); }
    {
        const int S = 2, per = CYC_VBL / S;
        main_parked = 0; sub_parked = 0;
        for(int s=0;s<S;s++){
            if(!main_parked){
                g_cpu=0; m68k_set_context(ctx_main);
                main_parked = exec_cpu_slice(0, per, &cycles_main, s>0);
                m68k_get_context(ctx_main);
            }
            if(sub_enabled && !sub_parked){
                g_cpu=1; m68k_set_context(ctx_sub);
                sub_parked = exec_cpu_slice(1, per, &cycles_sub, s>0);
                m68k_get_context(ctx_sub);
            }
            if(main_parked && (sub_parked || !sub_enabled)) break;
        }
    }
    if(sci_int6_toggle){
        g_cpu=0; m68k_set_context(ctx_main);
        m68k_set_irq(6);
        cycles_main += (unsigned)m68k_execute(500);
        m68k_get_context(ctx_main);
    }
}
