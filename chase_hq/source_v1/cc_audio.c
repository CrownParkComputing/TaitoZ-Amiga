#include <stdlib.h>
#include <stdio.h>
/* cc_audio.c -- Continental Circus sound: Z80 audiocpu + TC0140SYT mailbox +
 * YM2610 (FM + SSG + ADPCM-A + ADPCM-B/Delta-T).
 * Rendered to signed-8-bit mono PCM for Paula (cc_audio_amiga.c).
 *
 * SYT protocol ported from MAME shared/taitosnd.cpp; Z80 map from taito_z.cpp
 * z80_sound_map (0x0000-3fff rom, 0x4000-7fff bank, 0xc000-dfff ram, 0xe000-3
 * YM2610, 0xe200/1 SYT slave, 0xf200 bank). YM2610 @ 8MHz, Z80 @ 4MHz. */
#include "z80emu.h"
#include "cc_state.h"
#include <string.h>
#ifndef HAS_YM2610
#define HAS_YM2610 1
#endif
#include "driver.h"   /* OPN core types: UINT8/INT16/INT32/... + logerror */
#include "fm.h"

extern uint8_t cc_rom_audio[];   /* 0x10000 audiocpu (b33-30) */
extern uint8_t cc_adpcma[];      /* 0x180000 ADPCM-A samples */
extern uint8_t cc_adpcmb[];      /* 0x080000 ADPCM-B (delta-T) samples */

#define SND_SR   CC_SND_SR       /* shared output rate (cc_state.h) */

static MY_LITTLE_Z80 z;
static int z_ok, ym_ok;

#ifdef CHQ_AUDIO_NATIVE
/* ================= NATIVE audio Z80 (Rust static recompile) ==================
 * -DCHQ_AUDIO_NATIVE=1 replaces the z80emu interpreter (cores/z80.c) with the
 * Rust-generated native transcode of the sound driver: chq_aud_run() from
 * build/chq_audio_native/gencrate (emitted by tools/z80_recompiler
 * recompile_chasehq_audio from data/audiocpu.bin + the tools/z80cov coverage).
 * The default (interpreter) build is unchanged. Validation gate: the host build
 * of this mode (build_host_native.sh -> /tmp/cc_hosttest_native) must print the
 * SAME "RAW:" counters + YM-write FNV hash as the interpreter build.
 *
 * The struct below is the recompiler prelude's #[repr(C)] Z80State: identical
 * member order/sizes, so the layout matches on both m68k (mem at offset 44,
 * size 48) and the 64-bit host (mem at 48, size 56). zn.mem aliases z.memory
 * (fixed ROM copy 0x0000-0x7fff + work RAM 0xc000-0xdfff); banked DATA reads
 * 0x4000-0x7fff go through chq_aud_bank_rd() below (live bank + 64KB ROM, not
 * the memcpy'd window). */
typedef struct {
    uint8_t  b, c, d, e, h, l, a, f;
    uint8_t  ixh, ixl, iyh, iyl;
    uint16_t sp, pc;
    uint16_t bc_alt, de_alt, hl_alt, af_alt;
    uint8_t  i, r, iff1, iff2;
    uint8_t  im, halted, stop, pad0;
    uint32_t cycles, budget;
    uint16_t badpc, pad1;
    uint8_t *mem;
} ChqZ80Native;
static ChqZ80Native zn;
extern void chq_aud_run(ChqZ80Native *s);

/* Layout contract with the Rust crate's #[repr(C)] Z80State: on m68k (2-byte
 * struct alignment, 32-bit pointers) mem must sit at offset 44 and the struct
 * be 48 bytes.  A silent layout drift here would corrupt state on target while
 * every host gate stays green -- fail the build instead. */
#ifdef __m68k__
typedef char chq_zn_mem_off_check[__builtin_offsetof(ChqZ80Native, mem) == 44 ? 1 : -1];
typedef char chq_zn_size_check[sizeof(ChqZ80Native) == 48 ? 1 : -1];
#endif

/* mirrors cores/z80.c Z80Reset(): AF=SP=0xffff, pc/i/iff1/iff2/im=0; other regs,
 * R, and RAM untouched (also used on TC0140SYT reset-line release). */
static void zn_reset(void){
    zn.a = 0xff; zn.f = 0xff; zn.sp = 0xffff;
    zn.pc = 0; zn.i = 0; zn.iff1 = 0; zn.iff2 = 0; zn.im = 0;
    zn.halted = 0; zn.stop = 0;
}
#define Z80_RESET_NOW() zn_reset()
#else
#define Z80_RESET_NOW() Z80Reset(&z.state)
#endif

