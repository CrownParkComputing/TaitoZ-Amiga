/* cc_render.c -- Continental Circus software renderer (Amiga build).
 * Port of steps/03-video-decode/render.c: TC0100SCN (BG0/BG1/TX) + TC0110PCR
 * palette + TC0150ROD road (MAME BSD-3 port) + zoomed sprites. Outputs an 8-bit
 * 3-3-2 RGB chunky buffer (256-color RTG screen, no per-frame palette juggling). */
#include "cc_state.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static uint16_t road_layer[CC_H*CC_W];
static uint8_t  pri_layer[CC_H*CC_W];
static int      road_priority_switch_line;
static uint8_t  road_pix[0x40000 * 8];
static int      road_pix_ready;

static inline int r16(const uint8_t*p){ return (p[0]<<8)|p[1]; }
#ifdef __amigaos__
#define CC_GETENV(n) 0
#else
#define CC_GETENV(n) getenv(n)
#endif
/* xRGB555 pen -> 3-3-2 byte */
static inline uint8_t pal332(int pen){
    uint16_t d=cc_pal[pen&0xfff];
    int r=(d>>10)&0x1f, g=(d>>5)&0x1f, b=d&0x1f;
    return (uint8_t)(((r>>2)<<5)|((g>>2)<<2)|(b>>3));
}
static inline int bg_pix(unsigned code,int x,int y){
    unsigned off=(code*32u + y*4u + (x>>1)) & 0x7ffff;
    uint8_t b=cc_gfx_scn[off]; return (x&1)?(b&0xf):(b>>4);
}
static inline int tx_pix(unsigned code,int x,int y){
    const uint8_t*c=cc_scn + 0x6000 + code*16u + y*2u;
    return ((c[0]>>(7-x))&1) | (((c[1]>>(7-x))&1)<<1);
}

/* ---- TC0150ROD road (ported from MAME tc0150rod.cpp, BSD-3 N.Salmoria) ---- */
static inline int rgfx_word(unsigned w){ w&=0x3ffff; return (cc_gfx_road[w*2+1]<<8)|cc_gfx_road[w*2]; }
static inline int rd_ram(int idx){ if(idx<0||idx>=0x1000)return 0; return r16(cc_road+idx*2); }
static void road_transcode_once(void){
    if(road_pix_ready) return;
    for(unsigned w=0; w<0x40000; w++){
        int gw=rgfx_word(w);
        uint8_t *p=road_pix+w*8;
        for(int b=0;b<8;b++) p[b]=(uint8_t)((((gw>>(15-b))&1)<<1)|((gw>>(7-b))&1));
    }
    road_pix_ready=1;
}

