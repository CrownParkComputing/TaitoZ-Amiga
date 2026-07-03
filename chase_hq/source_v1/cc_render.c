/* cc_render.c -- Continental Circus software renderer (Amiga build).
 * Port of steps/03-video-decode/render.c: TC0100SCN (BG0/BG1/TX) + TC0110PCR
 * palette + TC0150ROD road (MAME BSD-3 port) + zoomed sprites. Outputs palette
 * pens; the RTG/host presenter expands Chase H.Q.'s XBGR555 palette words. */
#include "cc_state.h"
#include <stdlib.h>
#include <stdio.h>
#include <string.h>

static uint16_t road_layer[CC_H*CC_W];
static uint8_t  pri_layer[CC_H*CC_W];
static int      road_priority_switch_line;

static inline int r16(const uint8_t*p){ return (p[0]<<8)|p[1]; }
#ifdef __amigaos__
#define CC_GETENV(n) 0
#else
#define CC_GETENV(n) getenv(n)
#endif
/* XBGR555 pen -> 3-3-2 byte */
static inline uint8_t pal332(int pen){
    uint16_t d=cc_pal[pen&0xfff];
    int r=d&0x1f, g=(d>>5)&0x1f, b=(d>>10)&0x1f;
    return (uint8_t)(((r>>2)<<5)|((g>>2)<<2)|(b>>3));
}
static inline int bg_pix(unsigned code,int x,int y){
    unsigned off=(code*32u + y*4u + (x>>1)) & 0x7ffff;
    uint8_t b=cc_gfx_scn[off]; return (x&1)?(b&0xf):(b>>4);
}
static inline int tx_pix(unsigned code,int x,int y){
    const uint8_t*c=cc_scn + 0x6000 + code*16u + y*2u;
    return (((c[0]>>(7-x))&1)<<1) | ((c[1]>>(7-x))&1);
}

/* ---- TC0150ROD road (ported from MAME tc0150rod.cpp, BSD-3 N.Salmoria) ---- */
static inline int rd_ram(int idx){ if(idx<0||idx>=0x1000)return 0; return r16(cc_road+idx*2); }

/* Road gfx TRANSCODE CACHE (Continental Circus pattern): decode all 256K road
 * words to 8 pen bytes ONCE, so the per-pixel path is a single byte load
 * instead of a 16-bit assemble + two shifts + masks per pixel. 2MB static. */
static uint8_t road_pixc[0x40000*8];
static int road_pixc_ready;
static void road_transcode_once(void){
    if(road_pixc_ready) return;
    for(unsigned w=0;w<0x40000;w++){
        unsigned off=w*2u;
        unsigned gw=(cc_gfx_road[off+1]<<8)|cc_gfx_road[off];
        uint8_t *p=road_pixc+w*8;
        for(unsigned b=0;b<8;b++)
            p[b]=(uint8_t)((((gw>>(15-b))&1)<<1)|((gw>>(7-b))&1));
    }
    road_pixc_ready=1;
}
#define road_pix(w,x) ((int)road_pixc[(((unsigned)(w))&0x3ffff)*8u + ((unsigned)(x)&7u)])

