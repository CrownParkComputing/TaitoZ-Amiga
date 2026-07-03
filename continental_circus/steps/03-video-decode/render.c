/* Continental Circus — step 03 video decode (TC0100SCN tilemaps + TC0110PCR palette).
 *
 * Decodes the attract-frame VRAM captured by step 02 (../02-host-ref/out/state)
 * using the BG tile ROM (../01-rom-analysis/out/gfx_scn.bin) and renders:
 *   bg0.ppm, bg1.ppm        full 512x512 tilemaps (no scroll) — decode proof
 *   tx.ppm                  full 512x512 text layer (char-gen RAM)
 *   frame.ppm               320x224 composite (BG0 opaque, BG1, TX), with scroll
 *
 * Reference: MAME taito/tc0100scn.cpp, tc0110pcr.cpp, taito_z_v.cpp.
 *   BG tile: attr=ram[2i+Off], code=ram[2i+1+Off], color=attr&0xff, flip=attr>>14
 *   TX tile: a=ram[0x2000+i], code=a&0xff, color=(a>>8)&0x3f, flip=a>>14
 *   gfx ROM: gfx_8x8x4_packed_msb (32 B/tile, hi nibble=left pixel)
 *   char-gen: 2bpp, 16 B/char at ram byte 0x6000
 *   palette: raw xRGB555 word -> R=d>>10 G=d>>5 B=d>>0 (pal5bit)
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>

static uint8_t  gfx[0x80000];          /* BG tile ROM (gfx_scn.bin) */
static int      gfx_sz;
static uint8_t  scn[0x14000];          /* TC0100SCN RAM */
static uint16_t pal[0x1000];           /* PCR palette (raw xRGB555) */
static uint16_t ctrl[8];               /* TC0100SCN ctrl (scroll) */
static uint8_t  rgfx[0x80000];         /* TC0150ROD road gfx ROM (gfx_road.bin) */
static uint16_t rram[0x1000];          /* TC0150ROD road RAM (words) */
static int      road_palbank = 3;      /* contcirc_out_w bits 6-7 */
static uint16_t road_layer[224*320];   /* 0x8000 = transparent, else pen */
static uint8_t  sgfx[0x200000];        /* assembled sprite gfx (sprites_asm.bin) */
static uint8_t  smap[0x80000];         /* spritemap ROM (spritemap_asm.bin, LE words) */
static uint8_t  spram[0x700];          /* spriteram capture */

static inline uint16_t r16(const uint8_t*p){ return (p[0]<<8)|p[1]; }  /* big-endian */
static inline int pal5(int v){ v&=0x1f; return (v<<3)|(v>>2); }

static void rgb_of(uint16_t d, int*r,int*g,int*b){
    *r=pal5(d>>10); *g=pal5(d>>5); *b=pal5(d>>0);
}
/* 4bpp BG ROM pixel (0..15) */
static int bg_pix(unsigned code, int x, int y){
    unsigned off = (code*32u + y*4u + (x>>1)) & (sizeof(gfx)-1);
    uint8_t byte = gfx[off];
    return (x&1) ? (byte&0xf) : (byte>>4);
}
/* 2bpp TX char pixel (0..3) from char-gen RAM @ byte 0x6000 */
static int tx_pix(unsigned code, int x, int y){
    const uint8_t*c = scn + 0x6000 + code*16u + y*2u;
    return ((c[0]>>(7-x))&1) | (((c[1]>>(7-x))&1)<<1);
}

static int load(const char*path, void*dst, int max){
    FILE*f=fopen(path,"rb"); if(!f){perror(path);return -1;}
    int n=fread(dst,1,max,f); fclose(f); return n;
}
static void ppm(const char*path,const uint8_t*rgb,int w,int h){
    FILE*f=fopen(path,"wb"); fprintf(f,"P6\n%d %d\n255\n",w,h); fwrite(rgb,1,w*h*3,f); fclose(f);
    printf("  wrote %s (%dx%d)\n",path,w,h);
}