static void draw_road(void){
    road_transcode_once();
    const int screen_width=CC_W, H=CC_H, type=1, road_trans=0, y_offs=-3;
    const int palette_offs=cc_road_palbank<<6, x_offs=0xa7;
    uint16_t roada_line[512], roadb_line[512];
    uint16_t pixel,color,gfx_word,pri,pixpri;
    uint16_t ra_cl,ra_cr,ra_bc, rb_cl,rb_cr,rb_bc;
    uint8_t pr[6]; int x_index,ri,ri2,i,xoffset,paloffs,palloffs,palroffs,tile,colbank,rc;
    int road_ctrl=rd_ram(0xfff);
    road_priority_switch_line = (road_ctrl & 0x00ff) - y_offs;
    int le,re,begin,end,r_over,l_over,lnd,dtr,bgonly;
    int rA=y_offs*4+((road_ctrl&0x300)<<2), rB=y_offs*4+((road_ctrl&0xc00)<<0);
    uint16_t *roada,*roadb;
    for(i=0;i<screen_width*H;i++) road_layer[i]=0x8000;
    for(int y=0;y<H;y++){
        lnd=0; ri=rA+y*4; ri2=rB+y*4; roada=roada_line; roadb=roadb_line;
        for(i=0;i<screen_width;i++){*roada++=0x8000;*roadb++=0x8000;}
        pr[0]=1;pr[1]=1;pr[2]=2;pr[3]=3;pr[4]=3;pr[5]=4;
        ra_cr=rd_ram(ri);ra_cl=rd_ram(ri+1);ra_bc=rd_ram(ri+2);
        rb_cr=rd_ram(ri2);rb_cl=rd_ram(ri2+1);rb_bc=rd_ram(ri2+2);
        if(ra_bc&0x2000)pr[2]+=2; if(rb_bc&0x2000)pr[2]+=1;
        if(ra_cl&0x2000)pr[3]-=1; if(rb_cl&0x2000)pr[3]-=2;
        if(ra_cr&0x2000)pr[4]-=1; if(rb_cr&0x2000)pr[4]-=2; if(pr[4]==0)pr[4]++;
        /* ROAD A */
        palroffs=(ra_cr&0x1000)>>11;palloffs=(ra_cl&0x1000)>>11;xoffset=ra_bc&0x7ff;paloffs=(ra_bc&0x1800)>>11;
        colbank=(rd_ram(ri+3)&0xf000)>>10;tile=rd_ram(ri+3)&0x3ff;r_over=0;l_over=0;
        rc=0x5ff-((-xoffset+x_offs)&0x7ff); le=rc-(ra_cl&0x3ff); re=rc+1+(ra_cr&0x3ff);
        if(ra_cl||ra_cr)lnd=1;
        begin=le+1;if(begin<0)begin=0; end=re;if(end>screen_width)end=screen_width;
        if(re<0){r_over=-re;re=0;} if(le>=screen_width){l_over=le-screen_width+1;le=screen_width-1;}
        bgonly=(rc>(screen_width-2+512))?1:0;
        color=((palette_offs+colbank+paloffs)<<4)+(type?1:4); pri=pr[2]<<12;
#ifndef AMIGA
        if(CC_GETENV("CC_ROADC")&&lnd&&y==200) fprintf(stderr,"ROADC pb=%d colbank=%d paloffs=%d ra_bc=%04x word3=%04x color=%d pal[c..c+2]=%04x %04x %04x\n",
            cc_road_palbank,colbank,paloffs,ra_bc,rd_ram(ri+3),color,cc_pal[color&0xfff],cc_pal[(color+1)&0xfff],cc_pal[(color+2)&0xfff]);
#endif
        x_index=(-xoffset+x_offs+begin)&0x7ff; roada=roada_line+screen_width-1-begin;
        if(lnd&&begin<end) for(i=begin;i<end;i++){
            if(tile){ pixel=road_pix[((((unsigned)tile<<8)+(x_index>>3))&0x3ffff)*8+(x_index&7)];
                if(pixel||!road_trans){if(type)pixel=(pixel-1)&3;*roada--=(color+pixel)|pri;} else *roada--=0xf000;
            } else roada--; x_index++;x_index&=0x7ff; }
        color=((palette_offs+colbank+palloffs)<<4)+(type?1:4); pri=pr[0]<<12;
        if(bgonly){ if(ra_cl&0x8000){roada=roada_line;for(i=0;i<screen_width;i++)*roada++=(color+(type?3:0));} }
        else if(le>=0&&le<screen_width){ x_index=(511-l_over)&0x7ff; roada=roada_line+screen_width-1-le;
            if(lnd) for(i=le;i>=0;i--){ pixel=road_pix[((((unsigned)tile<<8)+(x_index>>3))&0x3ffff)*8+(x_index&7)];
                pixpri=(pixel==0)?0:pri; if(pixel==0&&!(ra_cl&0x8000))roada++; else{if(type)pixel=(pixel-1)&3;*roada++=(color+pixel)|pixpri;} x_index--;x_index&=0x7ff; } }
        color=((palette_offs+colbank+palroffs)<<4)+(type?1:4); pri=pr[1]<<12;
        if(re<screen_width&&re>=0){ x_index=(512+r_over)&0x7ff; roada=roada_line+screen_width-1-re;
            if(lnd) for(i=re;i<screen_width;i++){ pixel=road_pix[((((unsigned)tile<<8)+(x_index>>3))&0x3ffff)*8+(x_index&7)];
                pixpri=(pixel==0)?0:pri; if(pixel==0&&!(ra_cr&0x8000))roada--; else{if(type)pixel=(pixel-1)&3;*roada--=(color+pixel)|pixpri;} x_index++;x_index&=0x7ff; } }
        /* ROAD B */
        palroffs=(rb_cr&0x1000)>>11;palloffs=(rb_cl&0x1000)>>11;xoffset=rb_bc&0x7ff;paloffs=(rb_bc&0x1800)>>11;
        colbank=(rd_ram(ri2+3)&0xf000)>>10;tile=rd_ram(ri2+3)&0x3ff;r_over=0;l_over=0;
        rc=0x5ff-((-xoffset+x_offs)&0x7ff); le=rc-(rb_cl&0x3ff); re=rc+1+(rb_cr&0x3ff);
        if((rb_cl||rb_cr)&&((road_ctrl&0x800)||(type==2))){dtr=1;lnd=1;} else dtr=0;
        begin=le+1;if(begin<0)begin=0; end=re;if(end>screen_width)end=screen_width;
        if(re<0){r_over=-re;re=0;} if(le>=screen_width){l_over=le-screen_width+1;le=screen_width-1;}
        bgonly=(rc>(screen_width-2+512))?1:0;
        color=((palette_offs+colbank+paloffs)<<4)+(type?1:4); pri=pr[5]<<12;
        x_index=(-xoffset+x_offs+begin)&0x7ff;
        if(x_index>0x3ff){ roadb=roadb_line+screen_width-1-begin;
            if(dtr&&tile&&begin<end) for(i=begin;i<end;i++){ pixel=road_pix[((((unsigned)tile<<8)+(x_index>>3))&0x3ffff)*8+(x_index&7)];
                if(pixel||!road_trans){if(type)pixel=(pixel-1)&3;*roadb--=(color+pixel)|pri;} else *roadb--=0xf000; x_index++;x_index&=0x7ff; } }
        color=((palette_offs+colbank+palloffs)<<4)+(type?1:4); pri=pr[3]<<12;
        if(bgonly){ if((rb_cl&0x8000)&&dtr){roadb=roadb_line;for(i=0;i<screen_width;i++)*roadb++=(color+(type?3:0));} }
        else if(le>=0&&le<screen_width){ x_index=(511-l_over)&0x7ff; roadb=roadb_line+screen_width-1-le;
            if(dtr) for(i=le;i>=0;i--){ pixel=road_pix[((((unsigned)tile<<8)+(x_index>>3))&0x3ffff)*8+(x_index&7)];
                pixpri=(pixel==0)?0:pri; if(pixel==0&&!(rb_cl&0x8000))roadb++; else{if(type)pixel=(pixel-1)&3;*roadb++=(color+pixel)|pixpri;} x_index--;if(x_index<0)break; } }
        color=((palette_offs+colbank+palroffs)<<4)+(type?1:4); pri=pr[4]<<12;
        if(re<screen_width&&re>=0){ x_index=(512+r_over)&0x7ff; roadb=roadb_line+screen_width-1-re;
            if(dtr) for(i=re;i<screen_width;i++){ pixel=road_pix[((((unsigned)tile<<8)+(x_index>>3))&0x3ffff)*8+(x_index&7)];
                pixpri=(pixel==0)?0:pri; if(pixel==0&&!(rb_cr&0x8000))roadb--; else{if(type)pixel=(pixel-1)&3;*roadb--=(color+pixel)|pixpri;} x_index++;if(x_index>0x3ff)break; } }
        if(lnd) for(i=0;i<screen_width;i++){ uint16_t va=roada_line[i],vb=roadb_line[i],o;
            if(va==0x8000)o=vb&0x8fff; else if(vb==0x8000)o=va&0x8fff; else if((vb&0x7000)>(va&0x7000))o=vb&0x8fff; else o=va&0x8fff;
            road_layer[y*screen_width+i]=o; }
    }
}