static void draw_road(void){
    const int screen_width=CC_W, H=CC_H, type=0, road_trans=0, y_offs=-1;
    road_transcode_once();
    const int palette_offs=0xc0, x_offs=0xa7;
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
            if(tile){ pixel=road_pix((((unsigned)tile<<8)+(x_index>>3)),x_index);
                if(pixel||!road_trans){if(type)pixel=(pixel-1)&3;*roada--=(color+pixel)|pri;} else *roada--=0xf000;
            } else roada--; x_index++;x_index&=0x7ff; }
        color=((palette_offs+colbank+palloffs)<<4)+(type?1:4); pri=pr[0]<<12;
        if(bgonly){ if(ra_cl&0x8000){roada=roada_line;for(i=0;i<screen_width;i++)*roada++=(color+(type?3:0));} }
        else if(le>=0&&le<screen_width){ x_index=(511-l_over)&0x7ff; roada=roada_line+screen_width-1-le;
            if(lnd) for(i=le;i>=0;i--){ pixel=road_pix((((unsigned)tile<<8)+(x_index>>3)),x_index);
                pixpri=(pixel==0)?0:pri; if(pixel==0&&!(ra_cl&0x8000))roada++; else{if(type)pixel=(pixel-1)&3;*roada++=(color+pixel)|pixpri;} x_index--;x_index&=0x7ff; } }
        color=((palette_offs+colbank+palroffs)<<4)+(type?1:4); pri=pr[1]<<12;
        if(re<screen_width&&re>=0){ x_index=(512+r_over)&0x7ff; roada=roada_line+screen_width-1-re;
            if(lnd) for(i=re;i<screen_width;i++){ pixel=road_pix((((unsigned)tile<<8)+(x_index>>3)),x_index);
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
            if(dtr&&tile&&begin<end) for(i=begin;i<end;i++){ pixel=road_pix((((unsigned)tile<<8)+(x_index>>3)),x_index);
                if(pixel||!road_trans){if(type)pixel=(pixel-1)&3;*roadb--=(color+pixel)|pri;} else *roadb--=0xf000; x_index++;x_index&=0x7ff; } }
        color=((palette_offs+colbank+palloffs)<<4)+(type?1:4); pri=pr[3]<<12;
        if(bgonly){ if((rb_cl&0x8000)&&dtr){roadb=roadb_line;for(i=0;i<screen_width;i++)*roadb++=(color+(type?3:0));} }
        else if(le>=0&&le<screen_width){ x_index=(511-l_over)&0x7ff; roadb=roadb_line+screen_width-1-le;
            if(dtr) for(i=le;i>=0;i--){ pixel=road_pix((((unsigned)tile<<8)+(x_index>>3)),x_index);
                pixpri=(pixel==0)?0:pri; if(pixel==0&&!(rb_cl&0x8000))roadb++; else{if(type)pixel=(pixel-1)&3;*roadb++=(color+pixel)|pixpri;} x_index--;if(x_index<0)break; } }
        color=((palette_offs+colbank+palroffs)<<4)+(type?1:4); pri=pr[4]<<12;
        if(re<screen_width&&re>=0){ x_index=(512+r_over)&0x7ff; roadb=roadb_line+screen_width-1-re;
            if(dtr) for(i=re;i<screen_width;i++){ pixel=road_pix((((unsigned)tile<<8)+(x_index>>3)),x_index);
                pixpri=(pixel==0)?0:pri; if(pixel==0&&!(rb_cr&0x8000))roadb--; else{if(type)pixel=(pixel-1)&3;*roadb--=(color+pixel)|pixpri;} x_index++;if(x_index>0x3ff)break; } }
        /* keep the road's per-pixel PRIORITY (bits 12-14): sprites test against
         * it exactly like MAME pdrawgfx, which is what layers the tall grey
         * matte sprites BEHIND the road instead of flattening it to grey. */
        if(lnd) for(i=0;i<screen_width;i++){ uint16_t va=roada_line[i],vb=roadb_line[i],o;
            if(va==0x8000)o=vb; else if(vb==0x8000)o=va; else if((vb&0x7000)>(va&0x7000))o=vb; else o=va;
            road_layer[y*screen_width+i]=o; }
    }
}

/* ---- sprites (ported from chasehq_draw_sprites_16x16) ---- */
/* Decode one full 16-pen source row of a 16x16 4-plane sprite tile. Replaces the
 * old per-pixel spr_pix16 (4 plane reads + shifts PER PIXEL); the row is decoded
 * once per chunk row and cached across repeated rows when zy>16. Byte layout is
 * identical to spr_pix16: 8 bytes at code*128+sy*8, planes interleaved p*2+(x>>3). */
