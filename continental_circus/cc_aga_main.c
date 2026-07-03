/* cc_aga_main.c -- Continental Circus NATIVE AGA presenter (stock AGA, NO Picasso96).
 * PAL:LowRes 320x256 8-bitplane (256-colour) screen. The 320x224 game is scaled to a
 * 256x224 centred hole (4/5 horizontal, 1:1 vertical) inside a GB-style procedural
 * bezel (steel side panels + gold rim). Chunky -> planar (C2P) into an OS double-buffer
 * (AllocScreenBuffer + ChangeScreenBuffer), WaitTOF-paced @50Hz. Machine/renderer/audio
 * are SHARED with the RTG build; only the present layer differs.
 * Adapted from 1943kai/rtg_interp/c1943_aga_main.c (word-store c2p + double-buffer).
 * Keyboard: 5=coin 1=start arrows=steer Up/fire=gas P=pause Esc=quit. + joystick P1. */
#include <exec/exec.h>
#include <exec/memory.h>
#include <graphics/gfxbase.h>
#include <graphics/displayinfo.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <stdint.h>
#include <string.h>
#include "cc_state.h"

struct IntuitionBase *IntuitionBase=0;
struct GfxBase *GfxBase=0;

#define DISP_W 320
#define DISP_H 256
#define RGW    256          /* game hole width  (320 scaled 4/5) -- 16-aligned for word c2p */
#define RGH    224          /* game hole height (1:1) */
#define GX0    32           /* (320-256)/2 -- 16-aligned */
#define GY0    16           /* (256-224)/2 */
/* bezel palette slots: game uses 0..239 (LUT capped), 240..255 reserved for the bezel */
#define BZ_BASE 0xF0
#define BZ_STEPS 8
#define BZ_RIM  0xF8

static struct Screen *scr; static struct Window *win;
static struct ScreenBuffer *sb[2];
static int back=1, ok;
static uint16_t *game;       /* 320x224 pens from the renderer (FAST RAM) */
static uint8_t  *idx8;       /* 256x224 scaled CLUT indices = c2p source (FAST RAM) */
static uint8_t  *bezel;      /* 320x256 full-screen bezel chunky (FAST RAM) */
static uint32_t loadrgb[1+256*3+1];
static uint8_t keydown[128];
static int g_quit, g_pause;

#define RK_1 0x01
#define RK_5 0x05
#define RK_P 0x19
#define RK_ESC 0x45
#define RK_UP 0x4C
#define RK_DOWN 0x4D
#define RK_RIGHT 0x4E
#define RK_LEFT 0x4F
#define JOY1DAT (*(volatile unsigned short*)0xdff00cUL)
#define CIAA_PRA (*(volatile unsigned char*)0xbfe001UL)

/* ---- chunky -> 8 bitplanes (word-store C2P). dx,w MUST be multiples of 16. ---- */
static void c2p_region(struct BitMap *bm, const uint8_t *src, int src_stride, int dx, int dy, int w, int h){
    int bpr = bm->BytesPerRow;
    uint8_t *p0=bm->Planes[0],*p1=bm->Planes[1],*p2=bm->Planes[2],*p3=bm->Planes[3];
    uint8_t *p4=bm->Planes[4],*p5=bm->Planes[5],*p6=bm->Planes[6],*p7=bm->Planes[7];
    for(int y=0; y<h; y++){
        const uint8_t *s = src + (size_t)y*src_stride;
        int rowoff = (dy+y)*bpr;
        for(int x=0; x<w; x+=16){
            const uint8_t *q = s + x;
            uint16_t w0=0,w1=0,w2=0,w3=0,w4=0,w5=0,w6=0,w7=0;
            for(int k=0;k<16;k++){
                unsigned pen=q[k]; uint16_t bit=(uint16_t)(0x8000u>>k);
                if(pen&0x01)w0|=bit; if(pen&0x02)w1|=bit; if(pen&0x04)w2|=bit; if(pen&0x08)w3|=bit;
                if(pen&0x10)w4|=bit; if(pen&0x20)w5|=bit; if(pen&0x40)w6|=bit; if(pen&0x80)w7|=bit;
            }
            int o = rowoff + ((dx+x)>>3);
            *(uint16_t*)(p0+o)=w0; *(uint16_t*)(p1+o)=w1; *(uint16_t*)(p2+o)=w2; *(uint16_t*)(p3+o)=w3;
            *(uint16_t*)(p4+o)=w4; *(uint16_t*)(p5+o)=w5; *(uint16_t*)(p6+o)=w6; *(uint16_t*)(p7+o)=w7;
        }
    }
}

/* STABLE persistent-slot palette: a colour keeps the SAME slot across frames, so the
 * CLUT barely changes frame-to-frame and stays in sync with the double-buffer. (The
 * old reassign-every-frame scheme made slot->colour change each frame, which desyncs
 * vs the displayed buffer on real hardware -> flickering/wrong colours, even though a
 * non-double-buffered host render looked fine.) New colours take a free slot, else
 * evict the least-recently-used slot not touched this frame; only a (re)assigned slot
 * rewrites its LoadRGB32 entry. Scale 320->256 (dx*5/4) + remap in the same pass. */