/* render a full 64x64 BG layer (word offset off_w in scn) into 512x512 */
static void render_bg(const char*path, unsigned off_w){
    static uint8_t img[512*512*3];
    for(int ty=0;ty<64;ty++) for(int tx=0;tx<64;tx++){
        unsigned i = ty*64+tx;
        const uint8_t*e = scn + (off_w + 2*i)*2;
        uint16_t attr=r16(e), code=r16(e+2);
        int color=attr&0xff, fx=(attr>>14)&1, fy=(attr>>15)&1;
        for(int y=0;y<8;y++) for(int x=0;x<8;x++){
            int px=bg_pix(code, fx?7-x:x, fy?7-y:y);
            int r,g,b; rgb_of(pal[(color*16+px)&0xfff],&r,&g,&b);
            uint8_t*o=img+((ty*8+y)*512+(tx*8+x))*3; o[0]=r;o[1]=g;o[2]=b;
        }
    }
    ppm(path,img,512,512);
}
static void render_tx(const char*path){
    static uint8_t img[512*512*3];
    for(int ty=0;ty<64;ty++) for(int tx=0;tx<64;tx++){
        unsigned i=ty*64+tx;
        uint16_t a=r16(scn + (0x2000 + i)*2);
        int code=a&0xff, color=(a>>8)&0x3f, fx=(a>>14)&1, fy=(a>>15)&1;
        for(int y=0;y<8;y++) for(int x=0;x<8;x++){
            int px=tx_pix(code, fx?7-x:x, fy?7-y:y);
            int r,g,b; rgb_of(pal[(color*4+px)&0xfff],&r,&g,&b);
            uint8_t*o=img+((ty*8+y)*512+(tx*8+x))*3; o[0]=r;o[1]=g;o[2]=b;
        }
    }
    ppm(path,img,512,512);
}

/* sample a composited visible pixel; returns 1 if opaque */
static int bg_sample(unsigned off_w,int sx,int sy,int*r,int*g,int*b,int opaque){
    int wx=sx&511, wy=sy&511, tx=wx>>3, ty=wy>>3;
    unsigned i=ty*64+tx; const uint8_t*e=scn+(off_w+2*i)*2;
    uint16_t attr=r16(e),code=r16(e+2); int color=attr&0xff,fx=(attr>>14)&1,fy=(attr>>15)&1;
    int x=wx&7,y=wy&7; int px=bg_pix(code,fx?7-x:x,fy?7-y:y);
    if(!opaque && px==0) return 0;
    rgb_of(pal[(color*16+px)&0xfff],r,g,b); return 1;
}
static int tx_sample(int sx,int sy,int*r,int*g,int*b){
    int wx=sx&511, wy=sy&511, tx=wx>>3, ty=wy>>3;
    uint16_t a=r16(scn+(0x2000+ty*64+tx)*2); int code=a&0xff,color=(a>>8)&0x3f,fx=(a>>14)&1,fy=(a>>15)&1;
    int x=wx&7,y=wy&7; int px=tx_pix(code,fx?7-x:x,fy?7-y:y);
    if(px==0) return 0; rgb_of(pal[(color*4+px)&0xfff],r,g,b); return 1;
}

/* ---- TC0150ROD road, ported from MAME taito/tc0150rod.cpp (BSD-3-Clause,
 *      copyright Nicola Salmoria). Adapted to write a 320x224 road layer
 *      (0x8000 = transparent) instead of MAME's bitmap + priority bitmap.
 *      contcirc params: type=1, y_offs=-3, palette_offs=road_palbank<<6, road_trans=0. */
static inline int rgfx_word(unsigned w){ w &= (sizeof(rgfx)/2 - 1); return (rgfx[w*2]<<8)|rgfx[w*2+1]; }
static inline int rd_ram(int idx){ return (idx<0||idx>=0x1000)?0:rram[idx]; }