static inline void spr_row_decode(const uint8_t *gfx,unsigned code,int sy,uint8_t row[16]){
    unsigned base=(code*128u + (unsigned)sy*8u)&0x1fffff;
    const uint8_t *g=gfx+base;
    for(int half=0;half<2;half++){
        uint8_t b0=g[0+half],b1=g[2+half],b2=g[4+half],b3=g[6+half];
        uint8_t *r=row+half*8;
        for(int k=0;k<8;k++){ int sh=7-k;
            r[k]=(uint8_t)((((b0>>sh)&1)<<3)|(((b1>>sh)&1)<<2)|(((b2>>sh)&1)<<1)|((b3>>sh)&1)); }
    }
}

/* Sprite pen DECODE CACHE (St Dragon pattern, lazy): each of the 16384 16x16
 * tiles per gfx bank decodes once into 256 pen bytes; the draw loop then reads
 * rows straight from the cache.  4MB per bank (both banks 8MB) -- allocated at
 * first use via cc_big_alloc; on failure the per-row decode path still works.
 * code&0x3fff is exact: code*128 wraps the 2MB bank at 16384 (matches the
 * &0x1fffff masking in spr_row_decode). */
static uint8_t *spr_cache[2];
static uint8_t  spr_cache_valid[2][16384/8];
static int      spr_cache_tried[2];
static const uint8_t *spr_cache_row(const uint8_t *gfx,unsigned code,int sy){
    int bank = (gfx==cc_gfx_spr2) ? 1 : 0;
    uint8_t *cache = spr_cache[bank];
    if(!cache){
        if(spr_cache_tried[bank]) return 0;
        spr_cache_tried[bank]=1;
        cache = spr_cache[bank] = (uint8_t*)cc_big_alloc(16384u*256u);
        if(!cache) return 0;
    }
    code &= 0x3fff;
    if(!(spr_cache_valid[bank][code>>3] & (1u<<(code&7)))){
        uint8_t *dst = cache + code*256u;
        for(int r=0;r<16;r++) spr_row_decode(gfx,code,r,dst+r*16);
        spr_cache_valid[bank][code>>3] |= (uint8_t)(1u<<(code&7));
    }
    return cache + code*256u + (unsigned)sy*16u;
}
static inline int smap_word(unsigned i){ i&=0x3ffff; return cc_gfx_smap[i*2]|(cc_gfx_smap[i*2+1]<<8); }

static void draw_sprite_chunk(uint16_t *img,int stride,const uint8_t *gfx,int code,int color,
                              int flipx,int flipy,int curx,int cury,int zx,int zy,uint8_t pmask)
{
    const int W=CC_W,H=CC_H;
    if(zx<=0||zy<=0) return;
    int dx0 = (curx < 0) ? -curx : 0;
    int dx1 = (curx + zx > W) ? (W - curx) : zx;
    if(dx0 >= dx1) return;
    int dy0 = (cury < 0) ? -cury : 0;
    int dy1 = (cury + zy > H) ? (H - cury) : zy;
    if(dy0 >= dy1) return;
    int sy_step = (16 << 16) / zy;
    int sx_step = (16 << 16) / zx;
    int sy_acc = dy0 * sy_step;
    uint8_t rowbuf[16]; const uint8_t *row=rowbuf; int last_sy=-1;
    for(int dy=dy0;dy<dy1;dy++,sy_acc+=sy_step){
        int oy=cury+dy;
        int sy=sy_acc>>16; if(sy>15)sy=15; if(flipy)sy=15-sy;
        if(sy!=last_sy){
            row = spr_cache_row(gfx,code,sy);
            if(!row){ spr_row_decode(gfx,code,sy,rowbuf); row=rowbuf; }
            last_sy=sy;
        }
        uint16_t*o=img+oy*stride+curx+dx0;
        uint8_t *pr=pri_layer+oy*W+curx+dx0;
        int sx_acc = dx0 * sx_step;
        for(int dx=dx0;dx<dx1;dx++,sx_acc+=sx_step){
            int sx=sx_acc>>16; if(sx>15)sx=15; if(flipx)sx=15-sx;
            int pix=row[sx]; if(!pix)continue;   /* row = cache or rowbuf */
            int pidx=dx-dx0;
            uint8_t pri=pr[pidx];
            if(((1u << (pri & 0x1f)) & pmask) == 0) o[pidx]=color*16+pix;
            pr[pidx]=31;
        }
    }
}

