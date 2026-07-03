/* cc_hosttest.c -- native sanity test of the ACTUAL Amiga source (cc_machine.c +
 * cc_render.c). Provides the embedded-ROM symbols by loading the data/ files,
 * runs N frames, writes a PPM (3-3-2 -> RGB). Proves the Amiga code path before
 * testing in WinUAE.  Build: see build_hosttest.sh */
#include <stdio.h>
#include <stdlib.h>
#include <stdint.h>
#include "cc_state.h"

/* the symbols cc_romdata.S provides on Amiga -- here loaded from files */
uint8_t cc_rom_main[0x80000], cc_rom_sub[0x20000];
uint8_t cc_gfx_scn[0x80000], cc_gfx_road[0x80000], cc_gfx_spr[0x200000], cc_gfx_spr2[0x200000], cc_gfx_smap[0x80000];
uint8_t cc_rom_motor[0x8000], cc_proms[0x23000];
uint8_t cc_pal256[768], cc_lut32k[32768];
uint8_t cc_rom_audio[0x10000];
uint8_t cc_adpcma[0x180000], cc_adpcmb[0x80000];

static void ld(const char*p, const void*d, int n){ FILE*f=fopen(p,"rb"); if(!f){perror(p);exit(1);} fread((void*)d,1,n,f); fclose(f); }

void *cc_big_alloc(unsigned long n){ return calloc(1, n); }   /* render caches */