/* ---- sprites (ported from contcirc_draw_sprites_16x8) ---- */
static inline int spr_pix(unsigned code,int x,int y){
    /* tile16x8_layout planes = STEP4(0,16): byte pair p*2 is planeoffset[p].
     * MAME decode (drawgfx.cpp) assigns planeoffset[0] to the MSB (planebit starts at
     * 1<<(planes-1), shifts right), so byte pair p maps to bit (3-p), NOT bit p.
     * Validated pixel-faithful vs MAME on the CONTINENTAL CIRCUS title: with <<p the
     * planes were reversed -> red/gold rendered blue/black + 2 dropped letters. */
    unsigned base=(code*64u)&0x1fffff; int px=0;
    for(int p=0;p<4;p++){ uint8_t b=cc_gfx_spr[(base+y*8+p*2+(x>>3))&0x1fffff]; px|=((b>>(7-(x&7)))&1)<<(3-p); } return px;
}
static inline int smap_word(unsigned i){ i&=0x3ffff; return cc_gfx_smap[i*2]|(cc_gfx_smap[i*2+1]<<8); }

static void draw_sprites(uint16_t*img,int stride){
    const int W=CC_W,H=CC_H,y_offs=5,top_clip=16;
    static const uint8_t primasks[2] = { 0xf0, 0xfc };
    for(int offs=0; offs<0x700/2; offs+=4){
        #define SW(n) ((cc_spr[(offs+n)*2]<<8)|cc_spr[(offs+n)*2+1])
        int d0=SW(0),d1=SW(1),d2=SW(2),d3=SW(3);
        int zoomy=(d0&0xfe00)>>9, y=d0&0x1ff, tile=d1&0x7ff;
        int priority=(d2&0x8000)>>15, fx=(d2&0x4000)>>14, fy=(d2&0x2000)>>13, x=d2&0x1ff;
        int color=(d3&0xff00)>>8, zoomx=d3&0x7f;
        #undef SW
        if(!tile) continue;
        unsigned mo=tile<<7; zoomx+=1; zoomy+=1; y+=y_offs+(128-zoomy);
        if(x>0x140)x-=0x200; if(y>0x140)y-=0x200;
        uint8_t pmask = primasks[priority];
        for(int ch=0;ch<128;ch++){ int k=ch%8,j=ch/8; int spx=fx?(7-k):k, spy=fy?(15-j):j;
            int code=smap_word(mo+spx+(spy<<3)); if(code==0xffff)continue;
            int cx=x+((k*zoomx)/8), cy=y+((j*zoomy)/16);
            int zx=x+(((k+1)*zoomx)/8)-cx, zy=y+(((j+1)*zoomy)/16)-cy;
            if(zx<=0||zy<=0)continue;
            int dx0 = (cx < 0) ? -cx : 0;
            int dx1 = (cx + zx > W) ? (W - cx) : zx;
            if(dx0 >= dx1) continue;
            for(int dy=0;dy<zy;dy++){ int oy=cy+dy; if(oy<top_clip||oy>=H)continue; int sy=(dy*8)/zy; if(fy)sy=7-sy;
                uint16_t*o=img+oy*stride+cx+dx0;
                uint8_t *pr=pri_layer+oy*W+cx+dx0;
                for(int dx=dx0;dx<dx1;dx++){ int sx=(dx*16)/zx; if(fx)sx=15-sx;
                    int pix=spr_pix(code,sx,sy); if(!pix)continue;
                    if(((1u << (pr[dx-dx0] & 0x1f)) & pmask) == 0) o[dx-dx0]=color*16+pix;
                    pr[dx-dx0]=31;
                } }
        }
    }
}

