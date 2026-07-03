/* Continental Circus (contcirc) — step 02 host-reference model.
 *
 * Dual MC68000 (maincpu + sub) on the Musashi core, wiring the exact contcirc
 * address maps from MAME's taito/taito_z.cpp. Video chips (TC0100SCN /
 * TC0150ROD / TC0110PCR) and sprites are *stubbed as plain RAM* here — the goal
 * of this stage is to prove the CPU + memory model boots the attract logic and
 * drives video/sprite/road RAM. The Z80 + YM2610 are not wired yet (next
 * increment); the audio mailbox returns benign values so the CPUs don't block.
 *
 * Both 68000s free-run from reset and share 0x084000-0x087fff ("share1").
 * Each takes IRQ6 (autovector) once per frame at vblank (irq6_line_hold).
 *
 * Build: bash build.sh    Run: ./host [frames]
 * Dumps: out/state/{scn_ram,scn_ctrl,spriteram,road_ram,palette,ram_main,ram_sub,shared}.bin
 */
#include "m68k.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ---- ROM/RAM/video regions (sizes from the MAME maps) ---- */
#define ROM_SZ     0x40000   /* 256K each cpu */
#define RAM_SZ     0x4000    /* 0x080000-0x083fff private RAM, per cpu */
#define SHARE_SZ   0x4000    /* 0x084000-0x087fff shared */
#define SCN_SZ     0x10000   /* 0x200000-0x20ffff TC0100SCN ram */
#define SCNCTL_SZ  0x10      /* 0x220000-0x22000f TC0100SCN ctrl */
#define ROAD_SZ    0x2000    /* 0x300000-0x301fff TC0150ROD ram */
#define SPR_SZ     0x700     /* 0x400000-0x4006ff spriteram */
#define PAL_WORDS  0x1000    /* TC0110PCR internal palette */

static unsigned char rom_main[ROM_SZ], rom_sub[ROM_SZ];
static unsigned char ram_main[RAM_SZ], ram_sub[RAM_SZ], shared[SHARE_SZ];
static unsigned char scn_ram[SCN_SZ], scn_ctl[SCNCTL_SZ];
static unsigned char road_ram[ROAD_SZ], spriteram[SPR_SZ];
static unsigned short palette[PAL_WORDS];
static unsigned       pal_addr;         /* TC0110PCR address latch */
static int            road_palbank;     /* contcirc_out_w bits 6-7 */

/* ---- TC0040IOC (inputs/dsw), faithful to taitoio.cpp + contcirc input map ----
 * Two registers: m_port (selector, written via port_w @0x100003) and m_regs[]
 * (written via portreg_w @0x100001). Reads at 0x100001 go through contcirc's
 * input_bypass_r: port 8/9 = analog steering, else input port m_port.
 * Idle values computed for default DIPs (cockpit cabinet):
 *   0 DSWA=ff  1 DSWB=cf  2 IN0=13  3 IN1=1f  7 IN2=ff  steer=centered(0).
 * IN0 bits 2,3 = COIN2/COIN1 are ACTIVE_HIGH -> 0 at idle, which is exactly the
 * (IN0 & 0x0c)==0 boot self-test the sub performs. */
static unsigned char ioc_mport = 0;     /* m_port selector (port_w) */
static unsigned char ioc_regs[8];       /* m_regs (portreg_w) */
static unsigned ioc_read_selected(void){
    unsigned p = ioc_mport;
    if (p == 0x08 || p == 0x09){ unsigned steer = (0xff80 + 0x80) & 0xffff; return (p==0x08)?(steer&0xff):(steer>>8); }
    switch (p){
        case 0: return 0xff;            /* DSWA default */
        case 1: return 0xcf;            /* DSWB default */
        case 2: return 0x13;            /* IN0 idle (cockpit) */
        case 3: return 0x1f;            /* IN1 idle */
        case 4: return ioc_regs[4];     /* coin counters / lockout readback */
        case 7: return 0xff;            /* IN2 unused */
        default: return 0xff;
    }
}