static short  col2slot[32768];          /* colour -> slot (-1 = none) */
static int    slot_col[256];            /* slot  -> colour (-1 = free) */
static unsigned short slot_age[256], fstamp;
static int    clut_inited;
static void set_slot_color(int s,int col){
    int R=(col>>10)&0x1f,G=(col>>5)&0x1f,B=col&0x1f;
    loadrgb[1+s*3+0]=((R<<3)|(R>>2))*0x01010101u;
    loadrgb[1+s*3+1]=((G<<3)|(G>>2))*0x01010101u;
    loadrgb[1+s*3+2]=((B<<3)|(B>>2))*0x01010101u;
}
static void build_clut_and_remap(void){
    if(!clut_inited){ memset(col2slot,0xff,sizeof col2slot); for(int k=0;k<256;k++) slot_col[k]=-1; clut_inited=1; }
    fstamp++;
    for(int dy=0; dy<RGH; dy++){
        const uint16_t *srow = game + (size_t)dy*CC_W;
        uint8_t *drow = idx8 + (size_t)dy*RGW;
        for(int dx=0; dx<RGW; dx++){
            int col = cc_pal[srow[(dx*5)>>2]&0xfff] & 0x7fff;
            int s = col2slot[col];
            if(s<1 || s>239 || slot_col[s]!=col){             /* not currently assigned */
                s=-1; int lru=-1; unsigned lru_age=0xffff;
                for(int k=1;k<240;k++){
                    if(slot_col[k]<0){ s=k; break; }
                    if(slot_age[k]!=fstamp && slot_age[k]<=lru_age){ lru_age=slot_age[k]; lru=k; }
                }
                if(s<0) s=(lru>0)?lru:1;
                if(slot_col[s]>=0) col2slot[slot_col[s]]=-1;  /* evict old owner */
                slot_col[s]=col; col2slot[col]=s; set_slot_color(s,col);
            }
            slot_age[s]=fstamp;
            drow[dx]=(uint8_t)s;
        }
    }
}

/* Real bezel art (The Bezel Project contcirc.png, resized 320x256, quantised to
 * slots 240..255, game hole = 0). Loaded from embedded data, C2P'd into BOTH buffers
 * so the frame persists; per frame only the game hole is redrawn. */
static void draw_bezel(void){
    memcpy(bezel, cc_bezel_data, DISP_W*DISP_H);
    if(sb[0]) c2p_region(sb[0]->sb_BitMap, bezel, DISP_W, 0,0, DISP_W, DISP_H);
    if(sb[1]) c2p_region(sb[1]->sb_BitMap, bezel, DISP_W, 0,0, DISP_W, DISP_H);
}

/* init the LoadRGB32 table: header, slot 0 = black, bezel art in 240..255 (constant);
 * the game slots 1..239 are filled fresh each frame by build_clut_and_remap(). */
static void upload_palette(void){
    loadrgb[0]=((uint32_t)256<<16)|0;
    loadrgb[1]=loadrgb[2]=loadrgb[3]=0;               /* slot 0 = black */
    for(int i=0;i<16;i++){                             /* bezel art palette -> slots 240..255 */
        loadrgb[1+(240+i)*3+0]=(uint32_t)cc_bezelpal[i*3+0]*0x01010101u;
        loadrgb[1+(240+i)*3+1]=(uint32_t)cc_bezelpal[i*3+1]*0x01010101u;
        loadrgb[1+(240+i)*3+2]=(uint32_t)cc_bezelpal[i*3+2]*0x01010101u;
    }
    loadrgb[1+256*3]=0;
    LoadRGB32(&scr->ViewPort, loadrgb);
}

static void poll_input(void){
    static int c1p,s1p,c1h,s1h,p_prev; int steer=0;
    struct IntuiMessage *m;
    if(win&&win->UserPort)
        while((m=(struct IntuiMessage*)GetMsg(win->UserPort))){
            ULONG cls=m->Class; UWORD raw=m->Code; ReplyMsg((struct Message*)m);
            if(cls==IDCMP_RAWKEY) keydown[raw&0x7f]=(raw&0x80)?0:1;
        }
    if(keydown[RK_ESC]) g_quit=1;
    if(keydown[RK_P] && !p_prev) g_pause=!g_pause; p_prev=keydown[RK_P];
    unsigned v=JOY1DAT; int right=(v>>1)&1,left=(v>>9)&1;
    if(keydown[RK_RIGHT]||right) steer=+0x40;
    if(keydown[RK_LEFT]||left)   steer=-0x40;
    int fire=!(CIAA_PRA & 0x80);
    uint8_t in0=0x13, in1=0x1f;
    int coin=keydown[RK_5], start=keydown[RK_1];
    if(coin&&!c1p) c1h=8; c1p=coin;
    if(start&&!s1p) s1h=8; s1p=start;
    if(c1h){ in0|=0x08; c1h--; }
    if(s1h){ in1&=~0x08; s1h--; }
    if(keydown[RK_UP]||fire) in0|=0x80;  /* MAME gas_pedal_r raw 7 -> encoded 4, shifted to IN0 bits 5..7 */
    cc_set_inputs(in0,in1,0xff,0xcf,steer);
}

