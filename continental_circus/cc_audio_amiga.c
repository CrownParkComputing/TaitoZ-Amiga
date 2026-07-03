/* cc_audio_amiga.c -- Paula playback for Continental Circus sound.
 * Continuous ring playback. Paula free-runs over one CHIP RAM ring while the
 * game loop writes freshly mixed YM2610/Z80 audio a fixed lead ahead of the
 * estimated play cursor. The play cursor is measured from EClock wall time, so
 * speech pitch and music tempo do not drop when RTG rendering runs below 60Hz.
 * Two channels (0+1) play the same mono stream, centred. */
#include <exec/exec.h>
#include <exec/memory.h>
#include <proto/exec.h>
#include <devices/timer.h>
#include <proto/timer.h>
#include <stdint.h>
#include "cc_state.h"

extern struct Device *TimerBase;

#define CUSTOM    ((volatile uint16_t *)0xdff000)
#define R_DMACON  (0x096/2)
#define R_AUD0LCH (0x0a0/2)
#define R_AUD0LEN (0x0a4/2)
#define R_AUD0PER (0x0a6/2)
#define R_AUD0VOL (0x0a8/2)
#define R_AUD1LCH (0x0b0/2)
#define R_AUD1LEN (0x0b4/2)
#define R_AUD1PER (0x0b6/2)
#define R_AUD1VOL (0x0b8/2)
#define R_AUD2LEN (0x0c4/2)
#define R_AUD2VOL (0x0c8/2)
#define R_AUD3LEN (0x0d4/2)
#define R_AUD3VOL (0x0d8/2)

#ifndef CC_SND_SR
#define CC_SND_SR 18518       /* YM2610 native ADPCM-A rate; must match cc_audio.c */
#endif
#define SND_SR CC_SND_SR
#define PAULA_CLK 3546895UL   /* PAL Paula sample clock = EClock*5 */
#define A_PER  ((PAULA_CLK + SND_SR/2) / SND_SR)   /* 192 @ 18518 -> true 18473Hz */
#define CC_SPF  (SND_SR / 60 + 64)
#define CC_LEAD (6 * CC_SPF)
#define CC_RING (24 * CC_SPF)
#define CC_Z80_CLOCK 4000000UL
#define CC_RENDER_MAX 512UL

static signed char *ring;
static unsigned long p_play;
static unsigned long p_wrote;
static unsigned long aud_rate;
static unsigned long aud_last;
static unsigned long aud_frac;
static unsigned long z80_frac;
static long z80_pend;              /* Z80 cycles owed to the interleaved slices */
static unsigned int frame_acc;
static int aud_wallclock;

static void aud_setup(volatile uint16_t *c)
{
    uint32_t a = (uint32_t)ring;
    c[R_AUD0LCH] = (uint16_t)(a >> 16); c[R_AUD0LCH + 1] = (uint16_t)a;
    c[R_AUD1LCH] = (uint16_t)(a >> 16); c[R_AUD1LCH + 1] = (uint16_t)a;
    c[R_AUD0LEN] = CC_RING / 2; c[R_AUD1LEN] = CC_RING / 2;
    c[R_AUD0PER] = A_PER;       c[R_AUD1PER] = A_PER;
    c[R_AUD0VOL] = 64;          c[R_AUD1VOL] = 64;
}

static void ring_render(unsigned long n)
{
    while (n) {
        unsigned long idx = p_wrote % CC_RING;
        unsigned long chunk = CC_RING - idx;
        unsigned long cycles;
        if (chunk > n) chunk = n;
        if (chunk > CC_RENDER_MAX) chunk = CC_RENDER_MAX;

        /* Z80 cycles owed for these samples are NOT run here any more: they are
         * handed to cc_machine_run_frame as a budget and drained in slices
         * interleaved with the 68ks (SYT handshake latency fix -- running the
         * Z80 once per frame made the course-map screen take 12s instead of
         * 1.5s). chunk<=512 so chunk*4M fits 32 bits. */
        z80_frac += chunk * CC_Z80_CLOCK;
        cycles = z80_frac / SND_SR;
        z80_frac %= SND_SR;
        z80_pend += (long)cycles;

        cc_audio_render(ring + idx, (int)chunk);
        CacheClearE(ring + idx, chunk, CACRF_ClearD);
        p_wrote += chunk;
        n -= chunk;
    }
}