void cc_render_frame(uint16_t *out, int stride){
    draw_road();
    memset(pri_layer, 0, sizeof pri_layer);
    int bgsx=-(int16_t)r16(cc_scnctl+0), bgsy=-(int16_t)r16(cc_scnctl+6);
    int fgsx=-(int16_t)r16(cc_scnctl+2), fgsy=-(int16_t)r16(cc_scnctl+8);
    int txsx=-(int16_t)r16(cc_scnctl+4), txsy=-(int16_t)r16(cc_scnctl+10);
    int ctrl6=r16(cc_scnctl+12);
    int dis=ctrl6&0xf7;            /* layer disable bits: 1=BG0 2=BG1 4=TX */
    int bottom=(ctrl6>>3)&1;       /* which BG is the bottom (opaque) layer */
    int bg0_on=!(dis&1), bg1_on=!(dis&2), tx_on=!(dis&4);
    const char*only=CC_GETENV("CC_ONLY");   /* debug: isolate a layer */
    if(only){ bg0_on=!strcmp(only,"bg0"); bg1_on=!strcmp(only,"bg1"); tx_on=!strcmp(only,"tx"); }
    for(int y=0;y<CC_H;y++){
        uint16_t*o=out+y*stride;
        /* BG1(FG) per-row X scroll (fgscroll_ram@0xc400) + per-row source Y; the
         * per-column Y warp (colscroll_ram@0xe000) is applied per pixel below.
         * Mirrors MAME tc0100scn tilemap_draw_fg -- this is the pseudo-3D ground. */
        /* tilemap built-in offset: TC0100SCN set_scrolldx(xd-16): BG=+16, TX=+23 (xd=0, contcirc) */
        int fg_rowbase = fgsx - (int16_t)r16(cc_scn+0xc400+(y&0x1ff)*2) + 16;
        int fg_srcy    = fgsy + y;
        for(int x=0;x<CC_W;x++){
            int pen=0;            /* backdrop (palette[0]) */
            int b0px=0,b0pen=0,b1px=0,b1pen=0;
            /* BG0 flat (no rowscroll); + convention + tilemap dx offset */
            { int wx=(x+bgsx+16)&511,wy=(y+bgsy)&511; const uint8_t*e=cc_scn+((wy>>3)*64+(wx>>3))*4;
              int attr=r16(e),code=r16(e+2),c=attr&0xff,fxx=(attr>>14)&1,fyy=(attr>>15)&1;
              b0px=bg_pix(code,fxx?7-(wx&7):(wx&7),fyy?7-(wy&7):(wy&7)); b0pen=c*16+b0px; }
            /* BG1(FG): rowscroll X + colscroll Y */
            { int sx=(fg_rowbase+x)&511; int col=(int16_t)r16(cc_scn+0xe000+((sx>>3)&0x7f)*2);
              int sy=(fg_srcy-col)&511; const uint8_t*e=cc_scn+0x8000+((sy>>3)*64+(sx>>3))*4;
              int attr=r16(e),code=r16(e+2),c=attr&0xff,fxx=(attr>>14)&1,fyy=(attr>>15)&1;
              b1px=bg_pix(code,fxx?7-(sx&7):(sx&7),fyy?7-(sy&7):(sy&7)); b1pen=c*16+b1px; }
            /* bottom layer OPAQUE (incl pen0); top layer transparent (pen0 skipped) */
            if(bottom==0){ if(bg0_on) pen=b0pen; if(bg1_on&&b1px) pen=b1pen; }
            else         { if(bg1_on) pen=b1pen; if(bg0_on&&b0px) pen=b0pen; }
            /* road */
            { uint16_t rv=road_layer[y*CC_W+x]; if(!(rv&0x8000)){ pen=rv&0xfff; pri_layer[y*CC_W+x]=(y > road_priority_switch_line) ? 2 : 1; } }
            /* TX transparent */
            if(tx_on){ int wx=(x+txsx+23)&511,wy=(y+txsy)&511; int a=r16(cc_scn+0x4000+((wy>>3)*64+(wx>>3))*2);
              int code=a&0xff,c=(a>>8)&0x3f,fxx=(a>>14)&1,fyy=(a>>15)&1;
              int px=tx_pix(code,fxx?7-(wx&7):(wx&7),fyy?7-(wy&7):(wy&7)); if(px){ pen=c*16+px; pri_layer[y*CC_W+x]=4; } }  /* TX shares BG 16-colour groups (granularity 16, not 4) */
            o[x]=pen;
        }
    }
    if(!CC_GETENV("CC_NOSPR")) draw_sprites(out,stride);
}