#include <stdio.h>
static FILE *lg; static void L(const char*s){ if(lg){fputs(s,lg);fputc('\n',lg);fflush(lg);} }

static void shutdown(void){
    static int done; if(done)return; done=1;
    cc_audio_amiga_close();
    WaitTOF();
    if(win){ CloseWindow(win); win=0; }
    if(scr){
        WaitTOF();
        if(sb[0]){ FreeScreenBuffer(scr,sb[0]); sb[0]=0; }
        if(sb[1]){ FreeScreenBuffer(scr,sb[1]); sb[1]=0; }
        CloseScreen(scr); scr=0;
    }
    if(game)  FreeMem(game, CC_W*CC_H*2);
    if(idx8)  FreeMem(idx8, RGW*RGH);
    if(bezel) FreeMem(bezel, DISP_W*DISP_H);
}

static void *afast(int n){ void*p=AllocMem(n,MEMF_FAST|MEMF_CLEAR); if(!p)p=AllocMem(n,MEMF_ANY|MEMF_CLEAR); return p; }

int main(void){
    lg=fopen("cc_log.txt","w"); L("cc_aga start");
    IntuitionBase=(struct IntuitionBase*)OpenLibrary((CONST_STRPTR)"intuition.library",39);
    GfxBase=(struct GfxBase*)OpenLibrary((CONST_STRPTR)"graphics.library",39);
    if(!IntuitionBase||!GfxBase){ L("FAIL libs"); return 20; }
    game =(uint16_t*)afast(CC_W*CC_H*2);
    idx8 =(uint8_t*) afast(RGW*RGH);
    bezel=(uint8_t*) afast(DISP_W*DISP_H);
    if(!game||!idx8||!bezel){ L("FAIL bufs"); return 21; }

    scr=OpenScreenTags(0, SA_Width,DISP_W, SA_Height,DISP_H, SA_Depth,8,
                       SA_DisplayID,LORES_KEY, SA_Quiet,1, SA_Type,CUSTOMSCREEN, SA_ShowTitle,0, TAG_END);
    if(scr) L("OpenScreen LORES_KEY");
    if(!scr){
        scr=OpenScreenTags(0, SA_Width,DISP_W, SA_Height,DISP_H, SA_Depth,8,
                           SA_DisplayID,0x21000UL, SA_Quiet,1, SA_Type,CUSTOMSCREEN, SA_ShowTitle,0, TAG_END);
        if(scr) L("OpenScreen 0x21000");
    }
    if(!scr){
        scr=OpenScreenTags(0, SA_Width,DISP_W, SA_Height,DISP_H, SA_Depth,8,
                           SA_Quiet,1, SA_Type,CUSTOMSCREEN, SA_ShowTitle,0, TAG_END);
        if(scr) L("OpenScreen default");
    }
    if(!scr){ L("FAIL OpenScreen"); return 23; }
    { char b[64]; sprintf(b,"screen %dx%d d=%d",scr->Width,scr->Height,(int)scr->RastPort.BitMap->Depth); L(b); }
    win=OpenWindowTags(0, WA_CustomScreen,(ULONG)scr, WA_Left,0,WA_Top,0, WA_Width,DISP_W,WA_Height,DISP_H,
                       WA_Backdrop,TRUE,WA_Borderless,TRUE,WA_Activate,TRUE,WA_RMBTrap,TRUE,WA_IDCMP,IDCMP_RAWKEY,TAG_END);
    if(win){ ScreenToFront(scr); ActivateWindow(win); }

    sb[0]=AllocScreenBuffer(scr,NULL,SB_SCREEN_BITMAP);
    sb[1]=AllocScreenBuffer(scr,NULL,0);
    if(!sb[0]||!sb[1]){ L("FAIL AllocScreenBuffer"); shutdown(); return 24; }
    back=1; ok=1;
    upload_palette();
    draw_bezel();
    L("machine_init"); cc_machine_init(); cc_audio_init(); cc_audio_amiga_open();
    L("loop");

    while(!g_quit){
        poll_input();
        if(!g_pause){
            cc_machine_run_frame();
            cc_render_frame(game, CC_W);
            build_clut_and_remap();                              /* per-frame exact palette + scaled idx8 */
            c2p_region(sb[back]->sb_BitMap, idx8, RGW, GX0, GY0, RGW, RGH);
            LoadRGB32(&scr->ViewPort, loadrgb);                  /* same vblank as the flip -> no tear/blink */
            ChangeScreenBuffer(scr, sb[back]);
            back^=1;
            cc_audio_amiga_frame();
        }
        WaitTOF();
    }
    shutdown();
    CloseLibrary((struct Library*)GfxBase);
    CloseLibrary((struct Library*)IntuitionBase);
    return 0;
}
