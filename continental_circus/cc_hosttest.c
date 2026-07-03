/* cc_hosttest.c -- native sanity test of the ACTUAL Amiga source (cc_machine.c +
 * cc_render.c). Provides the embedded-ROM symbols by loading the data/ files,
 * runs N frames, writes a PPM (3-3-2 -> RGB). Proves the Amiga code path before
 * testing in WinUAE.  Build: see build_hosttest.sh */
#include <stdio.h>
#include <stdlib.h>
#include "cc_state.h"

/* the symbols cc_romdata.S provides on Amiga -- here loaded from files */
uint8_t cc_rom_main[0x40000], cc_rom_sub[0x40000];
uint8_t cc_gfx_scn[0x80000], cc_gfx_road[0x80000], cc_gfx_spr[0x200000], cc_gfx_smap[0x80000];
uint8_t cc_pal256[768], cc_lut32k[32768];
uint8_t cc_rom_audio[0x10000];
uint8_t cc_adpcma[0x100000], cc_adpcmb[0x80000];

static void ld(const char*p, const void*d, int n){ FILE*f=fopen(p,"rb"); if(!f){perror(p);exit(1);} fread((void*)d,1,n,f); fclose(f); }

int main(int argc,char**argv){
    int frames=(argc>1)?atoi(argv[1]):1800;
    ld("data/maincpu.bin",cc_rom_main,0x40000);
    ld("data/sub.bin",cc_rom_sub,0x40000);
    ld("data/gfx_scn.bin",cc_gfx_scn,0x80000);
    ld("data/gfx_road.bin",cc_gfx_road,0x80000);
    ld("data/sprites_asm.bin",cc_gfx_spr,0x200000);
    ld("data/spritemap_asm.bin",cc_gfx_smap,0x80000);
    ld("data/cc_pal256.bin",cc_pal256,768);
    ld("data/cc_lut32k.bin",cc_lut32k,32768);
    ld("data/audiocpu.bin",cc_rom_audio,0x10000);
    ld("data/adpcma.bin",cc_adpcma,0x100000);
    ld("data/adpcmb.bin",cc_adpcmb,0x80000);

    cc_machine_init();
    cc_audio_init();
    { extern int cc_dbg_mute; if(getenv("CC_MUTE")) cc_dbg_mute=atoi(getenv("CC_MUTE")); }
    { extern int cc_dbg_keytrace; if(getenv("CC_KEYTRACE")) cc_dbg_keytrace=1; }
    if(getenv("CC_TESTNOTE")){ void cc_audio_testnote(void); cc_audio_testnote();
        static signed char tp[4000]; int tmax=0;
        for(int r=0;r<8;r++){ cc_audio_render(tp,4000); for(int j=0;j<4000;j++){int v=tp[j];if(v<0)v=-v;if(v>tmax)tmax=v;} }
        extern int cc_dbg_ymraw,cc_dbg_yminit; int pc,iff,im; void cc_audio_dbg(int*,int*,int*); cc_audio_dbg(&pc,&iff,&im); extern int cc_dbg_ymok_v; printf("TESTNOTE: outPeak=%d ymRawPeak=%d YM2610Init_ret=%d ym_ok=%d\n",tmax,cc_dbg_ymraw,cc_dbg_yminit,cc_dbg_ymok_v); return 0; }
    static signed char pcm[48000];
    int amax=0; long anz=0;
    int snd_sr; { extern int cc_snd_sr(void); snd_sr=cc_snd_sr(); }
    int spf=snd_sr/60;
    FILE *wavf=0; long wav_n=0;
    if(getenv("CC_WAV")){                       /* dump rendered audio as mono u8 WAV at the engine rate */
        wavf=fopen(getenv("CC_WAV"),"wb");
        if(wavf){ unsigned char h[44]={'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,1,0,0,0,0,0,0,0,0,0,1,0,8,0,'d','a','t','a',0,0,0,0};
            h[24]=snd_sr&0xff;h[25]=(snd_sr>>8)&0xff;h[26]=(snd_sr>>16)&0xff;
            h[28]=h[24];h[29]=h[25];h[30]=h[26];
            fwrite(h,1,44,wavf); }
    }
    for(int i=0;i<frames;i++){
        { extern int cc_host_frame; cc_host_frame=i; }
        if(getenv("CC_COIN")){                 /* coin@120, start@200, GAS from 230 (race) */
            if(i>=120&&i<130) cc_set_inputs(0x13|0x08,0x1f,0xff,0xcf,0);
            else if(i>=200&&i<210) cc_set_inputs(0x13,0x1f&~0x08,0xff,0xcf,0);
            else if(i>=230 && !getenv("CC_NOGAS")){
                /* drive: full gas, shift to HIGH gear at ~s6, gentle scripted weave */
                uint8_t in1 = 0x1f;
                if(getenv("CC_DRIVE") && i>=360) in1 &= (uint8_t)~0x10;     /* shifter toggle = high gear */
                int st = 0;
                if(getenv("CC_DRIVE")){
                    int ph = (i/45)%8;                                       /* slow weave, avoid walls */
                    static const int weave[8] = {0,6,10,6,0,-6,-10,-6};
                    st = weave[ph];
                }
                cc_set_inputs(0x13|0x80,in1,0xff,0xcf,st);   /* FULL gas: Gray-encoded 4 = IN0 0x80 */
            }
            else cc_set_inputs(0x13,0x1f,0xff,0xcf,0);
        }
        { extern void cc_audio_frame_budget(int); cc_audio_frame_budget(4000000/60); }
        cc_machine_run_frame();   /* drains the Z80 budget in interleaved slices */
        if(getenv("CC_PCTRACE") && (i%30)==0){
            int pc,iff,im; void cc_audio_dbg(int*,int*,int*); cc_audio_dbg(&pc,&iff,&im);
            extern int cc_dbg_keyon,cc_dbg_sytc,cc_dbg_nmiset,cc_dbg_irq;
            printf("f=%4d pc=%04x iff1=%d im=%d  keyon=%d sytc=%d nmiset=%d irq=%d\n",i,pc,iff,im,cc_dbg_keyon,cc_dbg_sytc,cc_dbg_nmiset,cc_dbg_irq);
        }
        { static FILE *vf=0;
          if(getenv("CC_VIDEO") && i>=60){                /* raw RGB24 frames from f=60 */
            static uint16_t vb[CC_H*CC_W]; static uint8_t vrow[CC_W*3];
            if(!vf) vf=fopen(getenv("CC_VIDEO"),"wb");
            cc_render_frame(vb, CC_W);
            for(int q=0;q<CC_W*CC_H;q++){ unsigned d=cc_pal[vb[q]&0xfff]&0x7fff;
                int R=(d>>10)&0x1f,G=(d>>5)&0x1f,Bb=d&0x1f;
                vrow[(q%CC_W)*3+0]=(R<<3)|(R>>2); vrow[(q%CC_W)*3+1]=(G<<3)|(G>>2); vrow[(q%CC_W)*3+2]=(Bb<<3)|(Bb>>2);
                if((q%CC_W)==CC_W-1) fwrite(vrow,1,CC_W*3,vf);
            }
          } }
        if(getenv("CC_SNAPS") && (i==300||i==420||i==540||i==720)){
            static uint16_t sb[CC_H*CC_W]; char sp[64];
            cc_render_frame(sb, CC_W);
            snprintf(sp,sizeof sp,"/tmp/ccsnap_%04d.ppm",i);
            FILE*sf=fopen(sp,"wb"); fprintf(sf,"P6\n%d %d\n255\n",CC_W,CC_H);
            for(int q=0;q<CC_W*CC_H;q++){ unsigned d=cc_pal[sb[q]&0xfff]&0x7fff;
                int R=(d>>10)&0x1f,G=(d>>5)&0x1f,Bb=d&0x1f;
                fputc((R<<3)|(R>>2),sf);fputc((G<<3)|(G>>2),sf);fputc((Bb<<3)|(Bb>>2),sf); }
            fclose(sf);
        }
        if(getenv("CC_AUDIODUMP")||wavf){
            cc_audio_render(pcm, spf);
            for(int j=0;j<spf;j++){ int v=pcm[j]; if(v<0)v=-v; if(v>amax)amax=v; if(v)anz++; }
            if(wavf){ for(int j=0;j<spf;j++) fputc((unsigned char)(pcm[j]+128),wavf); wav_n+=spf; }
        }
    }
    if(wavf){ long dl=wav_n, rl=36+wav_n;        /* patch RIFF/data sizes */
        fseek(wavf,4,SEEK_SET); fputc(rl&0xff,wavf);fputc((rl>>8)&0xff,wavf);fputc((rl>>16)&0xff,wavf);fputc((rl>>24)&0xff,wavf);
        fseek(wavf,40,SEEK_SET); fputc(dl&0xff,wavf);fputc((dl>>8)&0xff,wavf);fputc((dl>>16)&0xff,wavf);fputc((dl>>24)&0xff,wavf);
        fclose(wavf); fprintf(stderr,"wrote %s (%ld samples)\n",getenv("CC_WAV"),wav_n); }
    if(getenv("CC_AUDIODUMP")){ extern int cc_dbg_ymw,cc_dbg_sytc,cc_dbg_nmi,cc_dbg_slaver;
        extern int cc_dbg_nmiset,cc_dbg_tmr,cc_dbg_irq,cc_dbg_nmien; int pc,iff,im; void cc_audio_dbg(int*,int*,int*); cc_audio_dbg(&pc,&iff,&im);
        extern int cc_dbg_tover; double cc_dbg_tper(void);
        printf("AUDIO: peak=%d YMw=%d SYTcmd=%d NMIset=%d nmiEn=%d slaveR=%d tmrSetup=%d tmrOver=%d INTfire=%d tper=%.6f  pc=%04x iff1=%d im=%d\n",amax,cc_dbg_ymw,cc_dbg_sytc,cc_dbg_nmiset,cc_dbg_nmien,cc_dbg_slaver,cc_dbg_tmr,cc_dbg_tover,cc_dbg_irq,cc_dbg_tper(),pc,iff,im);
        extern int cc_dbg_ymraw,cc_dbg_ssgraw,cc_dbg_keyon,cc_dbg_adpcma,cc_dbg_deltat,cc_dbg_iffever,cc_dbg_intok,cc_dbg_irq;
        printf("RAW: ymPeak=%d ssgPeak=%d FMkeyon=%d ADPCMAkey=%d DeltaT=%d  iff1_EVER=%d INT_accepted=%d/%d\n",cc_dbg_ymraw,cc_dbg_ssgraw,cc_dbg_keyon,cc_dbg_adpcma,cc_dbg_deltat,cc_dbg_iffever,cc_dbg_intok,cc_dbg_irq); }

    /* diagnostics */
    { long sn=0,pn=0,rn=0,spn=0;
      for(int i=0;i<0x10000;i+=2) if(cc_scn[i]||cc_scn[i+1]) sn++;
      for(int i=0;i<0x1000;i++) if(cc_pal[i]) pn++;
      for(int i=0;i<0x2000;i+=2) if(cc_road[i]||cc_road[i+1]) rn++;
      for(int i=0;i<0x700;i+=2) if(cc_spr[i]||cc_spr[i+1]) spn++;
      printf("ROADDBG palbank=%d roadpal@%d: %04x %04x %04x %04x\n", cc_road_palbank, (cc_road_palbank<<6)*16, cc_pal[(cc_road_palbank<<6)*16], cc_pal[(cc_road_palbank<<6)*16+1], cc_pal[(cc_road_palbank<<6)*16+2], cc_pal[(cc_road_palbank<<6)*16+8]);
      printf("DIAG scnNZ=%ld palNZ=%ld roadNZ=%ld sprNZ=%ld  rom_main[8c6..]=%02x%02x%02x%02x\n",
             sn,pn,rn,spn, cc_rom_main[0x8c6],cc_rom_main[0x8c7],cc_rom_main[0x8c8],cc_rom_main[0x8c9]); }

    static uint16_t buf[CC_H*CC_W];
    cc_render_frame(buf, CC_W);

    FILE*f=fopen("cc_hosttest.ppm","wb"); fprintf(f,"P6\n%d %d\n255\n",CC_W,CC_H);
    for(int i=0;i<CC_W*CC_H;i++){ unsigned d=cc_pal[buf[i]&0xfff]&0x7fff;   /* TRUE colour = per-frame AGA output */
        int R=(d>>10)&0x1f,G=(d>>5)&0x1f,B=d&0x1f;
        fputc((R<<3)|(R>>2),f);fputc((G<<3)|(G>>2),f);fputc((B<<3)|(B>>2),f); }
    fclose(f);
    if(getenv("CC_SPRDUMP")){
        for(int o=0;o<0x700/2;o+=4){
            int d0=(cc_spr[o*2]<<8)|cc_spr[o*2+1], d1=(cc_spr[(o+1)*2]<<8)|cc_spr[(o+1)*2+1];
            int d2=(cc_spr[(o+2)*2]<<8)|cc_spr[(o+2)*2+1], d3=(cc_spr[(o+3)*2]<<8)|cc_spr[(o+3)*2+1];
            int tile=d1&0x7ff; if(!tile) continue;
            printf("  spr[%d] tile=%03x x=%d y=%d zoomx=%d zoomy=%d color=%02x\n",
                o/4, tile, d2&0x1ff, d0&0x1ff, (d3&0x7f)+1, ((d0&0xfe00)>>9)+1, (d3&0xff00)>>8);
        }
    }
    if(getenv("CC_PALDUMP")){   /* append all nonzero palette colors (xRGB555) for offline palette gen */
        FILE*pf=fopen("palsample.bin","ab");
        for(int i=0;i<0x1000;i++){ unsigned d=cc_pal[i]; if(d){ fputc(d&0xff,pf); fputc((d>>8)&0xff,pf); } }
        fclose(pf);
    }
    if(getenv("CC_PALCHECK")){
        int nz=0,crush=0;
        for(int i=0;i<0x1000;i++){ unsigned d=cc_pal[i]; if(!d)continue; nz++;
            int r=(d>>10)&0x1f,g=(d>>5)&0x1f,b=d&0x1f;
            int p=((r>>2)<<5)|((g>>2)<<2)|(b>>3);   /* 3-3-2 */
            if(p==0) crush++;  /* nonzero color that 3-3-2 maps to pure black */
        }
        printf("PALCHECK: %d nonzero palette colors, %d crush to 3-3-2 BLACK (%.0f%%)\n",
               nz,crush, nz?100.0*crush/nz:0);
    }
    if(getenv("CC_SCROLLDUMP")){
        int bgv=0,fgv=0; int bg0=(cc_scn[0xc000]<<8)|cc_scn[0xc001], fg0=(cc_scn[0xc400]<<8)|cc_scn[0xc401];
        for(int i=0;i<256;i++){
            int bg=(cc_scn[0xc000+i*2]<<8)|cc_scn[0xc000+i*2+1];
            int fg=(cc_scn[0xc400+i*2]<<8)|cc_scn[0xc400+i*2+1];
            if(bg!=bg0)bgv++; if(fg!=fg0)fgv++;
        }
        printf("SCROLL bgscroll_ram: %d/256 rows differ from row0(%04x); fgscroll_ram: %d/256 differ from row0(%04x)\n",bgv,bg0,fgv,fg0);
        printf("  ctrl: bgsx=%04x fgsx=%04x bgsy=%04x fgsy=%04x dis=%04x\n",
            (cc_scnctl[0]<<8)|cc_scnctl[1],(cc_scnctl[2]<<8)|cc_scnctl[3],
            (cc_scnctl[6]<<8)|cc_scnctl[7],(cc_scnctl[8]<<8)|cc_scnctl[9],(cc_scnctl[12]<<8)|cc_scnctl[13]);
        printf("  bgscroll rows 0,30,60,90,120,150,180,210: ");
        for(int r=0;r<=210;r+=30) printf("%04x ",(cc_scn[0xc000+r*2]<<8)|cc_scn[0xc000+r*2+1]);
        printf("\n  fgscroll rows 0,30,60,90,120,150,180,210: ");
        for(int r=0;r<=210;r+=30) printf("%04x ",(cc_scn[0xc400+r*2]<<8)|cc_scn[0xc400+r*2+1]);
        printf("\n");
    }
    if(getenv("CC_TXDUMP")){ int shown=0;
      for(int r=0;r<4 && shown<200;r++) for(int col=0;col<64 && shown<200;col++){
        int a=(cc_scn[0x4000+(r*64+col)*2]<<8)|cc_scn[0x4000+(r*64+col)*2+1];
        int code=a&0xff, c=(a>>8)&0x3f; if(code==0||code==0x20)continue;
        printf("TX r%d code=%02x color=%d -> pen%d pal[%d]=%04x pal[%d]=%04x pal[%d]=%04x\n",
          r,code,c, c*16, c*16+1,cc_pal[c*16+1], c*16+2,cc_pal[c*16+2], c*16+3,cc_pal[c*16+3]); shown++; }
    }
      printf("PAL 64-79:"); for(int i=64;i<80;i++) printf(" %04x",cc_pal[i]); printf("\n");
      printf("PAL 268-283:"); for(int i=268;i<284;i++) printf(" %04x",cc_pal[i]); printf("\n");
      printf("nonzero RED-ish pal in 0..400:"); for(int i=0;i<400;i++){ unsigned d=cc_pal[i]; int R=(d>>10)&0x1f,G=(d>>5)&0x1f,B=d&0x1f; if(R>=0x18&&G<0x10&&B<0x10) printf(" [%d]=%04x",i,d);} printf("\n");
    if(getenv("CC_NCOL")){
        static unsigned char seenpen[4096]; static unsigned char seencol[32768];
        int np=0,nc=0;
        for(int i=0;i<CC_W*CC_H;i++){ int p=buf[i]&0xfff; if(!seenpen[p]){seenpen[p]=1;np++;} int c=cc_pal[p]&0x7fff; if(!seencol[c]){seencol[c]=1;nc++;} }
        printf("NCOL: distinct pens on screen=%d, distinct colours=%d\n",np,nc);
    }
    if(getenv("CC_RPB")){ extern int cc_road_palbank; int po=cc_road_palbank<<6;
        printf("RPB: road_palbank=%d palette_offs=%d  road body pens [(po+0)<<4 +1..3]: %d=%04x %d=%04x %d=%04x\n",
          cc_road_palbank, po, (po<<4)+1,cc_pal[(po<<4)+1], (po<<4)+2,cc_pal[(po<<4)+2], (po<<4)+3,cc_pal[(po<<4)+3]);
    }
    printf("wrote cc_hosttest.ppm (%d frames)\n",frames);
    return 0;
}