/* ================= AY-3-8910 SSG (the YM2610's 3 square channels) ============ */
typedef struct { unsigned char reg[16]; unsigned tcnt[3],tper[3]; unsigned char tout[3];
    unsigned ncnt,nper,nrng; unsigned char nout; unsigned ecnt,eper; unsigned char estep,eatt,evol; } AY;
static const unsigned char ay_vol[16]={0,1,2,3,4,5,7,10,13,18,24,30,36,40,42,42};
static void ay_reset(AY*a){ memset(a,0,sizeof*a); a->nrng=1; a->tper[0]=a->tper[1]=a->tper[2]=1; a->nper=1; a->eper=1; }
static void ay_write(AY*a,unsigned r,unsigned char v){ r&=15; a->reg[r]=v;
    switch(r){ case 0:case 1:a->tper[0]=(a->reg[0]|((a->reg[1]&0x0f)<<8));if(!a->tper[0])a->tper[0]=1;break;
        case 2:case 3:a->tper[1]=(a->reg[2]|((a->reg[3]&0x0f)<<8));if(!a->tper[1])a->tper[1]=1;break;
        case 4:case 5:a->tper[2]=(a->reg[4]|((a->reg[5]&0x0f)<<8));if(!a->tper[2])a->tper[2]=1;break;
        case 6:a->nper=v&0x1f;if(!a->nper)a->nper=1;break;
        case 11:case 12:a->eper=(a->reg[11]|(a->reg[12]<<8));if(!a->eper)a->eper=1;break;
        case 13:a->estep=0;a->ecnt=0;a->eatt=(v>>2)&1;a->evol=a->eatt?0:15;break; } }
static void ay_tick(AY*a){ for(int c=0;c<3;c++){ if(++a->tcnt[c]>=a->tper[c]){a->tcnt[c]=0;a->tout[c]^=1;} }
    if(++a->ncnt>=a->nper*2u){ a->ncnt=0; a->nrng=(a->nrng>>1)|(((a->nrng^(a->nrng>>3))&1)<<16); a->nout=a->nrng&1; }
    if(++a->ecnt>=a->eper*32u){ a->ecnt=0; unsigned char cont=a->reg[13]&8,alt=a->reg[13]&2,hold=a->reg[13]&1;
        if(a->estep<16){a->evol=a->eatt?a->estep:15-a->estep;a->estep++;}
        else if(!cont){a->evol=0;} else if(hold){a->evol=(a->eatt^(alt?1:0))?15:0;}
        else{ if(alt)a->eatt^=1; a->estep=0; a->evol=a->eatt?0:15; } } }
static int ay_amp(AY*a){ int s=0; unsigned char mix=a->reg[7];
    for(int c=0;c<3;c++){ int tone=(mix>>c)&1,noise=(mix>>(c+3))&1; int on=(tone||a->tout[c])&&(noise||a->nout);
        if(!on)continue; int v=(a->reg[8+c]&0x10)?a->evol:(a->reg[8+c]&0x0f); s+=ay_vol[v&15]; } return s; }

/* AY8910* shim fm.c calls for the YM2610 SSG half (chip 0 only) */
static AY ssg0; static int ssg_lat;
int  ay8910_index_ym = 0;
void AY8910Write(int chip,int addr,int v){ (void)chip; if(addr==0) ssg_lat=v&15; else ay_write(&ssg0, ssg_lat, (unsigned char)v); }
int  AY8910Read(int chip){ (void)chip; return ssg0.reg[ssg_lat]; }
void AY8910Reset(int chip){ (void)chip; ay_reset(&ssg0); ssg_lat=0; }
void AY8910_set_clock(int chip,int clock){ (void)chip;(void)clock; }
double timer_get_time(void){ return 0.0; }
void   BurnYM2610UpdateRequest(void){ }

/* ================= TC0140SYT (master = sub side, slave = this Z80) =========== */
#define P01 0x01
#define P23 0x02
#define P01M 0x04
#define P23M 0x08
static uint8_t mainmode, submode, syt_status;
static uint8_t slavedata[8], masterdata[8];
static int nmi_enabled = 0, nmi_pending, z_reset_held;   /* chip resets NMI disabled */
int cc_dbg_iffever=0, cc_dbg_intok=0;
int cc_dbg_ymw=0, cc_dbg_sytc=0, cc_dbg_nmi=0, cc_dbg_slaver=0, cc_dbg_nmiset=0, cc_dbg_tmr=0, cc_dbg_irq=0, cc_dbg_nmien=1, cc_dbg_ymok_v=0;
/* rolling FNV-1a over every YM2610 (addr&3,val) write pair, BOTH engine modes:
 * the interpreter-vs-native validation gate requires bit equality (cc_hosttest
 * prints it on the RAW: line). */