static void draw_road(void){
    const int screen_width = 320, H = 224;
    const int type = 1, road_trans = 0, y_offs = -3;
    const int palette_offs = road_palbank << 6;
    const int x_offs = 0xa7;
    uint16_t scanline[512], roada_line[512], roadb_line[512];
    uint16_t pixel, color, gfx_word, pri, pixpri;
    uint16_t roada_clipl,roada_clipr,roada_bodyctrl, roadb_clipl,roadb_clipr,roadb_bodyctrl;
    uint8_t priorities[6];
    int x_index, roadram_index, roadram2_index, i;
    int xoffset, paloffs, palloffs, palroffs, road_gfx_tilenum, colbank, road_center;
    int road_ctrl = rd_ram(0xfff);
    int left_edge,right_edge,begin,end,right_over,left_over;
    int line_needs_drawing, draw_top_road_line, background_only;
    int road_A_address = y_offs*4 + ((road_ctrl & 0x0300) << 2);
    int road_B_address = y_offs*4 + ((road_ctrl & 0x0c00) << 0);
    uint16_t *roada,*roadb,*dst16;

    for(i=0;i<screen_width*H;i++) road_layer[i]=0x8000;

    for(int y=0;y<H;y++){
        line_needs_drawing=0;
        roadram_index  = road_A_address + y*4;
        roadram2_index = road_B_address + y*4;
        roada=roada_line; roadb=roadb_line;
        for(i=0;i<screen_width;i++){ *roada++=0x8000; *roadb++=0x8000; }
        priorities[0]=1;priorities[1]=1;priorities[2]=2;priorities[3]=3;priorities[4]=3;priorities[5]=4;
        roada_clipr=rd_ram(roadram_index);   roada_clipl=rd_ram(roadram_index+1);   roada_bodyctrl=rd_ram(roadram_index+2);
        roadb_clipr=rd_ram(roadram2_index);  roadb_clipl=rd_ram(roadram2_index+1);  roadb_bodyctrl=rd_ram(roadram2_index+2);
        if(roada_bodyctrl&0x2000)priorities[2]+=2;
        if(roadb_bodyctrl&0x2000)priorities[2]+=1;
        if(roada_clipl&0x2000)priorities[3]-=1;
        if(roadb_clipl&0x2000)priorities[3]-=2;
        if(roada_clipr&0x2000)priorities[4]-=1;
        if(roadb_clipr&0x2000)priorities[4]-=2;
        if(priorities[4]==0)priorities[4]++;

        /* ---- ROAD A ---- */
        palroffs=(roada_clipr&0x1000)>>11; palloffs=(roada_clipl&0x1000)>>11;
        xoffset=roada_bodyctrl&0x7ff; paloffs=(roada_bodyctrl&0x1800)>>11;
        colbank=(rd_ram(roadram_index+3)&0xf000)>>10; road_gfx_tilenum=rd_ram(roadram_index+3)&0x3ff;
        right_over=0;left_over=0;
        road_center=0x5ff-((-xoffset+x_offs)&0x7ff);
        left_edge=road_center-(roada_clipl&0x3ff);
        right_edge=road_center+1+(roada_clipr&0x3ff);
        if(roada_clipl||roada_clipr) line_needs_drawing=1;
        begin=left_edge+1; if(begin<0)begin=0;
        end=right_edge; if(end>screen_width)end=screen_width;
        if(right_edge<0){right_over=-right_edge;right_edge=0;}
        if(left_edge>=screen_width){left_over=left_edge-screen_width+1;left_edge=screen_width-1;}
        background_only=(road_center>(screen_width-2+1024/2))?1:0;
        /* body */
        color=((palette_offs+colbank+paloffs)<<4)+(type?1:4); pri=priorities[2]<<12;
        x_index=(-xoffset+x_offs+begin)&0x7ff;
        roada=roada_line+screen_width-1-begin;
        if(line_needs_drawing&&begin<end){
            for(i=begin;i<end;i++){
                if(road_gfx_tilenum){
                    gfx_word=rgfx_word((road_gfx_tilenum<<8)+(x_index>>3));
                    pixel=((gfx_word>>(7-(x_index%8)+8))&1)*2+((gfx_word>>(7-(x_index%8)))&1);
                    if(pixel||!road_trans){ if(type)pixel=(pixel-1)&3; *roada--=(color+pixel)|pri; }
                    else *roada--=0xf000;
                } else roada--;
                x_index++; x_index&=0x7ff;
            }
        }
        /* left edge */
        color=((palette_offs+colbank+palloffs)<<4)+(type?1:4); pri=priorities[0]<<12;
        if(background_only){
            if(roada_clipl&0x8000){ roada=roada_line; for(i=0;i<screen_width;i++)*roada++=(color+(type?3:0)); }
        } else if(left_edge>=0&&left_edge<screen_width){
            x_index=(1024/2-1-left_over)&0x7ff; roada=roada_line+screen_width-1-left_edge;
            if(line_needs_drawing){
                for(i=left_edge;i>=0;i--){
                    gfx_word=rgfx_word((road_gfx_tilenum<<8)+(x_index>>3));
                    pixel=((gfx_word>>(7-(x_index%8)+8))&1)*2+((gfx_word>>(7-(x_index%8)))&1);
                    pixpri=(pixel==0)?0:pri;
                    if(pixel==0&&!(roada_clipl&0x8000)) roada++;
                    else { if(type)pixel=(pixel-1)&3; *roada++=(color+pixel)|pixpri; }
                    x_index--; x_index&=0x7ff;
                }
            }
        }
        /* right edge */
        color=((palette_offs+colbank+palroffs)<<4)+(type?1:4); pri=priorities[1]<<12;
        if(right_edge<screen_width&&right_edge>=0){
            x_index=(1024/2+right_over)&0x7ff; roada=roada_line+screen_width-1-right_edge;
            if(line_needs_drawing){
                for(i=right_edge;i<screen_width;i++){
                    gfx_word=rgfx_word((road_gfx_tilenum<<8)+(x_index>>3));
                    pixel=((gfx_word>>(7-(x_index%8)+8))&1)*2+((gfx_word>>(7-(x_index%8)))&1);
                    pixpri=(pixel==0)?0:pri;
                    if(pixel==0&&!(roada_clipr&0x8000)) roada--;
                    else { if(type)pixel=(pixel-1)&3; *roada--=(color+pixel)|pixpri; }
                    x_index++; x_index&=0x7ff;
                }
            }
        }

        /* ---- ROAD B ---- */
        palroffs=(roadb_clipr&0x1000)>>11; palloffs=(roadb_clipl&0x1000)>>11;
        xoffset=roadb_bodyctrl&0x7ff; paloffs=(roadb_bodyctrl&0x1800)>>11;
        colbank=(rd_ram(roadram2_index+3)&0xf000)>>10; road_gfx_tilenum=rd_ram(roadram2_index+3)&0x3ff;
        right_over=0;left_over=0;
        road_center=0x5ff-((-xoffset+x_offs)&0x7ff);
        left_edge=road_center-(roadb_clipl&0x3ff);
        right_edge=road_center+1+(roadb_clipr&0x3ff);
        if((roadb_clipl||roadb_clipr)&&((road_ctrl&0x800)||(type==2))){ draw_top_road_line=1; line_needs_drawing=1; }
        else draw_top_road_line=0;
        begin=left_edge+1; if(begin<0)begin=0;
        end=right_edge; if(end>screen_width)end=screen_width;
        if(right_edge<0){right_over=-right_edge;right_edge=0;}
        if(left_edge>=screen_width){left_over=left_edge-screen_width+1;left_edge=screen_width-1;}
        background_only=(road_center>(screen_width-2+1024/2))?1:0;
        /* body */
        color=((palette_offs+colbank+paloffs)<<4)+(type?1:4); pri=priorities[5]<<12;
        x_index=(-xoffset+x_offs+begin)&0x7ff;
        if(x_index>0x3ff){
            roadb=roadb_line+screen_width-1-begin;
            if(draw_top_road_line&&road_gfx_tilenum&&begin<end){
                for(i=begin;i<end;i++){
                    gfx_word=rgfx_word((road_gfx_tilenum<<8)+(x_index>>3));
                    pixel=((gfx_word>>(7-(x_index%8)+8))&1)*2+((gfx_word>>(7-(x_index%8)))&1);
                    if(pixel||!road_trans){ if(type)pixel=(pixel-1)&3; *roadb--=(color+pixel)|pri; }
                    else *roadb--=0xf000;
                    x_index++; x_index&=0x7ff;
                }
            }
        }
        /* left edge */
        color=((palette_offs+colbank+palloffs)<<4)+(type?1:4); pri=priorities[3]<<12;
        if(background_only){
            if((roadb_clipl&0x8000)&&draw_top_road_line){ roadb=roadb_line; for(i=0;i<screen_width;i++)*roadb++=(color+(type?3:0)); }
        } else if(left_edge>=0&&left_edge<screen_width){
            x_index=(1024/2-1-left_over)&0x7ff; roadb=roadb_line+screen_width-1-left_edge;
            if(draw_top_road_line){
                for(i=left_edge;i>=0;i--){
                    gfx_word=rgfx_word((road_gfx_tilenum<<8)+(x_index>>3));
                    pixel=((gfx_word>>(7-(x_index%8)+8))&1)*2+((gfx_word>>(7-(x_index%8)))&1);
                    pixpri=(pixel==0)?0:pri;
                    if(pixel==0&&!(roadb_clipl&0x8000)) roadb++;
                    else { if(type)pixel=(pixel-1)&3; *roadb++=(color+pixel)|pixpri; }
                    x_index--; if(x_index<0)break;
                }
            }
        }
        /* right edge */
        color=((palette_offs+colbank+palroffs)<<4)+(type?1:4); pri=priorities[4]<<12;
        if(right_edge<screen_width&&right_edge>=0){
            x_index=(1024/2+right_over)&0x7ff; roadb=roadb_line+screen_width-1-right_edge;
            if(draw_top_road_line){
                for(i=right_edge;i<screen_width;i++){
                    gfx_word=rgfx_word((road_gfx_tilenum<<8)+(x_index>>3));
                    pixel=((gfx_word>>(7-(x_index%8)+8))&1)*2+((gfx_word>>(7-(x_index%8)))&1);
                    pixpri=(pixel==0)?0:pri;
                    if(pixel==0&&!(roadb_clipr&0x8000)) roadb--;
                    else { if(type)pixel=(pixel-1)&3; *roadb--=(color+pixel)|pixpri; }
                    x_index++; if(x_index>0x3ff)break;
                }
            }
        }

        /* combine A/B by priority */
        if(line_needs_drawing){
            dst16=scanline;
            for(i=0;i<screen_width;i++){
                if(roada_line[i]==0x8000) *dst16++=roadb_line[i]&0x8fff;
                else if(roadb_line[i]==0x8000) *dst16++=roada_line[i]&0x8fff;
                else if((roadb_line[i]&0x7000)>(roada_line[i]&0x7000)) *dst16++=roadb_line[i]&0x8fff;
                else *dst16++=roada_line[i]&0x8fff;
            }
            for(i=0;i<screen_width;i++) road_layer[y*screen_width+i]=scanline[i];
        }
    }
}

