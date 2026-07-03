/* cc_audio_amiga.c -- Paula playback for S.C.I. sound, CONTINUOUS RING model.
 *
 * WHY A CONTINUOUS RING (the proven Pac-Land/Saint Dragon fix, see st_audio_amiga.c):
 * Paula free-runs over ONE large chip-RAM ring, looping it forever (AUDxLC/LEN set
 * once, never re-latched -> no loop-click).  Each game frame we advance an estimate
 * of how many samples Paula has consumed off the real EClock and render exactly that
 * many NEW samples a fixed LEAD ahead of Paula's read point.  So pitch + tempo stay
 * correct no matter how slow a video frame runs, and Paula is always fed fresh
 * continuous PCM instead of re-looping a stale chunk (the old double-buffer starved
 * and buzzed whenever the interpreted dual-68000 frame ran long).
 *
 * The Z80 + YM2610 are advanced PROPORTIONALLY to the samples rendered
 * (audio-clocked), so the music/SFX clock follows real playback time.
 * Two channels (0+1) play the same mono stream, centred. */
#include <exec/exec.h>
#include <exec/memory.h>
#include <exec/execbase.h>
#include <proto/exec.h>
#include <devices/timer.h>
#include <proto/timer.h>
#include <stdint.h>
#include "sci_state.h"

/* Data-cache-only flush of just the freshly written ring bytes.  This is all a
 * real 68040/060 copyback cache needs for Paula DMA to see the samples.  The old
 * CacheClearU() flushed EVERYTHING every frame -- under an emulator JIT that can
 * blow away the entire translation cache each frame, which measured as the whole
 * game running at a fraction of its speed. */
static inline void flush_ring_range(void *p, unsigned long n){
    if(SysBase->LibNode.lib_Version >= 37) CacheClearE(p, n, CACRF_ClearD);
    else CacheClearU();
}

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

#define SND_SR   CC_SND_SR                   /* shared synthesis rate (cc_state.h)   */
#define A_PER    ((3546895 + SND_SR/2) / SND_SR)   /* rounded PAL Paula period       */
#define SR_PLAY  (3546895 / A_PER)           /* what Paula REALLY consumes at A_PER  */
#define CC_SPF   (SND_SR/60 + 64)            /* ring frame-unit: 60Hz refill + margin */

/* Ring geometry, in display frames.  LEAD = how far ahead of Paula's read point we
 * keep fresh audio; must exceed the slowest DISPLAY-loop iteration (which can span
 * several guest frames under the wall-clock catch-up scheduler), or Paula reaches
 * stale data = jerky sound.  6 frames = 100ms, the Power Drift/Saint Dragon value. */
#define LEAD_FR  6
#define RING_FR  16
#define CC_LEAD  (LEAD_FR * CC_SPF)
#define CC_RING  (RING_FR * CC_SPF)
#define FRAME_HZ 6000u                       /* fallback pacing, 60Hz x100 */

static signed char  *ring = 0;
static unsigned long  p_play  = 0;     /* estimated samples Paula has consumed   */
static unsigned long  p_wrote = 0;     /* total samples rendered into the ring   */
static unsigned       acc     = 0;     /* fractional remainder of the rate accum */

/* Wall-clock audio pacing: drive p_play off the real EClock instead of the game
 * frame count, so music/SFX tempo stays correct however slow the video loop runs. */
extern struct Device *TimerBase;       /* opened by cc_rtg_main.c (UNIT_ECLOCK) */
static unsigned long    aud_rate;      /* EClock ticks per second               */
static unsigned long    aud_last;      /* previous EClock ev_lo                 */
static unsigned long    aud_frac;      /* fractional sample-numerator carry     */
static int              aud_wallclock; /* 1 = pace audio off real EClock        */

static inline void aud_setup(volatile uint16_t *c){
    uint32_t a = (uint32_t)ring;
    c[R_AUD0LCH] = (uint16_t)(a >> 16); c[R_AUD0LCH + 1] = (uint16_t)a;
    c[R_AUD1LCH] = (uint16_t)(a >> 16); c[R_AUD1LCH + 1] = (uint16_t)a;
    c[R_AUD0LEN] = CC_RING / 2;  c[R_AUD1LEN] = CC_RING / 2;
    c[R_AUD0PER] = A_PER;        c[R_AUD1PER] = A_PER;
    c[R_AUD0VOL] = 64;           c[R_AUD1VOL] = 64;
}

/* Render `n` fresh samples into the ring from the current write cursor, splitting at
 * the ring wrap.  The Z80 + YM2610 advance proportionally to each chunk (4MHz Z80,
 * exact via a fractional cycle accumulator; chunk capped so chunk*4e6 fits 32-bit). */