unsigned cc_dbg_ymhash = 2166136261u;
#ifdef CHQ_AUDIO_NATIVE
void cc_audio_dbg(int*pc,int*iff,int*im){ *pc=zn.pc; *iff=zn.iff1; *im=zn.im; cc_dbg_ymok_v=ym_ok; }
#else
void cc_audio_dbg(int*pc,int*iff,int*im){ *pc=z.state.pc; *iff=z.state.iff1; *im=z.state.im; cc_dbg_ymok_v=ym_ok; }
#endif

#ifdef __amigaos__
#define CCSND_TRACE 0
#else
#define CCSND_TRACE getenv("CCSND")
#endif

static void syt_update_nmi(void){ nmi_pending = ((syt_status & (P01|P23)) && nmi_enabled) ? 1 : 0; if(nmi_pending)cc_dbg_nmiset++; cc_dbg_nmien=nmi_enabled; }

#ifndef __amigaos__
/* CHQ_SYTLOG=<file>: record every master-side SYT write with a Z80-cycle
 * timestamp, so chq_covplay can replay the exact command stream into an
 * isolated single-stepped Z80 for recompiler coverage (decoupled from the
 * machine's phase-sensitive coin windows). */
static FILE *sytlog;
static int sytlog_tried;
unsigned long cc_z80_cyc_total;
static void sytlog_put(int is_comm, uint8_t d){
    if(!sytlog_tried){ sytlog_tried=1; const char*fn=getenv("CHQ_SYTLOG"); if(fn) sytlog=fopen(fn,"w"); }
    if(sytlog){ fprintf(sytlog, "%lu %d %02x\n", cc_z80_cyc_total, is_comm, d); fflush(sytlog); }
}
#define SYTLOG(c,d) sytlog_put(c,d)
#else
#define SYTLOG(c,d)
#endif

void cc_syt_master_port_w(uint8_t d){ SYTLOG(0,d); mainmode = d & 0x0f; }
void cc_syt_master_comm_w(uint8_t d){
    SYTLOG(1,d);
    cc_dbg_sytc++;
    d &= 0x0f;
    switch(mainmode){
        case 0: slavedata[mainmode++]=d; break;
        case 1: slavedata[mainmode++]=d; syt_status|=P01; cc_dbg_sytc++; if(CCSND_TRACE)fprintf(stderr,"CMD %02x%02x\n",slavedata[0],slavedata[1]); syt_update_nmi(); break;
        case 2: slavedata[mainmode++]=d; break;
        case 3: slavedata[mainmode++]=d; syt_status|=P23; syt_update_nmi(); break;
        case 4: { int hold=(d!=0);   /* MAME: reset_cb(data?ASSERT:CLEAR) -- hold when d!=0 */
                  SYTLOG(2, (uint8_t)!hold);
                  if(z_reset_held && !hold && z_ok) Z80_RESET_NOW();   /* release -> reset+run */
                  z_reset_held=hold; break; }
    }
}

void cc_audio_reset_line(int clear_line){
    int hold = clear_line ? 0 : 1;
    SYTLOG(2, (uint8_t)clear_line);
    if(z_reset_held && !hold && z_ok) Z80_RESET_NOW();
    z_reset_held = hold;
}

uint8_t cc_syt_master_comm_r(void){
    uint8_t r=0;
    switch(mainmode){
        case 0: r=masterdata[mainmode++]; break;
        case 1: r=masterdata[mainmode]; syt_status&=~P01M; mainmode++; break;
        case 2: r=masterdata[mainmode++]; break;
        case 3: r=masterdata[mainmode]; syt_status&=~P23M; mainmode++; break;
        case 4: r=syt_status; break;
    }
    return r;
}
static void syt_slave_port_w(uint8_t d){ submode = d & 0x0f; }
static void syt_slave_comm_w(uint8_t d){
    d &= 0x0f;
    switch(submode){
        case 0: masterdata[submode++]=d; break;
        case 1: masterdata[submode++]=d; syt_status|=P01M; break;
        case 2: masterdata[submode++]=d; break;
        case 3: masterdata[submode++]=d; syt_status|=P23M; break;
        case 5: nmi_enabled=0; syt_update_nmi(); break;
        case 6: nmi_enabled=1; syt_update_nmi(); break;
    }
}
static uint8_t syt_slave_comm_r(void){
    uint8_t r=0;
    switch(submode){
        case 0: r=slavedata[submode++]; break;
        case 1: r=slavedata[submode]; syt_status&=~P01; submode++; cc_dbg_slaver++; syt_update_nmi(); break;
        case 2: r=slavedata[submode++]; break;
        case 3: r=slavedata[submode]; syt_status&=~P23; submode++; syt_update_nmi(); break;
        case 4: r=syt_status; break;
    }
    return r;
}