/* Called once per main-loop frame BEFORE cc_machine_run_frame: hand over the
 * wall-clock-true Z80 cycle budget owed for the audio rendered so far. */
long cc_audio_amiga_take_z80(void){ long p = z80_pend; z80_pend = 0; return p; }

void cc_audio_amiga_open(void){
    volatile uint16_t *c = CUSTOM;
    void *chunk = AllocMem(CC_RING, MEMF_CHIP|MEMF_CLEAR);
    if(!chunk){ ring=0; return; }
    ring = (signed char*)chunk;

    p_play = 0;
    p_wrote = 0;
    aud_rate = 0;
    aud_last = 0;
    aud_frac = 0;
    z80_frac = 0;
    z80_pend = 0;
    frame_acc = 0;
    aud_wallclock = 0;
    if (TimerBase) {
        struct EClockVal ev;
        aud_rate = ReadEClock(&ev);
        if (aud_rate) {
            aud_last = ev.ev_lo;
            aud_wallclock = 1;
        }
    }

    ring_render(CC_LEAD);
    CacheClearU();
    c[R_DMACON] = 0x000f;                          /* stop AUD0-3 */
    c[R_AUD0VOL]=0; c[R_AUD1VOL]=0; c[R_AUD2VOL]=0; c[R_AUD3VOL]=0;
    c[R_AUD2LEN]=0; c[R_AUD3LEN]=0;
    aud_setup(c);
    c[R_DMACON] = 0x8203;                          /* SET|DMAEN|AUD0|AUD1 */
}

void cc_audio_amiga_close(void){
    volatile uint16_t *c = CUSTOM;
    signed char *old_ring = ring;
    ring = 0;

    c[R_DMACON] = 0x000f;                 /* stop AUD0-3 DMA */
    c[R_AUD0VOL]=0; c[R_AUD1VOL]=0; c[R_AUD2VOL]=0; c[R_AUD3VOL]=0;
    c[R_AUD0LEN]=1; c[R_AUD1LEN]=1; c[R_AUD2LEN]=1; c[R_AUD3LEN]=1;
    for(volatile unsigned i = 0; i < 50000; i++) ;
    if(old_ring) FreeMem(old_ring, CC_RING);
    p_play = p_wrote = 0;
    aud_rate = aud_last = aud_frac = z80_frac = 0;
    frame_acc = 0;
    aud_wallclock = 0;
}

void cc_audio_amiga_frame(void){
    if(!ring) return;

    if (aud_wallclock) {
        /* Pace p_play at Paula's TRUE consumption rate: Paula clock = EClock*5,
         * samples = eclock_ticks*5/A_PER. Exact 32-bit decomposition, and NO dt
         * clamp -- under-counting long frames left p_play behind Paula's real
         * position forever = the "jumping record" (Chase HQ lesson). */
        struct EClockVal ev;
        unsigned long dt, q, r, t;
        ReadEClock(&ev);
        dt = ev.ev_lo - aud_last;
        aud_last = ev.ev_lo;
        q = dt / A_PER;
        r = dt - q * A_PER;
        t = aud_frac + r * 5UL;
        p_play  += q * 5UL + t / A_PER;
        aud_frac = t - (t / A_PER) * A_PER;
    } else {
        frame_acc += SND_SR;
        p_play += frame_acc / 60u;
        frame_acc %= 60u;
    }

    /* Underrun: Paula lapped the writer (heavy frame / task switch). Zero the
     * ring and re-prime the lead -- brief silence, never looping stale audio. */
    if ((long)(p_wrote - p_play) < (long)CC_SPF) {
        unsigned long i;
        for (i = 0; i < CC_RING; i++) ring[i] = 0;
        CacheClearE(ring, CC_RING, CACRF_ClearD);
        p_wrote = p_play + CC_LEAD;
        return;
    }

    {
        unsigned long target = p_play + CC_LEAD;
        unsigned long cap = p_play + (CC_RING - CC_SPF);
        if (target > cap) target = cap;
        if ((long)(target - p_wrote) > 0)
            ring_render(target - p_wrote);
    }
}
