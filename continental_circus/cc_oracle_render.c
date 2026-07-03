/* cc_oracle_render.c -- validation harness. Feeds MAME's EXACT dumped video state
 * (scn/ctrl/road/spr/pal/palbank from dump.lua) through the REAL cc_render.c, so we
 * compare MAME's render of a frame against OUR render of the identical state -- with
 * zero CPU-timing drift. Proves each gfx layer's colour AND shape vs MAME.
 *
 * Build (native): see build_oracle.sh.  Usage: cc_oracle_render <tag> [layeropts]
 *   tag = f0300 etc; reads dumps/<tag>_{scn,ctrl,road,spr,pal,palbank}.bin
 *   writes dumps/<tag>_host.ppm. Honors cc_render env: CC_ONLY, CC_NOSPR, CC_ROADC. */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "cc_state.h"

/* ---- the state cc_render.c reads (normally owned by cc_machine.c) ---- */
uint8_t  cc_scn[0x14000];
uint8_t  cc_scnctl[0x10];
uint8_t  cc_road[0x2000];
uint8_t  cc_spr[0x700];
uint16_t cc_pal[0x1000];
int      cc_road_palbank = 3;

/* ---- gfx ROMs (loaded from data/) ---- */
uint8_t cc_gfx_scn[0x80000], cc_gfx_road[0x80000], cc_gfx_spr[0x200000], cc_gfx_smap[0x80000];
/* unused-by-render symbols some headers reference (kept for link parity) */
uint8_t cc_pal256[768], cc_lut32k[32768];

static void ld(const char*p, void*d, int n, int must){
    FILE*f=fopen(p,"rb"); if(!f){ if(must){perror(p);exit(1);} else {memset(d,0,n);return;} }
    int got=fread(d,1,n,f); fclose(f);
    if(must && got!=n){ fprintf(stderr,"%s: short read %d/%d\n",p,got,n); }
}

int main(int argc,char**argv){
    if(argc<2){ fprintf(stderr,"usage: %s <tag>\n",argv[0]); return 2; }
    const char*tag=argv[1];
    char p[256];
    /* gfx ROMs */
    ld("data/gfx_scn.bin",      cc_gfx_scn,  0x80000, 1);
    ld("data/gfx_road.bin",     cc_gfx_road, 0x80000, 1);
    ld("data/sprites_asm.bin",  cc_gfx_spr,  0x200000,1);
    ld("data/spritemap_asm.bin",cc_gfx_smap, 0x80000, 1);
    /* MAME-dumped video state */
    #define DD "/tmp/claude-1000/-home-jon-AmigaArcadePorts/6207b065-fbcb-4925-b26c-a38a6c4c8ce7/scratchpad/oracle/dumps/"
    snprintf(p,sizeof p,DD"%s_scn.bin",tag);  ld(p, cc_scn,   0x10000, 1);
    snprintf(p,sizeof p,DD"%s_ctrl.bin",tag); ld(p, cc_scnctl,0x10,    1);
    snprintf(p,sizeof p,DD"%s_road.bin",tag); ld(p, cc_road,  0x2000,  1);
    snprintf(p,sizeof p,DD"%s_spr.bin",tag);  ld(p, cc_spr,   0x700,   1);
    snprintf(p,sizeof p,DD"%s_pal.bin",tag);  ld(p, cc_pal,   0x2000,  1); /* LE u16 from dump.lua */
    { unsigned char b=3; snprintf(p,sizeof p,DD"%s_palbank.bin",tag); ld(p,&b,1,0); cc_road_palbank=b; }

    static uint16_t buf[CC_H*CC_W];
    cc_render_frame(buf, CC_W);

    snprintf(p,sizeof p,DD"%s_host.ppm",tag);
    FILE*f=fopen(p,"wb"); fprintf(f,"P6\n%d %d\n255\n",CC_W,CC_H);
    for(int i=0;i<CC_W*CC_H;i++){ unsigned d=cc_pal[buf[i]&0xfff]&0x7fff;
        int R=(d>>10)&0x1f,G=(d>>5)&0x1f,B=d&0x1f;
        fputc((R<<3)|(R>>2),f); fputc((G<<3)|(G>>2),f); fputc((B<<3)|(B>>2),f); }
    fclose(f);
    fprintf(stderr,"%s: rendered (palbank=%d)\n",tag,cc_road_palbank);
    return 0;
}