/* ================= YM2610 timer -> Z80 IRQ ================================== */
/* INTEGER timer (no soft-float on Amiga): period in Z80 cycles. TimerBase = TimerPres/clock
 * = 144/8MHz s/count; in Z80 cycles (4MHz): 72/count for timer A, *16 for timer B. */
static int tmr_per_cyc[2], tmr_acc_cyc[2];
static volatile int z_int;
int cc_dbg_tover=0;
int cc_dbg_ymraw=0, cc_dbg_ssgraw=0, cc_dbg_yminit=-99, cc_dbg_keyon=0, cc_dbg_adpcma=0, cc_dbg_deltat=0;
double cc_dbg_tper(void){ return (double)tmr_per_cyc[0]; }
/* Z80 UNDERCLOCK divisor: execute 4MHz/div Z80 cycles per wall second while
 * keeping the YM timer INTERRUPT RATE at true wall time (periods divide too).
 * Rationale (host PC histogram, race): ~85% of all driver instructions are
 * null main-loop iterations polling work flags; the sequencing itself is
 * timer-INT-driven.  Underclocking cuts the spin without touching tempo
 * (INTs/sec unchanged) or command handling (NMI immediate; flag processing
 * latency stays sub-millisecond).  div=1 is the faithful 4MHz.
 * On the HUD this is the Z number: it scales ~1/div. */
#ifndef CHQ_Z80_DIV_DEFAULT
#define CHQ_Z80_DIV_DEFAULT 1
#endif
int cc_z80_div = CHQ_Z80_DIV_DEFAULT;

/* Timer periods -> Z80 cycles.  count*72 for BOTH timers: fm.c already
 * pre-shifts timer B's count by <<4 (TBC<<4), so the old c==1 factor of 1152
 * double-applied the x16 = timer B 16x slow (starved Z80<->SYT pipeline).
 * Proven on Continental Circus 2026-07-02 (SYTcmd 23->1824, FMkeyon 4->733).
 * (Chase HQ's driver only arms timer A -- host counters identical either way.)
 * Divided by cc_z80_div so INT wall-rate is invariant under the underclock. */
static void fm_timer(int n,int c,int count,double step){ (void)n;(void)step; if(c<0||c>1)return;
    if(count<=0){ tmr_per_cyc[c]=0; }
    else { int p=(count*72)/cc_z80_div; if(p<1)p=1; tmr_per_cyc[c]=p; tmr_acc_cyc[c]=0; cc_dbg_tmr++; } }
static void fm_irq(int n,int irq){ (void)n; z_int = irq; }

/* ================= Z80 bank + memory glue ================================== */
static int cur_bank;   /* live 0xf200 bank value (coverage + native bridge) */
static void set_bank(int b){ b&=7; if(b>3)b&=3; cur_bank=b; memcpy(z.memory+0x4000, cc_rom_audio + b*0x4000, 0x4000); }

/* One YM2610 device-write funnel shared by BOTH engine modes (interpreter's
 * machine_wr and the native transcode's chq_aud_ym_write): debug key-on
 * counters + validation hash + the actual chip write stay identical. */
static void ym_dev_write(unsigned a, unsigned char v){
    cc_dbg_ymw++;
    cc_dbg_ymhash = (cc_dbg_ymhash ^ (a & 3)) * 16777619u;   /* FNV-1a (addr,val) */
    cc_dbg_ymhash = (cc_dbg_ymhash ^ v) * 16777619u;
#ifndef __amigaos__
    /* CHQ_YMLOG=<file>: host-only trace of every YM write (seq, z80-cycle stamp,
     * addr, val) -- diffing interpreter vs native logs pinpoints the first
     * divergent write when the RAW ymHash gate fails. */
    { static FILE *ymlog; static int ymlog_tried;
      if(!ymlog_tried){ ymlog_tried=1; const char*fn=getenv("CHQ_YMLOG"); if(fn) ymlog=fopen(fn,"w"); }
      if(ymlog) fprintf(ymlog, "%d %lu %u %02x\n", cc_dbg_ymw, cc_z80_cyc_total, a&3, v); }
#endif
    { static int la=0, lb=0;
      if((a&3)==0) la=v;                                   /* port-A addr latch */
      else if((a&3)==1){ if(la==0x28 && (v&0xf0)) cc_dbg_keyon++;     /* FM key-on */
      } else if((a&3)==2) lb=v;                                      /* port-B addr latch */
      else if((a&3)==3 && lb==0x00 && v<0x80 && (v&0x3f)) cc_dbg_adpcma++; /* ADPCM-A key-on */
      if((a&3)==1 && la==0x10 && (v&0x80)) cc_dbg_deltat++;            /* ADPCM-B/Delta-T start */
    }
    if(ym_ok) YM2610Write(0, a&3, v);
}

