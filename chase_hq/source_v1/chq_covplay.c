/* chq_covplay.c -- replay a CHQ_SYTLOG command stream into an ISOLATED Z80
 * sound driver to collect complete (bank,pc) coverage for the static
 * recompiler.  The driver's behaviour depends only on the SYT commands and
 * the reset line, so replaying them with cycle-accurate spacing reproduces
 * the real race's code paths -- without the machine's phase-sensitive coin
 * windows, and with the single-step coverage collector active.
 *
 * Usage: CHQ_Z80COV=<out> ./chq_covplay <sytlog> [tail_cycles]
 * Build: see build_covplay.sh */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "cc_state.h"

uint8_t cc_rom_audio[0x10000];
uint8_t cc_adpcma[0x180000], cc_adpcmb[0x80000];

void cc_audio_z80cov_dump(const char *path);

static void ld(const char*p, void*d, int n){ FILE*f=fopen(p,"rb"); if(!f){perror(p);exit(1);} fread(d,1,n,f); fclose(f); }

int main(int argc, char **argv)
{
    if(argc < 2){ fprintf(stderr, "usage: CHQ_Z80COV=out %s <sytlog> [tail_cycles]\n", argv[0]); return 1; }
    unsigned long tail = (argc > 2) ? strtoul(argv[2], 0, 0) : 8000000UL;  /* 2s of 4MHz */
    ld("data/audiocpu.bin", cc_rom_audio, 0x10000);
    ld("data/adpcma.bin", cc_adpcma, 0x180000);
    ld("data/adpcmb.bin", cc_adpcmb, 0x80000);

    cc_audio_init();

    FILE *f = fopen(argv[1], "r");
    if(!f){ perror(argv[1]); return 1; }
    unsigned long t, now = 0, sample_frac = 0;
    int type; unsigned d;
    long events = 0;
    static signed char pcm[2048];

    /* Advance the driver AND pump the synth: the YM's status/busy/ADPCM-end
     * flags only progress inside YM2610UpdateOne, and the driver polls them --
     * without rendering, init stalls forever.  CC_SND_SR samples per 4MHz of
     * Z80 cycles, in <=2048-sample chunks. */
    #define ADVANCE(cyc) do { \
        unsigned long c_ = (cyc); \
        while(c_){ \
            unsigned long step = c_ > 200000UL ? 200000UL : c_; \
            cc_audio_run_cycles((int)step); \
            sample_frac += step * (unsigned long)CC_SND_SR; \
            unsigned long want = sample_frac / 4000000UL; \
            sample_frac -= want * 4000000UL; \
            while(want){ \
                unsigned long n = want > sizeof(pcm) ? sizeof(pcm) : want; \
                cc_audio_render(pcm, (int)n); \
                want -= n; \
            } \
            c_ -= step; \
        } \
    } while(0)

    while(fscanf(f, "%lu %d %x", &t, &type, &d) == 3){
        if(t > now){ ADVANCE(t - now); now = t; }
        if(type == 0)      cc_syt_master_port_w((uint8_t)d);
        else if(type == 1) cc_syt_master_comm_w((uint8_t)d);
        else if(type == 2) cc_audio_reset_line((int)d);
        events++;
    }
    fclose(f);
    ADVANCE(tail);   /* let the last tunes/SFX run out */

    {
        extern int cc_dbg_keyon, cc_dbg_adpcma, cc_dbg_deltat, cc_dbg_nmi, cc_dbg_irq,
                   cc_dbg_slaver, cc_dbg_iffever, cc_dbg_tmr, cc_dbg_ymw;
        int pc, iff, im;
        void cc_audio_dbg(int*,int*,int*);
        cc_audio_dbg(&pc, &iff, &im);
        printf("driver: pc=%04x iff1=%d im=%d ymw=%d keyon=%d adpcma=%d deltat=%d nmi=%d irq=%d slaveR=%d tmr=%d iffever=%d\n",
               pc, iff, im, cc_dbg_ymw, cc_dbg_keyon, cc_dbg_adpcma, cc_dbg_deltat,
               cc_dbg_nmi, cc_dbg_irq, cc_dbg_slaver, cc_dbg_tmr, cc_dbg_iffever);
    }
    {
        const char *out = getenv("CHQ_Z80COV");
        if(out){ cc_audio_z80cov_dump(out); printf("replayed %ld events, coverage -> %s\n", events, out); }
        else printf("replayed %ld events (no CHQ_Z80COV set)\n", events);
    }
    return 0;
}