static void ring_render(unsigned long n){
    static unsigned long cyc_frac;
    while(n){
        unsigned long idx   = p_wrote % CC_RING;
        unsigned long chunk = CC_RING - idx;
        if(chunk > 736) chunk = 736;
        if(chunk > n)   chunk = n;
        cyc_frac += chunk * 4000000UL;
        cc_audio_run_cycles((int)(cyc_frac / SND_SR));   /* advance Z80+YM timers */
        cyc_frac %= SND_SR;
        cc_audio_render(ring + idx, (int)chunk);         /* synth the PCM         */
        flush_ring_range(ring + idx, chunk);
        p_wrote += chunk;
        n       -= chunk;
    }
}

void cc_audio_amiga_open(void){
    volatile uint16_t *c = CUSTOM;
    void *chunk = AllocMem(CC_RING, MEMF_CHIP|MEMF_CLEAR);
    if(!chunk){ ring = 0; return; }
    ring = (signed char*)chunk;

    p_play = 0; p_wrote = 0; acc = 0;
    aud_wallclock = 0; aud_frac = 0;
    if(TimerBase){ struct EClockVal ev; aud_rate = ReadEClock(&ev);
        if(aud_rate){ aud_last = ev.ev_lo; aud_wallclock = 1; } }
    ring_render(CC_LEAD);              /* prime the lead (flushed per chunk) */

    /* Start from a FULLY clean Paula: stop ALL FOUR channels and zero every volume/
     * length, so nothing a loader may have left on AUD2/3 can bleed under the game. */
    c[R_DMACON] = 0x000f;             /* stop AUD0-3 */
    c[R_AUD0VOL]=0; c[R_AUD1VOL]=0; c[R_AUD2VOL]=0; c[R_AUD3VOL]=0;
    c[R_AUD2LEN]=0; c[R_AUD3LEN]=0;
    aud_setup(c);                     /* configure AUD0/1 only */
    c[R_DMACON] = 0x8203;             /* SET | DMAEN | AUD0 | AUD1 (AUD2/3 stay off) */
}

void cc_audio_amiga_frame(void){
    if(!ring) return;

    if(aud_wallclock){
        /* real elapsed EClock -> samples consumed at Paula's true rate.  EXACT for any
         * dt (Paula's sample clock = EClock*5 on the same crystal, PAL and NTSC):
         * samples = ticks*5/A_PER, decomposed to stay in 32 bits.  p_play must ALWAYS
         * track true elapsed time -- Paula never stops.  (The old dt clamp here
         * under-counted consumption on long frames, so once Paula lapped the write
         * cursor the stale-playback offset never self-healed = the jumping record.) */
        struct EClockVal ev; ReadEClock(&ev);
        unsigned long dt = ev.ev_lo - aud_last;      /* ticks since last frame (wraps ok) */
        aud_last = ev.ev_lo;
        {
            unsigned long q = dt / A_PER, r = dt - q*A_PER;
            unsigned long t = aud_frac + r*5UL;      /* aud_frac < A_PER, r*5 < 1610   */
            p_play  += q*5UL + t/A_PER;
            aud_frac = t - (t/A_PER)*A_PER;
        }
    } else {
        acc += SR_PLAY * 100u;        /* fallback (no timer): frame-paced 60Hz */
        p_play += acc / FRAME_HZ;
        acc %= FRAME_HZ;
    }

    /* UNDERRUN GUARD: if Paula has caught (or lapped) the write cursor, everything
     * between them is stale ring content from a lap ago -- silence the whole ring,
     * resnap a full LEAD ahead of the play head, and resume rendering next frame
     * (rendering immediately at the head would tear under Paula).  Worst case is a
     * brief self-healing silence gap; NEVER a re-looping stale chunk ("jumping
     * record").  Works because p_play above now always tracks TRUE elapsed time. */
    if((long)(p_wrote - p_play) < (long)CC_SPF){
        unsigned long i;
        for(i = 0; i < CC_RING; i++) ring[i] = 0;
        flush_ring_range(ring, CC_RING);
        p_wrote = p_play + CC_LEAD;
        return;
    }

    /* keep the write cursor a fixed LEAD ahead of Paula, capped at ~one ring */
    {
        unsigned long target = p_play + CC_LEAD;
        unsigned long cap    = p_play + (CC_RING - CC_SPF);
        if(target > cap) target = cap;

        if((long)(target - p_wrote) > 0)
            ring_render(target - p_wrote);
    }
}

void cc_audio_amiga_pause(int on){
    volatile uint16_t *c = CUSTOM;
    if(!ring) return;
    c[R_AUD0VOL] = on ? 0 : 64;
    c[R_AUD1VOL] = on ? 0 : 64;
    if(!on && aud_wallclock){ struct EClockVal ev; ReadEClock(&ev); aud_last = ev.ev_lo; }
}

void cc_audio_amiga_close(void){
    volatile uint16_t *c = CUSTOM;
    c[R_DMACON] = 0x0003;             /* stop AUD0/1 */
    c[R_AUD0VOL] = 0; c[R_AUD1VOL] = 0;
    for(volatile unsigned i = 0; i < 50000; i++) ;
    if(ring){ FreeMem(ring, CC_RING); ring = 0; }
    p_play = p_wrote = 0; acc = 0;
}
