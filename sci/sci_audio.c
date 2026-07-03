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
#include "sci_state.h"
#include <string.h>
#ifndef HAS_YM2610
#define HAS_YM2610 1
#endif
#include "driver.h"   /* OPN core types: UINT8/INT16/INT32/... + logerror */
#include "fm.h"

extern uint8_t cc_rom_audio[];   /* 0x20000 audiocpu (c09-34) */
extern uint8_t cc_adpcma[];      /* 0x180000 ADPCM-A samples */
extern uint8_t cc_adpcmb[];      /* 0x080000 ADPCM-B (delta-T) samples */

#define SND_SR   CC_SND_SR       /* shared output rate (cc_state.h) */

static MY_LITTLE_Z80 z;
static int z_ok, ym_ok;

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
void cc_audio_dbg(int*pc,int*iff,int*im){ *pc=z.state.pc; *iff=z.state.iff1; *im=z.state.im; cc_dbg_ymok_v=ym_ok; }

#ifdef __amigaos__
#define CCSND_TRACE 0
#else
#define CCSND_TRACE getenv("CCSND")
#endif

static void syt_update_nmi(void){ nmi_pending = ((syt_status & (P01|P23)) && nmi_enabled) ? 1 : 0; if(nmi_pending)cc_dbg_nmiset++; cc_dbg_nmien=nmi_enabled; }

void cc_syt_master_port_w(uint8_t d){ mainmode = d & 0x0f; }
void cc_syt_master_comm_w(uint8_t d){
    cc_dbg_sytc++;
    d &= 0x0f;
    switch(mainmode){
        case 0: slavedata[mainmode++]=d; break;
        case 1: slavedata[mainmode++]=d; syt_status|=P01; cc_dbg_sytc++; if(CCSND_TRACE)fprintf(stderr,"CMD %02x%02x\n",slavedata[0],slavedata[1]); syt_update_nmi(); break;
        case 2: slavedata[mainmode++]=d; break;
        case 3: slavedata[mainmode++]=d; syt_status|=P23; syt_update_nmi(); break;
        case 4: { int hold=(d!=0);   /* MAME: reset_cb(data?ASSERT:CLEAR) -- hold when d!=0 */
                  if(z_reset_held && !hold && z_ok) Z80Reset(&z.state);   /* release -> reset+run */
                  z_reset_held=hold; break; }
    }
}

void cc_audio_reset_line(int clear_line){
    int hold = clear_line ? 0 : 1;
    if(z_reset_held && !hold && z_ok) Z80Reset(&z.state);
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
static void fm_timer(int n,int c,int count,double step){ (void)n;(void)step; if(c<0||c>1)return;
    if(count<=0){ tmr_per_cyc[c]=0; } else { tmr_per_cyc[c]=count*(c==0?72:1152); tmr_acc_cyc[c]=0; cc_dbg_tmr++; } }
static void fm_irq(int n,int irq){ (void)n; z_int = irq; }

/* ================= Z80 bank + memory glue ================================== */
static void set_bank(int b){ b&=7; memcpy(z.memory+0x4000, cc_rom_audio + b*0x4000, 0x4000); }

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
    if(a>=0xe000 && a<=0xe003){ cc_dbg_ymw++;
        { extern int cc_dbg_keyon, cc_dbg_adpcma; static int la=0, lb=0;
          if((a&3)==0) la=v;                                   /* port-A addr latch */
          else if((a&3)==1){ if(la==0x28 && (v&0xf0)) cc_dbg_keyon++;     /* FM key-on */
          } else if((a&3)==2) lb=v;                                      /* port-B addr latch */
          else if((a&3)==3 && lb==0x00 && v<0x80 && (v&0x3f)) cc_dbg_adpcma++; /* ADPCM-A key-on */
          if((a&3)==1 && la==0x10 && (v&0x80)) cc_dbg_deltat++;            /* ADPCM-B/Delta-T start */
        }
        if(ym_ok) YM2610Write(0, a&3, v); return; }
    if(a==0xe200){ syt_slave_port_w(v); return; }
    if(a==0xe201){ syt_slave_comm_w(v); return; }
    if(a>=0xe400 && a<=0xe403) return;       /* pan */
    if(a==0xf200){ set_bank(v); return; }
}
unsigned char in_impl(MY_LITTLE_Z80 *zz, int p){ (void)zz;(void)p; return 0xff; }
void          out_impl(MY_LITTLE_Z80 *zz, int p, unsigned char x){ (void)zz;(void)p;(void)x; }

/* ================= public API ============================================== */
void cc_audio_init(void){
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
    Z80Reset(&z.state);
    mainmode=submode=syt_status=0; nmi_enabled=1; nmi_pending=0; z_reset_held=1;
    tmr_per_cyc[0]=tmr_per_cyc[1]=tmr_acc_cyc[0]=tmr_acc_cyc[1]=0; z_int=0;
    z_ok=1;
}

/* Run the Z80 + YM timer for `total` Z80 cycles. On Amiga this is driven at the AUDIO
 * rate (Paula refill) so the music tempo is independent of the video framerate. */
void cc_audio_run_cycles(int total){
    if(!z_ok || z_reset_held) return;
    const int per=512;   /* pdrift-size slices: finer timer/NMI interleave */
    while(total>0){
        int run = total<per?total:per;
        Z80Emulate(&z.state, run, &z);
        for(int c=0;c<2;c++) if(tmr_per_cyc[c]>0){ tmr_acc_cyc[c]+=run; while(tmr_acc_cyc[c]>=tmr_per_cyc[c]){ tmr_acc_cyc[c]-=tmr_per_cyc[c]; cc_dbg_tover++; if(ym_ok) YM2610TimerOver(0,c); } }
        if(z.state.iff1) cc_dbg_iffever=1;
        if(nmi_pending){ Z80NonMaskableInterrupt(&z.state, &z); nmi_pending=0; cc_dbg_nmi++; }
        if(z_int){ if(z.state.iff1) cc_dbg_intok++; Z80Interrupt(&z.state, 0xff, &z); cc_dbg_irq++; }
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