unsigned char machine_rd(MY_LITTLE_Z80 *zz, unsigned int a){
    a &= 0xffff;
    if(a>=0xe000 && a<=0xe003) return ym_ok ? YM2610Read(0, a&3) : 0;
    if(a==0xe201) return syt_slave_comm_r();
    if(a==0xe200) return 0;
    return zz->memory[a];
}
void machine_wr(MY_LITTLE_Z80 *zz, unsigned int a, unsigned char v){
    a &= 0xffff;
    if(a>=0xc000 && a<0xe000){ zz->memory[a]=v; return; }
    if(a>=0xe000 && a<=0xe003){ ym_dev_write(a, v); return; }
    if(a==0xe200){ syt_slave_port_w(v); return; }
    if(a==0xe201){ syt_slave_comm_w(v); return; }
    if(a>=0xe400 && a<=0xe403) return;       /* pan */
    if(a==0xf200){ set_bank(v); return; }
}
unsigned char in_impl(MY_LITTLE_Z80 *zz, int p){ (void)zz;(void)p; return 0xff; }
void          out_impl(MY_LITTLE_Z80 *zz, int p, unsigned char x){ (void)zz;(void)p;(void)x; }

#ifdef CHQ_AUDIO_NATIVE
/* ---- C hooks the Rust transcode's rd()/wr() call (mirror machine_rd/machine_wr,
 * routed through the SAME statics/counters so validation counters keep working) ---- */
uint8_t chq_aud_bank_rd(uint32_t a){     /* 0x4000-0x7fff banked DATA window */
    return cc_rom_audio[((uint32_t)(cur_bank & 3) << 14) + (a - 0x4000u)];
}
uint8_t chq_aud_ym_read(uint32_t a){ return ym_ok ? (uint8_t)YM2610Read(0, (int)(a & 3)) : 0; }
void    chq_aud_ym_write(uint32_t a, uint8_t v){ ym_dev_write((unsigned)a, v); }
uint8_t chq_aud_syt_read(void){ return syt_slave_comm_r(); }
void    chq_aud_syt_port_w(uint8_t v){ syt_slave_port_w(v); }
void    chq_aud_syt_comm_w(uint8_t v){ syt_slave_comm_w(v); }
void    chq_aud_bank_w(uint8_t v){ set_bank(v); }

/* native bus write for C-side interrupt injection (stack pushes): same dispatch
 * as the transcode's wr() -- the driver stack lives in 0xc000-0xdfff RAM. */
static void zn_wr8(unsigned a, unsigned char v){
    a &= 0xffff;
    if((a >= 0x8000 && a < 0xa000) || (a >= 0xc000 && a < 0xe000)){ z.memory[a] = v; return; }
    if(a >= 0xe000 && a <= 0xe003){ ym_dev_write(a, v); return; }
    if(a == 0xe200){ syt_slave_port_w(v); return; }
    if(a == 0xe201){ syt_slave_comm_w(v); return; }
    if(a == 0xf200){ set_bank(v); return; }
}
static void zn_push_pc(void){
    zn.sp = (uint16_t)(zn.sp - 2);
    zn_wr8(zn.sp, (unsigned char)zn.pc);
    zn_wr8((uint16_t)(zn.sp + 1), (unsigned char)(zn.pc >> 8));
}
/* mirrors cores/z80.c Z80NonMaskableInterrupt(): iff2=iff1, iff1=0, R+1, push PC,
 * PC=0x66. (pc already points past a caught HALT, exactly like the interpreter.) */
static void zn_nmi(void){
    zn.iff2 = zn.iff1; zn.iff1 = 0;
    zn.r = (uint8_t)((zn.r & 0x80) | ((zn.r + 1) & 0x7f));
    zn_push_pc(); zn.pc = 0x0066; zn.halted = 0;
}
/* mirrors cores/z80.c Z80Interrupt() IM1 (this driver runs IM 1): honoured only
 * when iff1 is set; iff1=iff2=0, R+1, push PC, PC=0x38 (RST 38h). */
