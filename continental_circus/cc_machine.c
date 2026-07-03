/* cc_machine.c -- Continental Circus dual MC68000 machine model (Amiga build).
 * Port of steps/02-host-ref/host.c onto the Musashi core, ROMs embedded
 * (cc_romdata.s), exposing the video state in cc_state.h for cc_render.c.
 * Both 68000s free-run from reset, share 0x084000-0x087fff, take IRQ6 at vblank,
 * and drive the Z80/YM2610 sound path via TC0140SYT. */
#include "m68k.h"
#include "cc_state.h"
#include <string.h>

extern uint8_t cc_rom_main[];  /* 0x40000 maincpu */
extern uint8_t cc_rom_sub[];   /* 0x40000 sub */

/* ---- video/CPU RAM (shared via cc_state.h) ---- */
uint8_t  cc_scn[0x14000];
uint8_t  cc_scnctl[0x10];
uint8_t  cc_road[0x2000];
uint8_t  cc_spr[0x700];
uint16_t cc_pal[0x1000];
int      cc_road_palbank = 3;

static uint8_t ram_main[0x4000], ram_sub[0x4000], shared[0x4000];
static unsigned pal_addr;

/* ---- TC0040IOC inputs (default idle, overridable via cc_set_inputs) ---- */
static uint8_t io_dswa=0xff, io_dswb=0xcf, io_in0=0x13, io_in1=0x1f, io_in2=0xff;
static int     io_steer=0;            /* signed-ish, centered 0 */
static uint8_t ioc_mport=0, ioc_regs[8];

void cc_set_inputs(uint8_t in0, uint8_t in1, uint8_t dswa, uint8_t dswb, int steer){
    io_in0=in0; io_in1=in1; io_dswa=dswa; io_dswb=dswb; io_steer=steer;
}

static unsigned ioc_read_selected(void){
    unsigned p=ioc_mport;
    if(p==0x08||p==0x09){ unsigned s=(0xff80 + (0x80 + io_steer)) & 0xffff; return (p==0x08)?(s&0xff):(s>>8); }
    switch(p){ case 0:return io_dswa; case 1:return io_dswb; case 2:return io_in0;
               case 3:return io_in1; case 4:return ioc_regs[4]; case 7:return io_in2; default:return 0xff; }
}

/* ---- per-CPU memory dispatch ---- */
static int g_cpu;   /* 0=main, 1=sub */

static unsigned rd8(unsigned a){
    a&=0xffffff;
    if(g_cpu==0){
        if(a<0x040000) return cc_rom_main[a];
        if(a>=0x080000&&a<0x084000) return ram_main[a-0x080000];
        if(a>=0x084000&&a<0x088000) return shared[a-0x084000];
        if(a>=0x100000&&a<0x100008){ unsigned off=(a>>1)&3; if(off==1){unsigned v=cc_pal[pal_addr&0xfff]; return (a&1)?(v&0xff):(v>>8);} return 0; }
        if(a>=0x200000&&a<0x210000) return cc_scn[a-0x200000];
        if(a>=0x220000&&a<0x220010) return cc_scnctl[a-0x220000];
        if(a>=0x300000&&a<0x302000) return cc_road[a-0x300000];
        if(a>=0x400000&&a<0x400700) return cc_spr[a-0x400000];
        return 0xff;
    } else {
        if(a<0x040000) return cc_rom_sub[a];
        if(a>=0x080000&&a<0x084000) return ram_sub[a-0x080000];
        if(a>=0x084000&&a<0x088000) return shared[a-0x084000];
        if(a==0x100001) return ioc_read_selected();
        if(a==0x100003) return 0x00;     /* watchdog pet */
        if(a==0x200003) return cc_syt_master_comm_r();   /* TC0140SYT master comm */
        return 0xff;
    }
}
static void wr8(unsigned a, unsigned v){
    a&=0xffffff; v&=0xff;
    if(g_cpu==0){
        if(a>=0x080000&&a<0x084000){ ram_main[a-0x080000]=v; return; }
        if(a>=0x084000&&a<0x088000){ shared[a-0x084000]=v; return; }
        if(a==0x090001){
            cc_audio_reset_line(v & 0x01);          /* MAME contcirc_out_w bit0: Z80 run/reset */
            cc_road_palbank=(v&0xc0)>>6;
            return;
        }
        if(a>=0x100000&&a<0x100008){ unsigned off=(a>>1)&3;
            if(off==0){ if(a&1)pal_addr=(pal_addr&0xff00)|v; else pal_addr=(pal_addr&0x00ff)|(v<<8); }
            else if(off==1){ unsigned i=pal_addr&0xfff; if(a&1)cc_pal[i]=(cc_pal[i]&0xff00)|v; else cc_pal[i]=(cc_pal[i]&0x00ff)|(v<<8); }
            return; }
        if(a>=0x200000&&a<0x210000){ cc_scn[a-0x200000]=v; return; }
        if(a>=0x220000&&a<0x220010){ cc_scnctl[a-0x220000]=v; return; }
        if(a>=0x300000&&a<0x302000){ cc_road[a-0x300000]=v; return; }
        if(a>=0x400000&&a<0x400700){ cc_spr[a-0x400000]=v; return; }
        return;
    } else {
        if(a>=0x080000&&a<0x084000){ ram_sub[a-0x080000]=v; return; }
        if(a>=0x084000&&a<0x088000){ shared[a-0x084000]=v; return; }
        if(a==0x100001){ ioc_regs[ioc_mport&7]=v; return; }
        if(a==0x100003){ ioc_mport=v; return; }
        if(a==0x200001){ cc_syt_master_port_w(v); return; }   /* TC0140SYT master port */
        if(a==0x200003){ cc_syt_master_comm_w(v); return; }   /* TC0140SYT master comm */
        return;
    }
}