static void draw_sprites(uint16_t*img,int stride){
    const int y_offs=7;
    static const uint8_t primasks[2] = { 0xf0, 0xfc };
    for(int offs=0; offs<=(0x800/2 - 4); offs+=4){
        #define SW(n) ((cc_spr[(offs+n)*2]<<8)|cc_spr[(offs+n)*2+1])
        int d0=SW(0),d1=SW(1),d2=SW(2),d3=SW(3);
        int zoomy=(d0&0xfe00)>>9, y=d0&0x1ff;
        int priority=(d1&0x8000)>>15, color=(d1&0x7f80)>>7, zoomx=d1&0x7f;
        int fy=(d2&0x8000)>>15, fx=(d2&0x4000)>>14, x=d2&0x1ff;
        int tile=d3&0x7ff;
        #undef SW
        if(!tile) continue;
        zoomx+=1; zoomy+=1; y+=y_offs+(128-zoomy);
        if(zoomy>=120 && y>=170) y-=28;
        if(x>0x140)x-=0x200; if(y>0x140)y-=0x200;
        uint8_t pmask = primasks[priority];
        if((zoomx - 1) & 0x40){
            unsigned mo=tile<<6;
            for(int ch=0;ch<64;ch++){ int k=ch%8,j=ch/8; int spx=fx?(7-k):k, spy=fy?(7-j):j;
                int code=smap_word(mo+spx+(spy<<3)); if(code==0xffff)continue;
                int cx=x+((k*zoomx)/8), cy=y+((j*zoomy)/8);
                int zx=x+(((k+1)*zoomx)/8)-cx, zy=y+(((j+1)*zoomy)/8)-cy;
                draw_sprite_chunk(img,stride,cc_gfx_spr,code,color,fx,fy,cx,cy,zx,zy,pmask);
            }
        } else if((zoomx - 1) & 0x20){
            unsigned mo=(tile<<5)+0x20000;
            for(int ch=0;ch<32;ch++){ int k=ch%4,j=ch/4; int spx=fx?(3-k):k, spy=fy?(7-j):j;
                int code=smap_word(mo+spx+(spy<<2)); if(code==0xffff)continue;
                int cx=x+((k*zoomx)/4), cy=y+((j*zoomy)/8);
                int zx=x+(((k+1)*zoomx)/4)-cx, zy=y+(((j+1)*zoomy)/8)-cy;
                draw_sprite_chunk(img,stride,cc_gfx_spr2,code,color,fx,fy,cx,cy,zx,zy,pmask);
            }
        } else {
            unsigned mo=(tile<<4)+0x30000;
            for(int ch=0;ch<16;ch++){ int k=ch%2,j=ch/2; int spx=fx?(1-k):k, spy=fy?(7-j):j;
                int code=smap_word(mo+spx+(spy<<1)); if(code==0xffff)continue;
                int cx=x+((k*zoomx)/2), cy=y+((j*zoomy)/8);
                int zx=x+(((k+1)*zoomx)/2)-cx, zy=y+(((j+1)*zoomy)/8)-cy;
                draw_sprite_chunk(img,stride,cc_gfx_spr2,code,color,fx,fy,cx,cy,zx,zy,pmask);
            }
        }
    }
}

/* ---- tilemap row renderers: 8-pixel TILE SPANS instead of per-pixel ----------
 * The old per-pixel loop refetched the tile entry and re-decoded the gfx byte for
 * EVERY pixel of EVERY layer (70% of frame time profiled).  Within one 8-pixel
 * span the tile entry, colour, flips and source row are constant, so each span
 * fetches the entry once and decodes the 8 pens in one go.  Pens keep the
 * low-nibble == pixel-value property (c*16+px), so transparency is (pen&15). */

/* Emit one decoded BG tile span.  The full-tile aligned case (k=0,n=8, the vast
 * majority of spans) writes the pens straight from the gfx bytes -- no px8
 * temp buffer round-trip.  Partial spans fall back to the decode buffer. */