static void zn_int38(void){
    if(!zn.iff1) return;
    zn.iff1 = 0; zn.iff2 = 0;
    zn.r = (uint8_t)((zn.r & 0x80) | ((zn.r + 1) & 0x7f));
    zn_push_pc(); zn.pc = 0x0038; zn.halted = 0;
}
/* One interpreter-equivalent Z80Emulate(run) slice: fresh cycle count against a
 * `run`-cycle budget (overshoot discarded, like the interpreter caller which
 * ignores Z80Emulate's return). A caught HALT ends the slice with pc already
 * past the HALT -- cc_audio_run_cycles ignores the interpreter's HALT status the
 * same way, so execution simply resumes next slice. */
static void zn_run_slice(int run){
    zn.cycles = 0; zn.budget = (uint32_t)run;
    while(zn.cycles < zn.budget){
        uint32_t before = zn.cycles;
        chq_aud_run(&zn);
        if(zn.stop){ zn.halted = 0; zn.stop = 0; break; }   /* HALT: slice over */
        if(zn.cycles == before) break;                      /* safety: no progress */
    }
}
#endif

/* ================= public API ============================================== */
void cc_audio_init(void){
#ifndef __amigaos__
    { const char *e = getenv("CHQ_Z80_DIV");
      if(e){ int d = atoi(e); if(d >= 1 && d <= 8) cc_z80_div = d; } }
#endif
    memset(&z, 0, sizeof z);
    memcpy(z.memory, cc_rom_audio, 0x8000);
    set_bank(0);
    if(!ym_ok){
        void *pa=cc_adpcma; int sa=0x180000; void *pb=cc_adpcmb; int sb=0x80000;
        extern int cc_dbg_yminit;
        cc_dbg_yminit = YM2610Init(1, 0, 8000000, SND_SR, &pa,&sa,&pb,&sb, fm_timer, fm_irq);
        if(cc_dbg_yminit==0) ym_ok=1;
    }
    if(ym_ok) YM2610ResetChip(0);
    ay_reset(&ssg0); ssg_lat=0;
#ifdef CHQ_AUDIO_NATIVE
    memset(&zn, 0, sizeof zn);
    zn.mem = z.memory;
    zn_reset();
#else
    Z80Reset(&z.state);
#endif
    mainmode=submode=syt_status=0; nmi_enabled=1; nmi_pending=0; z_reset_held=1;
    tmr_per_cyc[0]=tmr_per_cyc[1]=tmr_acc_cyc[0]=tmr_acc_cyc[1]=0; z_int=0;
    z_ok=1;
}

#ifndef __amigaos__
/* HOST-ONLY per-instruction trace sink shared by both engines: the interpreter
 * feeds it via z80_trace_pc (build z80.c with -DZ80_TRACE_PC), the native via
 * chq_aud_trace (crate generated with CHQAUD_TRACE=1). CHQ_ITRACE=<file> +
 * CHQ_ITRACE_LO/HI=<slice range> log pc-per-instruction for those slices --
 * diffing the two logs pinpoints a cycle-cost mismatch to the exact opcode. */
static long cc_dbg_sliceno;
static void itrace_log(unsigned pc){
    static FILE *itf; static int itf_tried; static long lo=-1, hi=-1;
    if(!itf_tried){ itf_tried=1; const char*fn=getenv("CHQ_ITRACE");
        if(fn){ itf=fopen(fn,"w");
            lo = getenv("CHQ_ITRACE_LO") ? atol(getenv("CHQ_ITRACE_LO")) : 0;
            hi = getenv("CHQ_ITRACE_HI") ? atol(getenv("CHQ_ITRACE_HI")) : 0x7fffffffL; } }
    if(itf && cc_dbg_sliceno >= lo && cc_dbg_sliceno <= hi)
        fprintf(itf, "%ld %04x\n", cc_dbg_sliceno, pc & 0xffff);
}
#ifdef CHQ_AUDIO_NATIVE
void chq_aud_trace(unsigned short pc){ itrace_log(pc); }
#endif

/* HOST-ONLY (bank,pc) coverage for the Z80->native static recompiler.  When
 * cores/z80.c is built with -DZ80_TRACE_PC it calls z80_trace_pc() before
 * every instruction -- timing-neutral, so the REAL race run (batch execution,
 * exact machine phase) produces complete driver coverage including banks. */