/* ---- sprites, ported from MAME taito_z_v.cpp::contcirc_draw_sprites_16x8 ----
 * 16x8 4bpp tiles (64 B each, tile16x8_layout), aggregated via spritemap ROM into
 * 128x128 sprites (8x16 chunks), hardware-zoomed. Draws over the RGB frame buffer. */
static inline int spr_pix(unsigned code, int x, int y){   /* 4bpp pixel 0..15 */
    unsigned base = (code*64u) & (sizeof(sgfx)-1);
    int px=0;
    for(int p=0;p<4;p++){ uint8_t b=sgfx[(base + y*8 + p*2 + (x>>3)) & (sizeof(sgfx)-1)];
        px |= ((b>>(7-(x&7)))&1)<<p; }
    return px;
}
static inline int smap_word(unsigned i){ i &= (sizeof(smap)/2-1); return smap[i*2]|(smap[i*2+1]<<8); } /* LE */

static void draw_sprites(uint8_t*img,int W,int H){
    const int y_offs=5;
    for(int offs=0; offs < (int)sizeof(spram)/2; offs+=4){
        #define SW(n) ((spram[(offs+n)*2]<<8)|spram[(offs+n)*2+1])
        int d0=SW(0), d1=SW(1), d2=SW(2), d3=SW(3);
        int zoomy=(d0&0xfe00)>>9, y=d0&0x1ff;
        int tilenum=d1&0x7ff;
        int flipx=(d2&0x4000)>>14, flipy=(d2&0x2000)>>13, x=d2&0x1ff;
        int color=(d3&0xff00)>>8, zoomx=d3&0x7f;
        #undef SW
        if(!tilenum) continue;
        unsigned map_offset=tilenum<<7;
        zoomx+=1; zoomy+=1;
        y += y_offs + (128 - zoomy);
        if(x>0x140) x-=0x200;
        if(y>0x140) y-=0x200;
        for(int chunk=0; chunk<128; chunk++){
            int k=chunk%8, j=chunk/8;
            int spx=flipx?(7-k):k, spy=flipy?(15-j):j;
            int code=smap_word(map_offset + spx + (spy<<3));
            if(code==0xffff) continue;
            int curx=x+((k*zoomx)/8), cury=y+((j*zoomy)/16);
            int zx=x+(((k+1)*zoomx)/8)-curx, zy=y+(((j+1)*zoomy)/16)-cury;
            if(zx<=0||zy<=0) continue;
            for(int dy=0;dy<zy;dy++){ int oy=cury+dy; if(oy<0||oy>=H)continue;
                int sy=(dy*8)/zy; if(flipy)sy=7-sy;
                for(int dx=0;dx<zx;dx++){ int ox=curx+dx; if(ox<0||ox>=W)continue;
                    int sx=(dx*16)/zx; if(flipx)sx=15-sx;
                    int pix=spr_pix(code,sx,sy);
                    if(!pix) continue;            /* transpen 0 */
                    int r,g,b; rgb_of(pal[(color*16+pix)&0xfff],&r,&g,&b);
                    uint8_t*o=img+(oy*W+ox)*3; o[0]=r;o[1]=g;o[2]=b;
                }
            }
        }
    }
}