static inline void bg_span_emit(uint16_t *o,const uint8_t *g,unsigned cbase,
                                int fxx,int rowy,int k,int n,int opaque){
    (void)rowy;
    if(k==0 && n==8 && !fxx){
        uint8_t b;
        if(opaque){
            b=g[0]; o[0]=(uint16_t)(cbase+(b>>4)); o[1]=(uint16_t)(cbase+(b&0xf));
            b=g[1]; o[2]=(uint16_t)(cbase+(b>>4)); o[3]=(uint16_t)(cbase+(b&0xf));
            b=g[2]; o[4]=(uint16_t)(cbase+(b>>4)); o[5]=(uint16_t)(cbase+(b&0xf));
            b=g[3]; o[6]=(uint16_t)(cbase+(b>>4)); o[7]=(uint16_t)(cbase+(b&0xf));
        } else {
            unsigned p;
            b=g[0]; p=b>>4;  if(p)o[0]=(uint16_t)(cbase+p); p=b&0xf; if(p)o[1]=(uint16_t)(cbase+p);
            b=g[1]; p=b>>4;  if(p)o[2]=(uint16_t)(cbase+p); p=b&0xf; if(p)o[3]=(uint16_t)(cbase+p);
            b=g[2]; p=b>>4;  if(p)o[4]=(uint16_t)(cbase+p); p=b&0xf; if(p)o[5]=(uint16_t)(cbase+p);
            b=g[3]; p=b>>4;  if(p)o[6]=(uint16_t)(cbase+p); p=b&0xf; if(p)o[7]=(uint16_t)(cbase+p);
        }
        return;
    }
    {
        uint8_t px8[8];
        if(!fxx){
            px8[0]=g[0]>>4; px8[1]=g[0]&0xf; px8[2]=g[1]>>4; px8[3]=g[1]&0xf;
            px8[4]=g[2]>>4; px8[5]=g[2]&0xf; px8[6]=g[3]>>4; px8[7]=g[3]&0xf;
        } else {
            px8[7]=g[0]>>4; px8[6]=g[0]&0xf; px8[5]=g[1]>>4; px8[4]=g[1]&0xf;
            px8[3]=g[2]>>4; px8[2]=g[2]&0xf; px8[1]=g[3]>>4; px8[0]=g[3]&0xf;
        }
        if(opaque) for(int i=0;i<n;i++) o[i]=(uint16_t)(cbase+px8[k+i]);
        else       for(int i=0;i<n;i++){ if(px8[k+i]) o[i]=(uint16_t)(cbase+px8[k+i]); }
    }
}

/* BG0: flat scroll.  opaque=1 writes every pen (bottom layer, incl pen0);
 * opaque=0 overlays only non-zero pixels (top layer). */
static void bg0_row(uint16_t *o,int y,int bgsx,int bgsy,int opaque){
    int wy=(y+bgsy)&511, wxs=(bgsx+16)&511, wyf=wy&7;
    const uint8_t *rowbase=cc_scn+((wy>>3)*64)*4;
    int x=0;
    while(x<CC_W){
        int wx=(x+wxs)&511;
        const uint8_t *e=rowbase+(wx>>3)*4;
        int attr=r16(e),code=r16(e+2);
        unsigned cbase=(unsigned)(attr&0xff)<<4;
        int fxx=(attr>>14)&1, rowy=(attr&0x8000)?7-wyf:wyf;
        const uint8_t *g=cc_gfx_scn+((code*32u+(unsigned)rowy*4u)&0x7ffff);
        int k=wx&7, n=8-k; if(n>CC_W-x)n=CC_W-x;
        bg_span_emit(o+x,g,cbase,fxx,rowy,k,n,opaque);
        x+=n;
    }
}

/* BG1(FG): per-row X scroll (fgscroll_ram@0xc400) + per-COLUMN Y warp
 * (colscroll_ram@0xe000).  The column index sx>>3 is constant within a span, so
 * the Y warp -- and with it the tile entry -- is span-constant too.  Mirrors
 * MAME tc0100scn tilemap_draw_fg (the pseudo-3D ground). */