static uint8_t z80cov[4][0x10000/8];
static unsigned z80hist[0x10000];   /* per-PC instruction counts */
void z80_trace_pc(int ipc){
    unsigned pc = (unsigned)ipc & 0xffff;
    unsigned b = (pc >= 0x4000 && pc < 0x8000) ? (unsigned)cur_bank & 3 : 0;
    z80cov[b][pc >> 3] |= (uint8_t)(1u << (pc & 7));
    z80hist[pc]++;
    itrace_log(pc);
}
void cc_audio_z80hist_dump(const char *path){
    FILE *f = fopen(path, "w");
    if(!f) return;
    for(unsigned pc = 0; pc < 0x10000; pc++)
        if(z80hist[pc]) fprintf(f, "%04x %u\n", pc, z80hist[pc]);
    fclose(f);
}
void cc_audio_z80cov_dump(const char *path){
    FILE *f = fopen(path, "w");
    if(!f) return;
    for(unsigned b = 0; b < 4; b++)
        for(unsigned pc = 0; pc < 0x10000; pc++)
            if(z80cov[b][pc >> 3] & (1u << (pc & 7))){
                if(pc >= 0x4000 && pc < 0x8000) fprintf(f, "%u:%04x\n", b, pc);
                else if(b == 0) fprintf(f, "0:%04x\n", pc);
            }
    fclose(f);
}
#endif

/* Run the Z80 + YM timer for `total` Z80 cycles. On Amiga this is driven at the AUDIO
 * rate (Paula refill) so the music tempo is independent of the video framerate. */
void cc_audio_run_cycles(int total){
#ifndef __amigaos__
    cc_z80_cyc_total += (unsigned long)(total > 0 ? total : 0);
#endif
    total /= cc_z80_div;                 /* underclock: fewer real cycles, same wall time */
    if(!z_ok || z_reset_held) return;
    const int per=4166;
    while(total>0){
        int run = total<per?total:per;
#ifdef CHQ_AUDIO_NATIVE
        zn_run_slice(run);
#else
        /* Z80_CATCH_EI: Z80Emulate stops at each EI (PC at the next opcode).
         * Real Z80 semantics: one more instruction executes after EI, THEN a
         * pending INT is accepted.  Without this, the driver's ~2-instruction
         * enable window (ei; jp loop; di) made acceptance depend on where
         * slice boundaries happened to land -- most timer INTs were missed. */
        {
            int done = 0;
            while(done < run){
                int e = Z80Emulate(&z.state, run - done, &z);
                if(e <= 0) break;
                done += e;
                if(z.state.status == Z80_STATUS_EI){
                    z.state.status = 0;
                    if(z_int){
                        int e2 = Z80Emulate(&z.state, 1, &z);   /* the post-EI instruction */
                        if(e2 > 0) done += e2;
                        z.state.status = 0;
                        if(z.state.iff1){ cc_dbg_intok++; done += Z80Interrupt(&z.state, 0xff, &z); cc_dbg_irq++; }
                    }
                } else if(z.state.status != 0){
                    z.state.status = 0;   /* HALT etc.: burn the rest of the slice */
                    break;
                }
            }
        }
#endif
#ifndef __amigaos__
        /* CHQ_SLICELOG=<file>: end-of-slice (pc,iff1) trace -- diffing interpreter
         * vs native finds the FIRST slice whose boundary lands on a different
         * instruction (i.e. the first cycle-accounting mismatch). */
        { static FILE *slog; static int slog_tried;
          if(!slog_tried){ slog_tried=1; const char*fn=getenv("CHQ_SLICELOG"); if(fn) slog=fopen(fn,"w"); }
          cc_dbg_sliceno++;
          if(slog){
#ifdef CHQ_AUDIO_NATIVE
              fprintf(slog, "%ld %04x %d %d\n", cc_dbg_sliceno, zn.pc, zn.iff1, z_int);
#else
              fprintf(slog, "%ld %04x %d %d\n", cc_dbg_sliceno, z.state.pc & 0xffff, z.state.iff1 & 1, z_int);
#endif
          } }
#endif
        for(int c=0;c<2;c++) if(tmr_per_cyc[c]>0){ tmr_acc_cyc[c]+=run; while(tmr_acc_cyc[c]>=tmr_per_cyc[c]){ tmr_acc_cyc[c]-=tmr_per_cyc[c]; cc_dbg_tover++; if(ym_ok) YM2610TimerOver(0,c); } }
#ifdef CHQ_AUDIO_NATIVE
        if(zn.iff1) cc_dbg_iffever=1;
        if(nmi_pending){ zn_nmi(); nmi_pending=0; cc_dbg_nmi++; }
        if(z_int){ if(zn.iff1) cc_dbg_intok++; zn_int38(); cc_dbg_irq++; }
#else
        if(z.state.iff1) cc_dbg_iffever=1;
        if(nmi_pending){ Z80NonMaskableInterrupt(&z.state, &z); nmi_pending=0; cc_dbg_nmi++; }
        if(z_int){ if(z.state.iff1) cc_dbg_intok++; Z80Interrupt(&z.state, 0xff, &z); cc_dbg_irq++; }
#endif
        total -= run;
    }
}
void cc_audio_run_frame(void){ cc_audio_run_cycles(4000000/60); }   /* host: one 1/60s frame */