int main(int argc,char**argv){
    /* visible-area pixel offsets into the tilemaps (tunable vs oracle) */
    int gx = (argc>1)?atoi(argv[1]):0;
    int gy = (argc>2)?atoi(argv[2]):0;
    /* STATE = filename prefix; default host capture. For oracle: STATE=./oracle_ */
    const char*sd = getenv("STATE"); if(!sd) sd="../02-host-ref/out/state/";
    char p[256];
    gfx_sz=load("../01-rom-analysis/out/gfx_scn.bin",gfx,sizeof gfx);
    snprintf(p,sizeof p,"%sscn_ram.bin",sd);  load(p,scn,sizeof scn);
    snprintf(p,sizeof p,"%spalette.bin",sd);  load(p,pal,sizeof pal);
    snprintf(p,sizeof p,"%sscn_ctrl.bin",sd); { uint8_t c[16]; int n=load(p,c,16); for(int i=0;i<8;i++) ctrl[i]=r16(c+i*2); }
    load("../01-rom-analysis/out/gfx_road.bin",rgfx,sizeof rgfx);
    snprintf(p,sizeof p,"%sroad_ram.bin",sd); { uint8_t c[0x2000]; int n=load(p,c,0x2000); for(int i=0;i<0x1000;i++) rram[i]=r16(c+i*2); }
    if(getenv("ROAD_PALBANK")) road_palbank=atoi(getenv("ROAD_PALBANK"));
    draw_road();
    load("sprites_asm.bin",sgfx,sizeof sgfx);
    load("spritemap_asm.bin",smap,sizeof smap);
    snprintf(p,sizeof p,"%sspriteram.bin",sd); load(p,spram,sizeof spram);
    printf("gfx=%dB scn loaded. ctrl: bgsx=%04x fgsx=%04x bgsy=%04x fgsy=%04x dis=%04x flip=%04x\n",
        gfx_sz, ctrl[0],ctrl[1],ctrl[3],ctrl[4],ctrl[6],ctrl[7]);

    render_bg("bg0.ppm", 0x0000);
    render_bg("bg1.ppm", 0x4000);
    render_tx("tx.ppm");

    /* composite 320x224: BG0 opaque, BG1, TX. Scroll = -ctrl (MAME). */
    static uint8_t img[320*224*3];
    int bgsx=-(int16_t)ctrl[0], bgsy=-(int16_t)ctrl[3];
    int fgsx=-(int16_t)ctrl[1], fgsy=-(int16_t)ctrl[4];
    int txsx=-(int16_t)ctrl[2], txsy=-(int16_t)ctrl[5];
    for(int y=0;y<224;y++) for(int x=0;x<320;x++){
        int r=0,g=0,b=0;
        bg_sample(0x0000, x-bgsx+gx, y-bgsy+gy, &r,&g,&b, 1);          /* BG0 opaque */
        { int rr,gg,bb; if(bg_sample(0x4000, x-fgsx+gx, y-fgsy+gy,&rr,&gg,&bb,0)){r=rr;g=gg;b=bb;} }
        { uint16_t rv=road_layer[y*320+x]; if(!(rv&0x8000)){ rgb_of(pal[rv&0xfff],&r,&g,&b); } }  /* TC0150ROD road */
        { int rr,gg,bb; if(tx_sample(x-txsx+gx, y-txsy+gy,&rr,&gg,&bb)){r=rr;g=gg;b=bb;} }
        uint8_t*o=img+(y*320+x)*3; o[0]=r;o[1]=g;o[2]=b;
    }
    draw_sprites(img,320,224);
    ppm("frame.ppm",img,320,224);
    return 0;
}