/* ---- per-CPU dispatch ---- */
static int g_cpu;            /* 0 = main, 1 = sub */
static long illegal_count;
static unsigned long instr_count;

static unsigned rd8(unsigned a){
    a &= 0xffffff;
    if (g_cpu == 0){
        if (a < 0x040000) return rom_main[a];
        if (a >= 0x080000 && a < 0x084000) return ram_main[a-0x080000];
        if (a >= 0x084000 && a < 0x088000) return shared[a-0x084000];
        if (a >= 0x090000 && a < 0x090002) return 0xff;             /* out_w (write only) */
        if (a >= 0x100000 && a < 0x100008){                          /* TC0110PCR */
            unsigned off = (a>>1)&3;
            if (off==1){ unsigned v=palette[pal_addr & (PAL_WORDS-1)]; return (a&1)?(v&0xff):(v>>8); }
            return 0;
        }
        if (a >= 0x200000 && a < 0x210000) return scn_ram[a-0x200000];
        if (a >= 0x220000 && a < 0x220010) return scn_ctl[a-0x220000];
        if (a >= 0x300000 && a < 0x302000) return road_ram[a-0x300000];
        if (a >= 0x400000 && a < 0x400700) return spriteram[a-0x400000];
        return 0xff;
    } else {
        if (a < 0x040000) return rom_sub[a];
        if (a >= 0x080000 && a < 0x084000) return ram_sub[a-0x080000];
        if (a >= 0x084000 && a < 0x088000) return shared[a-0x084000];
        if (a == 0x100001) return ioc_read_selected();               /* input_bypass_r */
        if (a == 0x100003) return 0x00;                              /* watchdog_r (pet) */
        if (a == 0x200003) return 0x00;                              /* TC0140SYT master_comm_r (no Z80 yet) */
        return 0xff;
    }
}

static void wr8(unsigned a, unsigned v){
    a &= 0xffffff; v &= 0xff;
    if (g_cpu == 0){
        if (a >= 0x080000 && a < 0x084000){ ram_main[a-0x080000]=v; return; }
        if (a >= 0x084000 && a < 0x088000){ shared[a-0x084000]=v; return; }
        if (a == 0x090001){                                          /* contcirc_out_w */
            road_palbank = (v & 0xc0) >> 6;                          /* bit0 = Z80 reset (later) */
            return;
        }
        if (a >= 0x100000 && a < 0x100008){                          /* TC0110PCR */
            unsigned off = (a>>1)&3;
            if (off==0){ if(a&1) pal_addr=(pal_addr&0xff00)|v; else pal_addr=(pal_addr&0x00ff)|(v<<8); }
            else if (off==1){ unsigned idx=pal_addr&(PAL_WORDS-1); if(a&1) palette[idx]=(palette[idx]&0xff00)|v; else palette[idx]=(palette[idx]&0x00ff)|(v<<8); }
            return;
        }
        if (a >= 0x200000 && a < 0x210000){ scn_ram[a-0x200000]=v; return; }
        if (a >= 0x220000 && a < 0x220010){ scn_ctl[a-0x220000]=v; return; }
        if (a >= 0x300000 && a < 0x302000){ road_ram[a-0x300000]=v; return; }
        if (a >= 0x400000 && a < 0x400700){ spriteram[a-0x400000]=v; return; }
        return;
    } else {
        if (a >= 0x080000 && a < 0x084000){ ram_sub[a-0x080000]=v; return; }
        if (a >= 0x084000 && a < 0x088000){ shared[a-0x084000]=v; return; }
        if (a == 0x100001){ ioc_regs[ioc_mport & 7] = v; return; }   /* portreg_w (m_regs[m_port]) */
        if (a == 0x100003){ ioc_mport = v; return; }                 /* port_w (select m_port) */
        if (a == 0x200001){ return; }                                /* TC0140SYT master_port_w */
        if (a == 0x200003){ return; }                                /* TC0140SYT master_comm_w */
        return;
    }
}