/* DIAGNOSTIC: force a loud FM note (algorithm 7 = all carriers, pan L+R) to prove
 * the YM core + my integration can produce output independent of the Z80 driver. */
void cc_audio_testnote(void){
    if(!ym_ok) return;
    /* YM2610 active FM channels are 1,2,4,5 -- use channel 1 (regs +1: 0x31/0xb1/0xa1) */
    static const unsigned char seq[][2]={
        {0x28,0x01},
        {0x31,0x01},{0x41,0x00},{0x51,0x1f},{0x61,0x00},{0x71,0x00},{0x81,0x0f},
        {0x35,0x01},{0x45,0x00},{0x55,0x1f},{0x65,0x00},{0x75,0x00},{0x85,0x0f},
        {0x39,0x01},{0x49,0x00},{0x59,0x1f},{0x69,0x00},{0x79,0x00},{0x89,0x0f},
        {0x3d,0x01},{0x4d,0x00},{0x5d,0x1f},{0x6d,0x00},{0x7d,0x00},{0x8d,0x0f},
        {0xb1,0x07},{0xb5,0xc0},{0xa5,0x22},{0xa1,0x69},{0x28,0xf1},
    };
    for(int i=0;i<(int)(sizeof seq/sizeof seq[0]);i++){ YM2610Write(0,0,seq[i][0]); YM2610Write(0,1,seq[i][1]); }
}

/* Render `n` samples of mixed mono signed-8-bit PCM (Paula). FM+ADPCM via YM2610
 * UpdateOne (stereo), SSG ticked + amplitude-mixed. */
#define CHUNK 256
void cc_audio_render(signed char *out, int n){
    static INT16 bl[CHUNK], br[CHUNK];
    static unsigned ssg_acc;
    static int lp;
    INT16 *bufs[2];
    if(!ym_ok){ memset(out,0,n); return; }
    int done=0;
    /* SSG idle gate: if no channel has a nonzero fixed volume or envelope mode
     * (regs 8-10 low 5 bits all clear), ay_amp is identically 0 -- skip the
     * per-sample tick/mix entirely.  Regs only change between render chunks.
     * (CC_FM_FAST=0 keeps the original always-tick path for PCM reference.) */
#ifndef CC_FM_FAST
#define CC_FM_FAST 1
#endif
    int ssg_on = CC_FM_FAST ? ((ssg0.reg[8]|ssg0.reg[9]|ssg0.reg[10]) & 0x1f) : 1;
    while(done<n){
        int m=n-done; if(m>CHUNK)m=CHUNK;
        bufs[0]=bl; bufs[1]=br;
        YM2610UpdateOne(0, bufs, m);
        for(int i=0;i<m;i++){
            int ss = 0;
            if(ssg_on){
                ssg_acc += 250000;             /* SSG ~2MHz/8 tick rate */
                while(ssg_acc >= SND_SR){ ssg_acc -= SND_SR; ay_tick(&ssg0); }
                ss = ay_amp(&ssg0) * 8;        /* keep square/noise SSG below the FM bed */
            }
            int fm = ((int)bl[i] + (int)br[i]) / 2;
            { extern int cc_dbg_ymraw, cc_dbg_ssgraw; int af=fm<0?-fm:fm; if(af>cc_dbg_ymraw)cc_dbg_ymraw=af; int as=ss<0?-ss:ss; if(as>cc_dbg_ssgraw)cc_dbg_ssgraw=as; }
            int s  = (fm + ss) / 38;           /* Paula-safe YM bed without hard pumping */
            if(s >  96) s =  96 + (s-96)/8;    /* softer transient knee for Paula */
            if(s < -96) s = -96 + (s+96)/8;
            if(s> 127)s=127; if(s<-127)s=-127;
            lp += (s - lp) >> 3;               /* smoother Paula low-pass; reduces level wobble */
            out[done+i]=(signed char)lp;
        }
        done+=m;
    }
}