static void bg1_row(uint16_t *o,int fg_rowbase,int fg_srcy,int opaque){
    int sxs=fg_rowbase&511;
    int x=0;
    while(x<CC_W){
        int sx=(x+sxs)&511;
        int col=(int16_t)r16(cc_scn+0xe000+((sx>>3)&0x7f)*2);
        int sy=(fg_srcy-col)&511;
        const uint8_t *e=cc_scn+0x8000+((sy>>3)*64+(sx>>3))*4;
        int attr=r16(e),code=r16(e+2);
        unsigned cbase=(unsigned)(attr&0xff)<<4;
        int fxx=(attr>>14)&1, rowy=(attr&0x8000)?7-(sy&7):(sy&7);
        const uint8_t *g=cc_gfx_scn+((code*32u+(unsigned)rowy*4u)&0x7ffff);
        int k=sx&7, n=8-k; if(n>CC_W-x)n=CC_W-x;
        bg_span_emit(o+x,g,cbase,fxx,rowy,k,n,opaque);
        x+=n;
    }
}

/* TX 2bpp text overlay; TX shares BG 16-colour groups (granularity 16, not 4).
 * plane 0 = high bit (MAME decoded-gfx convention, matches tx_pix). */
static void tx_row(uint16_t *o,uint8_t *pl,int y,int txsx,int txsy){
    int wy=(y+txsy)&511, wxs=(txsx+23)&511, wyf=wy&7;
    const uint8_t *rowbase=cc_scn+0x4000+((wy>>3)*64)*2;
    int x=0;
    while(x<CC_W){
        int wx=(x+wxs)&511;
        int a=r16(rowbase+(wx>>3)*2);
        int code=a&0xff;
        unsigned cbase=(unsigned)((a>>8)&0x3f)<<4;
        int rowy=(a&0x8000)?7-wyf:wyf, fxx=(a>>14)&1;
        const uint8_t *cg=cc_scn+0x6000+code*16u+rowy*2;
        uint8_t c0=cg[0],c1=cg[1];
        uint8_t px8[8];
        for(int i=0;i<8;i++){ int sxp=fxx?7-i:i;
            px8[i]=(uint8_t)((((c0>>(7-sxp))&1)<<1)|((c1>>(7-sxp))&1)); }
        int k=wx&7, n=8-k; if(n>CC_W-x)n=CC_W-x;
        for(int i=0;i<n;i++){ if(px8[k+i]){ o[x+i]=(uint16_t)(cbase+px8[k+i]); pl[x+i]=4; } }
        x+=n;
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
        uint8_t *pl=pri_layer+y*CC_W;
        /* tilemap built-in offset: TC0100SCN set_scrolldx(xd-16): BG=+16, TX=+23 (xd=0, contcirc) */
        int fg_rowbase = fgsx - (int16_t)r16(cc_scn+0xc400+(y&0x1ff)*2) + 16;
        int fg_srcy    = fgsy + y;
        /* bottom layer OPAQUE (incl pen0); top layer transparent (pen0 skipped) */
        if(bottom==0){
            if(bg0_on) bg0_row(o,y,bgsx,bgsy,1); else memset(o,0,CC_W*sizeof(uint16_t));
            if(bg1_on) bg1_row(o,fg_rowbase,fg_srcy,0);
        } else {
            if(bg1_on) bg1_row(o,fg_rowbase,fg_srcy,1); else memset(o,0,CC_W*sizeof(uint16_t));
            if(bg0_on) bg0_row(o,y,bgsx,bgsy,0);
        }
        /* road overlay: pen + the road's own priority level (MAME semantics) */
        { const uint16_t *rl=road_layer+y*CC_W;
          for(int x=0;x<CC_W;x++){ uint16_t rv=rl[x]; if(!(rv&0x8000)){ o[x]=rv&0xfff; pl[x]=(uint8_t)((rv>>12)&7); } } }
        /* TX transparent overlay */
        if(tx_on) tx_row(o,pl,y,txsx,txsy);
    }
    if(!CC_GETENV("CC_NOSPR")) draw_sprites(out,stride);
}