int main(int argc,char**argv){
    int frames=(argc>1)?atoi(argv[1]):1800;
    const char *dswa_env = getenv("CHQ_DSWA");
    int coin_frame = getenv("CHQ_COIN_FRAME") ? atoi(getenv("CHQ_COIN_FRAME")) : 120;
    int start_frame = getenv("CHQ_START_FRAME") ? atoi(getenv("CHQ_START_FRAME")) : 200;
    int gas_frame = getenv("CHQ_GAS_FRAME") ? atoi(getenv("CHQ_GAS_FRAME")) : 230;
    int pulse_frames = getenv("CHQ_PULSE_FRAMES") ? atoi(getenv("CHQ_PULSE_FRAMES")) : 10;
    if(pulse_frames < 1) pulse_frames = 1;
    ld("data/maincpu.bin",cc_rom_main,0x80000);
    ld("data/sub.bin",cc_rom_sub,0x20000);
    ld("data/gfx_scn.bin",cc_gfx_scn,0x80000);
    ld("data/gfx_road.bin",cc_gfx_road,0x80000);
    ld("data/sprites_asm.bin",cc_gfx_spr,0x200000);
    ld("data/sprites2_asm.bin",cc_gfx_spr2,0x200000);
    ld("data/spritemap_asm.bin",cc_gfx_smap,0x80000);
    ld("data/motorcpu.bin",cc_rom_motor,0x8000);
    ld("data/proms.bin",cc_proms,0x23000);
    ld("data/cc_pal256.bin",cc_pal256,768);
    ld("data/cc_lut32k.bin",cc_lut32k,32768);
    ld("data/audiocpu.bin",cc_rom_audio,0x10000);
    ld("data/adpcma.bin",cc_adpcma,0x180000);
    ld("data/adpcmb.bin",cc_adpcmb,0x80000);

    if(dswa_env) cc_set_inputs(0x33,0x3f,(uint8_t)strtoul(dswa_env,0,0),0xff,0);
    cc_machine_init();
    cc_audio_init();
    if(getenv("CC_TESTNOTE")){ void cc_audio_testnote(void); cc_audio_testnote();
        static signed char tp[4000]; int tmax=0;
        for(int r=0;r<8;r++){ cc_audio_render(tp,4000); for(int j=0;j<4000;j++){int v=tp[j];if(v<0)v=-v;if(v>tmax)tmax=v;} }
        extern int cc_dbg_ymraw,cc_dbg_yminit; int pc,iff,im; void cc_audio_dbg(int*,int*,int*); cc_audio_dbg(&pc,&iff,&im); extern int cc_dbg_ymok_v; printf("TESTNOTE: outPeak=%d ymRawPeak=%d YM2610Init_ret=%d ym_ok=%d\n",tmax,cc_dbg_ymraw,cc_dbg_yminit,cc_dbg_ymok_v); return 0; }
    static signed char pcm[22050];
    int amax=0; long anz=0;
    for(int i=0;i<frames;i++){
        if(getenv("CC_COIN")){                 /* Chase H.Q. upright digital cabinet raw inputs. */
            uint8_t in0 = 0x33, in1 = 0x3f;
            if(i >= coin_frame && i < coin_frame + pulse_frames) in0 |= 0x04;
            if(i >= start_frame && i < start_frame + pulse_frames) in1 &= (uint8_t)~0x08;
            if(i >= gas_frame) in1 &= (uint8_t)~0x20;
            cc_set_inputs(in0,in1,0xff,0xff,0);
        }
        cc_machine_run_frame(); cc_audio_run_frame();
        if(getenv("CC_RENDER_EVERY")){   /* profiling: mirror the Amiga per-frame video path */
            static uint16_t pbuf[CC_H*CC_W];
            static signed char apcm[22050/60 + 4];
            cc_render_frame(pbuf, CC_W);
            cc_audio_render(apcm, 22050/60);
        }
        if(getenv("CHQ_SUBTRACE")){
            int lo = atoi(getenv("CHQ_SUBTRACE")); int hi = lo + 200;
            if(i >= lo && i < hi){
                unsigned mpc=0,spc=0; int son=0; unsigned cc2=0,cl2=0;
                uint16_t sh1a=0,sh1e=0,sha6=0,s802=0,s804=0,s80a=0,s1a58=0,s1a5a=0;
                cc_machine_debug(&mpc,&spc,&son,&cc2,&cl2);
                cc_machine_debug_bus(&sh1a,&sh1e,&sha6,&s802,&s804,&s80a,&s1a58,&s1a5a);
                printf("ST %d main=%06x sub=%06x on=%d ctrl=%02x cnt=%u sh14=%04x sha6=%04x s804=%04x\n",
                       i,mpc,spc,son,cl2,cc2,
                       (unsigned)cc_machine_debug_main_word(0)*0 + (unsigned)((cc_bus_read16(1,0x108014))),
                       (unsigned)cc_bus_read16(1,0x1080a6), s804);
            }
        }
        if(getenv("CHQ_FRAMELOG") && (i % 60) == 0){
            unsigned mpc=0,spc=0,msr=0,ssr=0,mcy=0,scy=0; int son=0; unsigned cc=0,cl=0;
            cc_machine_debug(&mpc,&spc,&son,&cc,&cl);
            cc_machine_debug_cpu(&msr,&ssr,&mcy,&scy);
            printf("FRAME %d mainPC=%06x mainSR=%04x subPC=%06x subSR=%04x sub_on=%d\n",
                   i,mpc,msr,spc,ssr,son);
        }
        if(getenv("CC_AUDIODUMP")){
            static FILE *pcmf; static int pcmf_tried;
            cc_audio_render(pcm, 22050/60);
            for(int j=0;j<22050/60;j++){ int v=pcm[j]; if(v<0)v=-v; if(v>amax)amax=v; if(v)anz++; }
            if(!pcmf_tried){ pcmf_tried=1; const char*fn=getenv("CC_PCMDUMP"); if(fn) pcmf=fopen(fn,"wb"); }
            if(pcmf) fwrite(pcm, 1, 22050/60, pcmf);
        }
    }
    if(getenv("CC_AUDIODUMP")){ extern int cc_dbg_ymw,cc_dbg_sytc,cc_dbg_nmi,cc_dbg_slaver;
        extern int cc_dbg_nmiset,cc_dbg_tmr,cc_dbg_irq,cc_dbg_nmien; int pc,iff,im; void cc_audio_dbg(int*,int*,int*); cc_audio_dbg(&pc,&iff,&im);
        extern int cc_dbg_tover; double cc_dbg_tper(void);
        printf("AUDIO: peak=%d YMw=%d SYTcmd=%d NMIset=%d nmiEn=%d slaveR=%d tmrSetup=%d tmrOver=%d INTfire=%d tper=%.6f  pc=%04x iff1=%d im=%d\n",amax,cc_dbg_ymw,cc_dbg_sytc,cc_dbg_nmiset,cc_dbg_nmien,cc_dbg_slaver,cc_dbg_tmr,cc_dbg_tover,cc_dbg_irq,cc_dbg_tper(),pc,iff,im);
        extern int cc_dbg_ymraw,cc_dbg_keyon,cc_dbg_adpcma,cc_dbg_deltat,cc_dbg_iffever,cc_dbg_intok,cc_dbg_irq;
        extern unsigned cc_dbg_ymhash;   /* FNV-1a over every YM (addr,val) write pair -- interp vs native must match bit-exactly */
        printf("RAW: ymPeak=%d FMkeyon=%d ADPCMAkey=%d DeltaT=%d ymHash=%08x  iff1_EVER=%d INT_accepted=%d/%d\n",cc_dbg_ymraw,cc_dbg_keyon,cc_dbg_adpcma,cc_dbg_deltat,cc_dbg_ymhash,cc_dbg_iffever,cc_dbg_intok,cc_dbg_irq);
#ifdef CHQ_AUDIO_NATIVE
        { extern unsigned long long chq_aud_ione_hits(void);   /* transcode's uncovered-PC interp fallback (host feature) */
          printf("NATIVE: interp_one_fallback_hits=%llu\n", chq_aud_ione_hits()); }
#endif
    }
    if(getenv("CHQ_COVERAGE")){
        cc_machine_coverage_dump(getenv("CHQ_COVERAGE"));
    }
    if(getenv("CHQ_Z80COV")){
        void cc_audio_z80cov_dump(const char *path);
        cc_audio_z80cov_dump(getenv("CHQ_Z80COV"));
    }
    if(getenv("CHQ_Z80HIST")){
        void cc_audio_z80hist_dump(const char *path);
        cc_audio_z80hist_dump(getenv("CHQ_Z80HIST"));
    }

    /* diagnostics */
    { long sn=0,pn=0,rn=0,spn=0;
      unsigned main_pc=0, sub_pc=0, ctrl_count=0, ctrl_last=0; int sub_on=0;
      unsigned irq_main=0, irq_sub=0, task_active=0; uint16_t task_first[8]={0};
      unsigned sr_main=0, sr_sub=0, cyc_main=0, cyc_sub=0;
      unsigned mregs[4]={0};
      uint16_t sh1a=0,sh1e=0,sha6=0,s802=0,s804=0,s80a=0,s1a58=0,s1a5a=0;
      uint8_t ioc_port=0; unsigned ioc_counts[16]={0};
      cc_machine_debug(&main_pc,&sub_pc,&sub_on,&ctrl_count,&ctrl_last);
      cc_machine_debug_cpu(&sr_main,&sr_sub,&cyc_main,&cyc_sub);
      cc_machine_debug_main_regs(mregs);
      cc_machine_debug_irq(&irq_main,&irq_sub);
      cc_machine_debug_tasks(&task_active,task_first);
      cc_machine_debug_bus(&sh1a,&sh1e,&sha6,&s802,&s804,&s80a,&s1a58,&s1a5a);
      cc_machine_debug_ioc(&ioc_port,ioc_counts);
      for(int i=0;i<0x10000;i+=2) if(cc_scn[i]||cc_scn[i+1]) sn++;
      for(int i=0;i<0x1000;i++) if(cc_pal[i]) pn++;
      for(int i=0;i<0x2000;i+=2) if(cc_road[i]||cc_road[i+1]) rn++;
      for(int i=0;i<0x800;i+=2) if(cc_spr[i]||cc_spr[i+1]) spn++;
      printf("CPUDBG mainPC=%06x subPC=%06x sub_on=%d ctrl_count=%u ctrl_last=%02x\n",
             main_pc,sub_pc,sub_on,ctrl_count,ctrl_last);
      printf("CPUSTATE mainSR=%04x subSR=%04x cycles main=%u sub=%u\n",
             sr_main,sr_sub,cyc_main,cyc_sub);
      printf("MAINREG D0=%08x A0=%08x A5=%08x A7=%08x\n",
             mregs[0],mregs[1],mregs[2],mregs[3]);
      printf("IRQDBG main=%u sub=%u tasks=%u first:", irq_main,irq_sub,task_active);
      for(int ii=0;ii<8;ii++) printf(" %04x",task_first[ii]);
      printf("\n");
      printf("BUSDBG sh[1a]=%04x sh[1e]=%04x sh[a6]=%04x sub[0802]=%04x sub[0804]=%04x sub[080a]=%04x sub[1a58]=%04x sub[1a5a]=%04x\n",
             sh1a,sh1e,sha6,s802,s804,s80a,s1a58,s1a5a);
      printf("MAINDBG ram[0124]=%04x ram[7edc]=%04x\n",
             cc_machine_debug_main_word(0x0124), cc_machine_debug_main_word(0x7edc));
      { uint32_t task_sp = cc_machine_debug_main_long(0x0002);
        unsigned spoff = (task_sp >= 0x100000 && task_sp < 0x108000) ? task_sp - 0x100000 : 0xffffffffu;
        printf("TASKDBG slot0 timer=%04x sp=%08x off=%04x stack:",
               cc_machine_debug_main_word(0), task_sp, spoff == 0xffffffffu ? 0xffffu : spoff);
        if(spoff != 0xffffffffu && spoff + 64 < 0x8000){
            for(int ii=0; ii<16; ii++) printf(" %08x", cc_machine_debug_main_long(spoff + ii*4));
        }
        printf("\n");
      }
      printf("IOCDBG port=%02x counts:", ioc_port);
      for(int ii=0;ii<16;ii++) if(ioc_counts[ii]) printf(" %x=%u",ii,ioc_counts[ii]);
      printf("\n");
      printf("ROADDBG palbank=%d roadpal@%d: %04x %04x %04x %04x\n", cc_road_palbank, (cc_road_palbank<<6)*16, cc_pal[(cc_road_palbank<<6)*16], cc_pal[(cc_road_palbank<<6)*16+1], cc_pal[(cc_road_palbank<<6)*16+2], cc_pal[(cc_road_palbank<<6)*16+8]);
      printf("DIAG scnNZ=%ld palNZ=%ld roadNZ=%ld sprNZ=%ld  rom_main[8c6..]=%02x%02x%02x%02x\n",
             sn,pn,rn,spn, cc_rom_main[0x8c6],cc_rom_main[0x8c7],cc_rom_main[0x8c8],cc_rom_main[0x8c9]); }

    static uint16_t buf[CC_H*CC_W];
    cc_render_frame(buf, CC_W);

    FILE*f=fopen("cc_hosttest.ppm","wb"); fprintf(f,"P6\n%d %d\n255\n",CC_W,CC_H);
    for(int i=0;i<CC_W*CC_H;i++){ unsigned d=cc_pal[buf[i]&0xfff]&0x7fff;   /* TRUE colour = per-frame RTG output */
        int R=d&0x1f,G=(d>>5)&0x1f,B=(d>>10)&0x1f;
        fputc((R<<3)|(R>>2),f);fputc((G<<3)|(G>>2),f);fputc((B<<3)|(B>>2),f); }
    fclose(f);
    if(getenv("CC_SPRDUMP")){
        for(int o=0;o<0x800/2;o+=4){
            int d0=(cc_spr[o*2]<<8)|cc_spr[o*2+1], d1=(cc_spr[(o+1)*2]<<8)|cc_spr[(o+1)*2+1];
            int d2=(cc_spr[(o+2)*2]<<8)|cc_spr[(o+2)*2+1], d3=(cc_spr[(o+3)*2]<<8)|cc_spr[(o+3)*2+1];
            int tile=d1&0x7ff; if(!tile) continue;
            printf("  spr[%d] d=%04x,%04x,%04x,%04x tile=%03x x=%d y=%d zoomx=%d zoomy=%d color=%02x\n",
                o/4, d0,d1,d2,d3, tile, d2&0x1ff, d0&0x1ff, (d3&0x7f)+1, ((d0&0xfe00)>>9)+1, (d3&0xff00)>>8);
        }
    }
    if(getenv("CC_PALDUMP")){   /* append all nonzero palette colors (XBGR555) for offline palette gen */
        FILE*pf=fopen("palsample.bin","ab");
        for(int i=0;i<0x1000;i++){ unsigned d=cc_pal[i]; if(d){ fputc(d&0xff,pf); fputc((d>>8)&0xff,pf); } }
        fclose(pf);
    }
    if(getenv("CC_PALCHECK")){
        int nz=0,crush=0;
        for(int i=0;i<0x1000;i++){ unsigned d=cc_pal[i]; if(!d)continue; nz++;
            int r=d&0x1f,g=(d>>5)&0x1f,b=(d>>10)&0x1f;
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
      printf("nonzero RED-ish pal in 0..400:"); for(int i=0;i<400;i++){ unsigned d=cc_pal[i]; int R=d&0x1f,G=(d>>5)&0x1f,B=(d>>10)&0x1f; if(R>=0x18&&G<0x10&&B<0x10) printf(" [%d]=%04x",i,d);} printf("\n");
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
    if(getenv("CC_RPALDUMP")){
        for(int base=0xc00; base<0xd40; base+=0x10){
            printf("RPAL %03x:", base);
            for(int i=0;i<16;i++) printf(" %04x", cc_pal[base+i]);
            printf("\n");
        }
    }
    if(getenv("CC_PALGROUPS")){
        for(int base=0x00; base<0x200; base+=0x10){
            printf("PALG %03x:", base);
            for(int i=0;i<16;i++) printf(" %04x", cc_pal[base+i]);
            printf("\n");
        }
    }
    printf("wrote cc_hosttest.ppm (%d frames)\n",frames);
    return 0;
}