/* Musashi big-endian glue */
unsigned int m68k_read_memory_8 (unsigned int a){ return rd8(a); }
unsigned int m68k_read_memory_16(unsigned int a){ return (rd8(a)<<8)|rd8(a+1); }
unsigned int m68k_read_memory_32(unsigned int a){ return ((unsigned)rd8(a)<<24)|((unsigned)rd8(a+1)<<16)|(rd8(a+2)<<8)|rd8(a+3); }
void m68k_write_memory_8 (unsigned int a, unsigned int v){ wr8(a,v); }
void m68k_write_memory_16(unsigned int a, unsigned int v){ wr8(a,v>>8); wr8(a+1,v); }
void m68k_write_memory_32(unsigned int a, unsigned int v){ wr8(a,v>>24); wr8(a+1,v>>16); wr8(a+2,v>>8); wr8(a+3,v); }
unsigned int m68k_read_disassembler_8 (unsigned int a){ return rd8(a); }
unsigned int m68k_read_disassembler_16(unsigned int a){ return m68k_read_memory_16(a); }
unsigned int m68k_read_disassembler_32(unsigned int a){ return m68k_read_memory_32(a); }

static int cc_int_ack(int level){ (void)level; m68k_set_irq(0); return M68K_INT_ACK_AUTOVECTOR; }

static unsigned char ctx_main[1024], ctx_sub[1024];

void cc_machine_init(void){
    memset(ram_main,0,sizeof ram_main); memset(ram_sub,0,sizeof ram_sub); memset(shared,0,sizeof shared);
    memset(cc_scn,0,sizeof cc_scn); memset(cc_road,0,sizeof cc_road); memset(cc_spr,0,sizeof cc_spr);
    memset(cc_pal,0,sizeof cc_pal); memset(cc_scnctl,0,sizeof cc_scnctl);
    m68k_init();
    m68k_set_int_ack_callback(cc_int_ack);
    g_cpu=0; m68k_set_cpu_type(M68K_CPU_TYPE_68000); m68k_pulse_reset(); m68k_get_context(ctx_main);
    g_cpu=1; m68k_set_cpu_type(M68K_CPU_TYPE_68000); m68k_pulse_reset(); m68k_get_context(ctx_sub);
}

void cc_machine_run_frame(void){
    const int CYC_FRAME = 12000000/60;     /* 200000 per cpu per frame */
    const int SLICES = 8;
    const int per = CYC_FRAME / SLICES;
    g_cpu=0; m68k_set_context(ctx_main); m68k_set_irq(6); m68k_get_context(ctx_main);
    g_cpu=1; m68k_set_context(ctx_sub);  m68k_set_irq(6); m68k_get_context(ctx_sub);
    for(int s=0;s<SLICES;s++){
        g_cpu=0; m68k_set_context(ctx_main); m68k_execute(per); m68k_get_context(ctx_main);
        g_cpu=1; m68k_set_context(ctx_sub);  m68k_execute(per);  m68k_get_context(ctx_sub);
        { extern void cc_audio_run_slice(int i, int n);   /* Z80 rides along at 1/SLICES-frame
                                                           * latency (SYT handshake fix) */
          cc_audio_run_slice(s, SLICES); }
    }
}