/* ---- Musashi memory glue (big-endian) ---- */
unsigned int m68k_read_memory_8 (unsigned int a){ return rd8(a); }
unsigned int m68k_read_memory_16(unsigned int a){ return (rd8(a)<<8)|rd8(a+1); }
unsigned int m68k_read_memory_32(unsigned int a){ return ((unsigned)rd8(a)<<24)|((unsigned)rd8(a+1)<<16)|(rd8(a+2)<<8)|rd8(a+3); }
void m68k_write_memory_8 (unsigned int a, unsigned int v){ wr8(a,v); }
void m68k_write_memory_16(unsigned int a, unsigned int v){ wr8(a,v>>8); wr8(a+1,v); }
void m68k_write_memory_32(unsigned int a, unsigned int v){ wr8(a,v>>24); wr8(a+1,v>>16); wr8(a+2,v>>8); wr8(a+3,v); }
unsigned int m68k_read_disassembler_8 (unsigned int a){ return rd8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a){ return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a){ return m68k_read_memory_32(a); }

/* ---- callbacks ---- */
int int_ack(int level){ m68k_set_irq(0); return M68K_INT_ACK_AUTOVECTOR; (void)level; }
void instr_hook(unsigned int pc){ instr_count++; (void)pc; }
int illg_hook(int opcode){
    char buf[128]; unsigned pc = m68k_get_reg(NULL, M68K_REG_PPC);
    if (illegal_count < 12){ m68k_disassemble(buf, pc, M68K_CPU_TYPE_68000);
        fprintf(stderr, "  ILLEGAL cpu%d op=%04x pc=%06x : %s\n", g_cpu, opcode&0xffff, pc, buf); }
    illegal_count++; return 0;
}

/* ---- helpers ---- */
static int loadbin(const char*path, unsigned char*dst, int sz){
    FILE*f=fopen(path,"rb"); if(!f){perror(path);return -1;}
    int n=fread(dst,1,sz,f); fclose(f);
    if(n!=sz){fprintf(stderr,"%s: got %d want %d\n",path,n,sz);return -1;} return 0;
}
static long nz_words(const unsigned char*p,int n){ long c=0; for(int i=0;i+1<n;i+=2) if(p[i]||p[i+1]) c++; return c; }
static void dump(const char*path,const void*p,int n){ FILE*f=fopen(path,"wb"); if(f){fwrite(p,1,n,f);fclose(f);} }

int main(int argc, char**argv){
    int frames = (argc>1)?atoi(argv[1]):300;
    const char *base = "../01-rom-analysis/out";
    char p[256];
    snprintf(p,sizeof p,"%s/maincpu.bin",base); if(loadbin(p,rom_main,ROM_SZ)) return 1;
    snprintf(p,sizeof p,"%s/sub.bin",base);     if(loadbin(p,rom_sub,ROM_SZ))  return 1;

    m68k_init();
    m68k_set_int_ack_callback(int_ack);
    m68k_set_instr_hook_callback(instr_hook);
    m68k_set_illg_instr_callback(illg_hook);

    static unsigned char ctx_main[8192], ctx_sub[8192];
    unsigned csz = m68k_context_size();
    if (csz > sizeof ctx_main){ fprintf(stderr,"context too big: %u\n",csz); return 1; }

    g_cpu=0; m68k_set_cpu_type(M68K_CPU_TYPE_68000); m68k_pulse_reset(); m68k_get_context(ctx_main);
    g_cpu=1; m68k_set_cpu_type(M68K_CPU_TYPE_68000); m68k_pulse_reset(); m68k_get_context(ctx_sub);

    printf("contcirc step02 host: main PC=%06x  sub PC=%06x  ctx=%u  frames=%d\n",
        (g_cpu=0,m68k_set_context(ctx_main),m68k_get_reg(NULL,M68K_REG_PC)),
        (g_cpu=1,m68k_set_context(ctx_sub), m68k_get_reg(NULL,M68K_REG_PC)), csz, frames);

    const int CYC_FRAME = 12000000/60;   /* 200000 per cpu per frame */
    const int SLICES = 64;
    const int per = CYC_FRAME / SLICES;
    unsigned main_pc_min=~0u, main_pc_max=0, sub_pc_min=~0u, sub_pc_max=0;

    for (int fr=0; fr<frames; fr++){
        /* vblank: assert IRQ6 on both */
        g_cpu=0; m68k_set_context(ctx_main); m68k_set_irq(6); m68k_get_context(ctx_main);
        g_cpu=1; m68k_set_context(ctx_sub);  m68k_set_irq(6); m68k_get_context(ctx_sub);
        for (int s=0;s<SLICES;s++){
            g_cpu=0; m68k_set_context(ctx_main); m68k_execute(per); m68k_get_context(ctx_main);
            g_cpu=1; m68k_set_context(ctx_sub);  m68k_execute(per);  m68k_get_context(ctx_sub);
        }
        /* sample PCs near end of frame */
        g_cpu=0; m68k_set_context(ctx_main); { unsigned pc=m68k_get_reg(NULL,M68K_REG_PC); if(pc<main_pc_min)main_pc_min=pc; if(pc>main_pc_max)main_pc_max=pc; }
        g_cpu=1; m68k_set_context(ctx_sub);  { unsigned pc=m68k_get_reg(NULL,M68K_REG_PC); if(pc<sub_pc_min)sub_pc_min=pc; if(pc>sub_pc_max)sub_pc_max=pc; }
        if (fr==9 || fr==59 || fr==frames-1){
            g_cpu=0; m68k_set_context(ctx_main); unsigned mpc=m68k_get_reg(NULL,M68K_REG_PC);
            g_cpu=1; m68k_set_context(ctx_sub);  unsigned spc=m68k_get_reg(NULL,M68K_REG_PC);
            printf("  frame %4d: mainPC=%06x subPC=%06x  scnNZ=%ld sprNZ=%ld roadNZ=%ld palNZ=%ld  illegal=%ld\n",
                fr, mpc, spc, nz_words(scn_ram,SCN_SZ), nz_words(spriteram,SPR_SZ),
                nz_words(road_ram,ROAD_SZ), nz_words((unsigned char*)palette,PAL_WORDS*2), illegal_count);
        }
    }

    printf("\n== summary after %d frames ==\n", frames);
    printf("  instructions retired : %lu\n", instr_count);
    printf("  illegal ops          : %ld\n", illegal_count);
    printf("  main PC range        : %06x .. %06x\n", main_pc_min, main_pc_max);
    printf("  sub  PC range        : %06x .. %06x\n", sub_pc_min, sub_pc_max);
    printf("  TC0100SCN nonzero w  : %ld / %d\n", nz_words(scn_ram,SCN_SZ), SCN_SZ/2);
    printf("  spriteram nonzero w  : %ld / %d\n", nz_words(spriteram,SPR_SZ), SPR_SZ/2);
    printf("  road_ram  nonzero w  : %ld / %d\n", nz_words(road_ram,ROAD_SZ), ROAD_SZ/2);
    printf("  palette   nonzero w  : %ld / %d\n", nz_words((unsigned char*)palette,PAL_WORDS*2), PAL_WORDS);
    printf("  road_palbank=%d  ioc_mport=%02x\n", road_palbank, ioc_mport);

    system("mkdir -p out/state");
    dump("out/state/scn_ram.bin",  scn_ram,  SCN_SZ);
    dump("out/state/scn_ctrl.bin", scn_ctl,  SCNCTL_SZ);
    dump("out/state/spriteram.bin",spriteram,SPR_SZ);
    dump("out/state/road_ram.bin", road_ram, ROAD_SZ);
    dump("out/state/palette.bin",  palette,  PAL_WORDS*2);
    dump("out/state/ram_main.bin", ram_main, RAM_SZ);
    dump("out/state/ram_sub.bin",  ram_sub,  RAM_SZ);
    dump("out/state/shared.bin",   shared,   SHARE_SZ);
    printf("  state dumped to out/state/\n");
    return 0;
}
