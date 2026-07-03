/* cc_rtg_main.c -- Continental Circus RTG truecolour interpreter presenter.
 * Dual MC68000 Musashi interpreter + shared software renderer + YM2610/Paula sound.
 * The presenter maps the renderer's 12-bit palette pens directly to RGB888 and
 * refreshes only the scaled game window through cybergraphics WritePixelArray. */
#include <exec/exec.h>
#include <exec/memory.h>
#include <graphics/gfxbase.h>
#include <graphics/displayinfo.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <devices/timer.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/timer.h>
#include <stdint.h>
#include <string.h>
#include <stdio.h>
#include "cc_state.h"

struct IntuitionBase *IntuitionBase = 0;
struct GfxBase *GfxBase = 0;
struct Device *TimerBase = 0;
struct Library *CyberGfxBase = 0;

static ULONG cgx_BestCModeIDTagList(struct TagItem *taglist)
{
    register struct Library *a6 __asm("a6") = CyberGfxBase;
    register struct TagItem *a0 __asm("a0") = taglist;
    register ULONG d0 __asm("d0");
    __asm volatile("jsr -60(%%a6)" : "=r"(d0) : "r"(a6), "r"(a0) : "cc", "memory");
    return d0;
}

static ULONG cgx_WritePixelArray(APTR src, UWORD sx, UWORD sy, UWORD srcmod,
    struct RastPort *rastport, UWORD dx, UWORD dy, UWORD w, UWORD h, UBYTE fmt)
{
    register struct Library *a6 __asm("a6") = CyberGfxBase;
    register APTR a0 __asm("a0") = src;
    register ULONG d0 __asm("d0") = sx;
    register ULONG d1 __asm("d1") = sy;
    register ULONG d2 __asm("d2") = srcmod;
    register struct RastPort *a1 __asm("a1") = rastport;
    register ULONG d3 __asm("d3") = dx;
    register ULONG d4 __asm("d4") = dy;
    register ULONG d5 __asm("d5") = w;
    register ULONG d6 __asm("d6") = h;
    register ULONG d7 __asm("d7") = fmt;
    __asm volatile("jsr -126(%%a6)"
        : "+r"(d0)
        : "r"(a6), "r"(a0), "r"(d1), "r"(d2), "r"(a1),
          "r"(d3), "r"(d4), "r"(d5), "r"(d6), "r"(d7)
        : "cc", "memory");
    return d0;
}

#define CYBRBIDTG_Depth         (0x80050000UL + 0)
#define CYBRBIDTG_NominalWidth  (0x80050000UL + 1)
#define CYBRBIDTG_NominalHeight (0x80050000UL + 2)
#define RECTFMT_RGB             0
#define CGX_INVALID_ID          0xffffffffUL
#define CC_RTG_SWAP_RB          1

#define RTG_W 640
#define RTG_H 480
#define CC_FAST_NATIVE_PRESENT 1

static struct Screen *scr;
static struct Window *win;
static struct MsgPort *timer_port;
static struct timerequest *timer_io;
static uint16_t *game;
static uint8_t *rtg_frame;
static uint8_t pal_rgb[0x1000 * 3];
static uint8_t pal_rgb2[0x1000 * 6];
static uint32_t pal_csum = 0xffffffffUL;
static int *xmap, *ymap;
static unsigned disp_w, disp_h, stride_rgb;
static int gx, gy, gw, gh;
static ULONG frame_ticks, next_tick;
static uint8_t keydown[128];
static int g_quit, g_pause, ok, g_gear_high;
static FILE *lg;

#define RK_1 0x01
#define RK_5 0x05
#define RK_P 0x19
#define RK_X 0x32
#define RK_SPACE 0x40
#define RK_RETURN 0x44
#define RK_ESC 0x45
#define RK_UP 0x4c
#define RK_DOWN 0x4d
#define RK_RIGHT 0x4e
#define RK_LEFT 0x4f
#define RK_LCTRL 0x63
#define RK_LALT 0x64
#define JOY1DAT  (*(volatile unsigned short *)0xdff00cUL)
#define CIAA_PRA (*(volatile unsigned char  *)0xbfe001UL)
#define CIAA_DDRA (*(volatile unsigned char *)0xbfe201UL)
#define POTGO    (*(volatile unsigned short *)0xdff034UL)
#define POTINP   (*(volatile unsigned short *)0xdff016UL)
#define PORT1_FIRE 0x80
#define CD32_BLUE 0x80
#define CD32_RED  0x40
#define CD32_YELLOW 0x20
#define CD32_GREEN  0x10
#define CD32_RSHOULDER 0x08
#define CD32_LSHOULDER 0x04
#define CD32_PLAY 0x02
#define CD32_DATRY 0x4000

static void L(const char *s)
{
    if (lg) { fputs(s, lg); fputc('\n', lg); fflush(lg); }
}

static void *afast(unsigned long n)
{
    void *p = AllocMem(n, MEMF_FAST | MEMF_CLEAR);
    if (!p) p = AllocMem(n, MEMF_ANY | MEMF_CLEAR);
    return p;
}

static void store_rgb(uint8_t *p, unsigned r, unsigned g, unsigned b)
{
#if CC_RTG_SWAP_RB
    p[0] = (uint8_t)b; p[1] = (uint8_t)g; p[2] = (uint8_t)r;
#else
    p[0] = (uint8_t)r; p[1] = (uint8_t)g; p[2] = (uint8_t)b;
#endif
}

static void close_timer(void)
{
    if (timer_io) {
        if (TimerBase) CloseDevice((struct IORequest *)timer_io);
        DeleteIORequest((struct IORequest *)timer_io);
        timer_io = 0;
    }
    if (timer_port) { DeleteMsgPort(timer_port); timer_port = 0; }
    TimerBase = 0;
    frame_ticks = next_tick = 0;
}

static void open_timer(void)
{
    struct EClockVal ev;
    ULONG rate;
    timer_port = CreateMsgPort();
    if (!timer_port) return;
    timer_io = (struct timerequest *)CreateIORequest(timer_port, sizeof(*timer_io));
    if (!timer_io) { close_timer(); return; }
    if (OpenDevice((CONST_STRPTR)TIMERNAME, UNIT_ECLOCK, (struct IORequest *)timer_io, 0) != 0) {
        close_timer();
        return;
    }
    TimerBase = timer_io->tr_node.io_Device;
    rate = ReadEClock(&ev);
    frame_ticks = (rate + 30) / 60;
    if (frame_ticks < 1) frame_ticks = 1;
    next_tick = ev.ev_lo;
}

static void frame_pace(void)
{
    struct EClockVal ev;
    ULONG now;
    if (!TimerBase || !frame_ticks) { WaitTOF(); return; }
    ReadEClock(&ev);
    now = ev.ev_lo;
    if ((LONG)(now - next_tick) > (LONG)frame_ticks) {
        next_tick = now;
        return;
    }
    next_tick += frame_ticks;
    do {
        ReadEClock(&ev);
        now = ev.ev_lo;
    } while ((LONG)(now - next_tick) < 0);
}

static unsigned read_cd32(void)
{
    unsigned out = 0;
    volatile unsigned char t;
    CIAA_DDRA |= PORT1_FIRE;
    CIAA_PRA &= (unsigned char)~PORT1_FIRE;
    POTGO = 0x6f00;
    for (int i = 7; i >= 0; i--) {
        t = CIAA_PRA; t = CIAA_PRA; t = CIAA_PRA; t = CIAA_PRA; (void)t;
        if (!(POTINP & CD32_DATRY)) out |= (1u << i);
        CIAA_PRA |= PORT1_FIRE;
        CIAA_PRA &= (unsigned char)~PORT1_FIRE;
    }
    CIAA_DDRA &= (unsigned char)~PORT1_FIRE;
    POTGO = 0xff00;
    CIAA_PRA |= 0xc0;
    return out;
}

static int try_open_depth(int w, int h, int depth)
{
    struct TagItem tags[4];
    ULONG mode = CGX_INVALID_ID;

    if (CyberGfxBase) {
        tags[0].ti_Tag = CYBRBIDTG_NominalWidth;  tags[0].ti_Data = (ULONG)w;
        tags[1].ti_Tag = CYBRBIDTG_NominalHeight; tags[1].ti_Data = (ULONG)h;
        tags[2].ti_Tag = CYBRBIDTG_Depth;         tags[2].ti_Data = (ULONG)depth;
        tags[3].ti_Tag = TAG_DONE;                tags[3].ti_Data = 0;
        mode = cgx_BestCModeIDTagList(tags);
    }
    if (mode == CGX_INVALID_ID || mode == INVALID_ID) {
        mode = BestModeID(BIDTAG_NominalWidth, (ULONG)w,
                          BIDTAG_NominalHeight, (ULONG)h,
                          BIDTAG_DesiredWidth, (ULONG)w,
                          BIDTAG_DesiredHeight, (ULONG)h,
                          BIDTAG_Depth, (ULONG)depth,
                          TAG_DONE);
    }
    if (mode != INVALID_ID && mode != CGX_INVALID_ID) {
        scr = OpenScreenTags(0, SA_DisplayID, mode, SA_Width, (ULONG)w, SA_Height, (ULONG)h,
                             SA_Depth, (ULONG)depth, SA_Type, CUSTOMSCREEN,
                             SA_Quiet, TRUE, SA_ShowTitle, FALSE, TAG_END);
        if (scr) return 1;
    }
    return 0;
}

static int try_open(int w, int h)
{
    if (try_open_depth(w, h, 24)) return 1;
    if (try_open_depth(w, h, 32)) return 1;
    return 0;
}

static void pixel(int x, int y, unsigned r, unsigned g, unsigned b)
{
    if ((unsigned)x >= disp_w || (unsigned)y >= disp_h) return;
    uint8_t *p = rtg_frame + (size_t)y * stride_rgb + (size_t)x * 3;
    store_rgb(p, r, g, b);
}

static void fill_rect(int x, int y, int w, int h, unsigned r, unsigned g, unsigned b)
{
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > (int)disp_w) w = (int)disp_w - x;
    if (y + h > (int)disp_h) h = (int)disp_h - y;
    if (w <= 0 || h <= 0) return;
    for (int yy = 0; yy < h; yy++) {
        uint8_t *p = rtg_frame + (size_t)(y + yy) * stride_rgb + (size_t)x * 3;
        for (int xx = 0; xx < w; xx++) {
            store_rgb(p, r, g, b); p += 3;
        }
    }
}

static const uint8_t *glyph(char c)
{
    static const uint8_t blank[7] = {0,0,0,0,0,0,0};
    static const uint8_t font[36][7] = {
        {14,17,17,31,17,17,17},{30,17,17,30,17,17,30},{14,17,16,16,16,17,14},
        {30,17,17,17,17,17,30},{31,16,16,30,16,16,31},{31,16,16,30,16,16,16},
        {14,17,16,23,17,17,15},{17,17,17,31,17,17,17},{14,4,4,4,4,4,14},
        {7,2,2,2,18,18,12},{17,18,20,24,20,18,17},{16,16,16,16,16,16,31},
        {17,27,21,21,17,17,17},{17,25,21,19,17,17,17},{14,17,17,17,17,17,14},
        {30,17,17,30,16,16,16},{14,17,17,17,21,18,13},{30,17,17,30,20,18,17},
        {15,16,16,14,1,1,30},{31,4,4,4,4,4,4},{17,17,17,17,17,17,14},
        {17,17,17,17,17,10,4},{17,17,17,21,21,21,10},{17,17,10,4,10,17,17},
        {17,17,10,4,4,4,4},{31,1,2,4,8,16,31},
        {14,17,19,21,25,17,14},{4,12,4,4,4,4,14},{14,17,1,2,4,8,31},
        {30,1,1,14,1,1,30},{2,6,10,18,31,2,2},{31,16,30,1,1,17,14},
        {6,8,16,30,17,17,14},{31,1,2,4,8,8,8},{14,17,17,14,17,17,14},
        {14,17,17,15,1,2,12}
    };
    if (c >= 'A' && c <= 'Z') return font[c - 'A'];
    if (c >= '0' && c <= '9') return font[26 + c - '0'];
    return blank;
}

static void draw_text(const char *s, int x, int y, int scale, unsigned r, unsigned g, unsigned b)
{
    int ox = x;
    for (; *s; s++) {
        if (*s == '\n') { y += 9 * scale; x = ox; continue; }
        if (*s == ' ') { x += 4 * scale; continue; }
        const uint8_t *gph = glyph(*s);
        for (int yy = 0; yy < 7; yy++)
            for (int xx = 0; xx < 5; xx++)
                if (gph[yy] & (1 << (4 - xx)))
                    fill_rect(x + xx * scale, y + yy * scale, scale, scale, r, g, b);
        x += 6 * scale;
    }
}

static void draw_text_center(const char *s, int y, int scale, unsigned r, unsigned g, unsigned b)
{
    int n = 0;
    for (const char *p = s; *p; p++) n += (*p == ' ') ? 4 : 6;
    draw_text(s, ((int)disp_w - n * scale) / 2, y, scale, r, g, b);
}

static void draw_shell(void)
{
    memset(rtg_frame, 0, (size_t)stride_rgb * disp_h);
    for (unsigned y = 0; y < disp_h; y++) {
        unsigned shade = 18 + (y * 30) / (disp_h ? disp_h : 1);
        fill_rect(0, (int)y, (int)disp_w, 1, shade / 2, shade, shade + 8);
    }
    fill_rect(gx - 8, gy - 8, gw + 16, gh + 16, 190, 148, 44);
    fill_rect(gx - 4, gy - 4, gw + 8, gh + 8, 18, 20, 22);
    fill_rect(gx, gy, gw, gh, 0, 0, 0);
    draw_text_center("CONTINENTAL CIRCUS", 22, 4, 236, 218, 152);
    draw_text_center("TAITO Z SYSTEM RTG INTERPRETER", (int)disp_h - 74, 2, 170, 210, 230);
    draw_text_center("5 COIN  1 START  ESC EXIT", (int)disp_h - 48, 2, 220, 220, 220);
    draw_text_center("ARROWS STEER  FIRE GAS  BLUE GEAR", (int)disp_h - 26, 2, 220, 220, 220);
}

static void present_full(void)
{
    if (win && rtg_frame)
        cgx_WritePixelArray(rtg_frame, 0, 0, (UWORD)stride_rgb, win->RPort, 0, 0,
                            (UWORD)disp_w, (UWORD)disp_h, RECTFMT_RGB);
}

static void poll_keys(void)
{
    struct IntuiMessage *m;
    if (!win || !win->UserPort) return;
    while ((m = (struct IntuiMessage *)GetMsg(win->UserPort))) {
        ULONG cls = m->Class;
        UWORD raw = m->Code;
        ReplyMsg((struct Message *)m);
        if (cls == IDCMP_RAWKEY) keydown[raw & 0x7f] = (raw & 0x80) ? 0 : 1;
    }
}

static void run_loader(void)
{
    int tick = 0, prev = 1;
    for (;;) {
        poll_keys();
        unsigned cd32 = read_cd32();
        int fire = !(CIAA_PRA & PORT1_FIRE) || keydown[RK_SPACE] || keydown[RK_LCTRL] ||
                   (cd32 & (CD32_RED | CD32_BLUE | CD32_YELLOW | CD32_GREEN));
        int start = keydown[RK_1] || keydown[RK_RETURN] || (cd32 & CD32_PLAY);
        int now = fire || start;
        if (keydown[RK_ESC]) { g_quit = 1; return; }
        draw_shell();
        if (((tick / 24) & 1) == 0)
            draw_text_center("PRESS FIRE OR START", (int)disp_h - 112, 3, 255, 255, 255);
        present_full();
        if (tick > 30 && now && !prev) break;
        prev = now;
        tick++;
        frame_pace();
    }
    memset(keydown, 0, sizeof keydown);
    draw_shell();
    present_full();
}

static void build_geometry(void)
{
#if CC_FAST_NATIVE_PRESENT
    gw = CC_W * 2;
    gh = CC_H * 2;
    if (gw > (int)disp_w) gw = (int)disp_w;
    if (gh > (int)disp_h) gh = (int)disp_h;
#else
    int maxw = (int)disp_w - 96;
    int maxh = (int)disp_h - 38;
    if (maxw < CC_W) maxw = (int)disp_w - 16;
    if (maxh < CC_H) maxh = (int)disp_h - 16;
    gw = maxw;
    gh = (CC_H * gw) / CC_W;
    if (gh > maxh) {
        gh = maxh;
        gw = (CC_W * gh) / CC_H;
    }
    if (gw < 1) gw = CC_W;
    if (gh < 1) gh = CC_H;
#endif
    gx = ((int)disp_w - gw) / 2;
    gy = ((int)disp_h - gh) / 2;
    for (int x = 0; x < gw; x++) xmap[x] = (x * CC_W) / gw;
    for (int y = 0; y < gh; y++) ymap[y] = (y * CC_H) / gh;
}

static void rebuild_palette(void)
{
    uint32_t cs = 2166136261UL;
    const uint32_t *p = (const uint32_t *)cc_pal;
    for (int i = 0; i < (int)(sizeof(cc_pal[0]) * 0x1000 / 4); i++)
        cs = (cs ^ p[i]) * 16777619UL;
    if (cs == pal_csum) return;
    for (int i = 0; i < 0x1000; i++) {
        unsigned c = cc_pal[i] & 0x7fff;
        unsigned r = (c >> 10) & 0x1f;
        unsigned g = (c >> 5) & 0x1f;
        unsigned b = c & 0x1f;
        pal_rgb[i * 3 + 0] = (uint8_t)((r << 3) | (r >> 2));
        pal_rgb[i * 3 + 1] = (uint8_t)((g << 3) | (g >> 2));
        pal_rgb[i * 3 + 2] = (uint8_t)((b << 3) | (b >> 2));
#if CC_RTG_SWAP_RB
        pal_rgb2[i * 6 + 0] = pal_rgb2[i * 6 + 3] = pal_rgb[i * 3 + 2];
        pal_rgb2[i * 6 + 1] = pal_rgb2[i * 6 + 4] = pal_rgb[i * 3 + 1];
        pal_rgb2[i * 6 + 2] = pal_rgb2[i * 6 + 5] = pal_rgb[i * 3 + 0];
#else
        pal_rgb2[i * 6 + 0] = pal_rgb2[i * 6 + 3] = pal_rgb[i * 3 + 0];
        pal_rgb2[i * 6 + 1] = pal_rgb2[i * 6 + 4] = pal_rgb[i * 3 + 1];
        pal_rgb2[i * 6 + 2] = pal_rgb2[i * 6 + 5] = pal_rgb[i * 3 + 2];
#endif
    }
    pal_csum = cs;
}

static void present_game(void)
{
    if (!win || !rtg_frame || !game) return;
    rebuild_palette();
    if (gw == CC_W * 2 && gh == CC_H * 2) {
        const int rowbytes = CC_W * 2 * 3;
        for (int sy = 0; sy < CC_H; sy++) {
            uint8_t *dst = rtg_frame + (size_t)(gy + sy * 2) * stride_rgb + (size_t)gx * 3;
            const uint16_t *src = game + (size_t)sy * CC_W;
            for (int x = 0; x < CC_W; x++) {
                const uint8_t *rgb = pal_rgb2 + (size_t)(src[x] & 0xfff) * 6;
                dst[0] = rgb[0]; dst[1] = rgb[1]; dst[2] = rgb[2];
                dst[3] = rgb[3]; dst[4] = rgb[4]; dst[5] = rgb[5];
                dst += 6;
            }
            memcpy(dst - rowbytes + stride_rgb, dst - rowbytes, rowbytes);
        }
    } else {
        int last_sy = -1;
        for (int y = 0; y < gh; y++) {
            uint8_t *dst = rtg_frame + (size_t)(gy + y) * stride_rgb + (size_t)gx * 3;
            int sy = ymap[y];
            if (sy == last_sy) {
                memcpy(dst, dst - stride_rgb, (size_t)gw * 3);
                continue;
            }
            last_sy = sy;
            const uint16_t *src = game + (size_t)sy * CC_W;
            for (int x = 0; x < gw; x++) {
                const uint8_t *rgb = pal_rgb + (size_t)(src[xmap[x]] & 0xfff) * 3;
                store_rgb(dst, rgb[0], rgb[1], rgb[2]);
                dst += 3;
            }
        }
    }
    fill_rect(gx + 8, gy + 8, 46, 24, 4, 6, 8);
    fill_rect(gx + 9, gy + 9, 44, 22, 34, 38, 42);
    draw_text(g_gear_high ? "HI" : "LO", gx + 16, gy + 14, 2, 255, 236, 120);
    cgx_WritePixelArray(rtg_frame + (size_t)gy * stride_rgb + (size_t)gx * 3, 0, 0,
                        (UWORD)stride_rgb, win->RPort, (UWORD)gx, (UWORD)gy,
                        (UWORD)gw, (UWORD)gh, RECTFMT_RGB);
}

static void poll_input(void)
{
    static int c1p, s1p, c1h, s1h, p_prev, play_started, gear_high;
    int steer = 0;
    poll_keys();
    if (keydown[RK_ESC]) g_quit = 1;

    unsigned cd32 = read_cd32();
    int play = (cd32 & CD32_PLAY) ? 1 : 0;
    int pause_key = keydown[RK_P] || (play && play_started);
    if (pause_key && !p_prev) g_pause = !g_pause;
    p_prev = pause_key;
    unsigned v = JOY1DAT;
    int right = (v >> 1) & 1;
    int left = (v >> 9) & 1;
    if (keydown[RK_RIGHT] || right) steer = -0x40;
    if (keydown[RK_LEFT] || left) steer = +0x40;

    int fire = !(CIAA_PRA & PORT1_FIRE) || keydown[RK_UP] || keydown[RK_SPACE] || keydown[RK_LCTRL] ||
               (cd32 & CD32_RED);
    if (keydown[RK_LALT] || (cd32 & CD32_LSHOULDER)) gear_high = 0;
    if (keydown[RK_X] || (cd32 & CD32_RSHOULDER)) gear_high = 1;
    uint8_t in0 = 0x13, in1 = 0x1f;
    int coin = keydown[RK_5] || (cd32 & CD32_BLUE);
    int start = keydown[RK_1] || keydown[RK_RETURN] || play;
    if (coin && !c1p) c1h = 8; c1p = coin;
    if (start && !s1p) s1h = 8; s1p = start;
    if (play && !play_started) play_started = 1;
    if (c1h) { in0 |= 0x08; c1h--; }
    if (s1h) { in1 &= (uint8_t)~0x08; s1h--; }
    if (gear_high) in1 &= (uint8_t)~0x10;
    g_gear_high = gear_high;
    if (fire) in0 |= 0x80;     /* MAME gas_pedal_r raw 7 -> encoded 4, shifted to IN0 bits 5..7 */
    cc_set_inputs(in0, in1, 0xff, 0xcf, steer);
}

static void shutdown(void)
{
    static int done;
    if (done) return;
    done = 1;
    cc_audio_amiga_close();
    close_timer();
    if (win) { CloseWindow(win); win = 0; }
    if (scr) { CloseScreen(scr); scr = 0; }
    if (rtg_frame) { FreeMem(rtg_frame, (unsigned long)stride_rgb * disp_h); rtg_frame = 0; }
    if (game) { FreeMem(game, CC_W * CC_H * 2UL); game = 0; }
    if (xmap) { FreeMem(xmap, (unsigned long)disp_w * sizeof(int)); xmap = 0; }
    if (ymap) { FreeMem(ymap, (unsigned long)disp_h * sizeof(int)); ymap = 0; }
    if (CyberGfxBase) { CloseLibrary(CyberGfxBase); CyberGfxBase = 0; }
    if (GfxBase) { CloseLibrary((struct Library *)GfxBase); GfxBase = 0; }
    if (IntuitionBase) { CloseLibrary((struct Library *)IntuitionBase); IntuitionBase = 0; }
    if (lg) { fclose(lg); lg = 0; }
}

int main(void)
{
    lg = fopen("cc_log.txt", "w");
    L("cc_rtg start");
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    CyberGfxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 0);
    if (!IntuitionBase || !GfxBase || !CyberGfxBase) { L("FAIL libs"); shutdown(); return 20; }
    open_timer();

    if (!try_open(RTG_W, RTG_H) && !try_open(800, 600) && !try_open(640, 480)) {
        L("FAIL OpenScreen RTG");
        shutdown();
        return 23;
    }
    disp_w = (unsigned)scr->Width;
    disp_h = (unsigned)scr->Height;
    stride_rgb = disp_w * 3;
    xmap = (int *)afast((unsigned long)disp_w * sizeof(int));
    ymap = (int *)afast((unsigned long)disp_h * sizeof(int));
    game = (uint16_t *)afast(CC_W * CC_H * 2UL);
    rtg_frame = (uint8_t *)afast((unsigned long)stride_rgb * disp_h);
    if (!xmap || !ymap || !game || !rtg_frame) { L("FAIL bufs"); shutdown(); return 21; }
    build_geometry();

    win = OpenWindowTags(0, WA_CustomScreen, (ULONG)scr, WA_Left, 0, WA_Top, 0,
                         WA_Width, (ULONG)disp_w, WA_Height, (ULONG)disp_h,
                         WA_Backdrop, TRUE, WA_Borderless, TRUE, WA_Activate, TRUE,
                         WA_RMBTrap, TRUE, WA_IDCMP, IDCMP_RAWKEY, TAG_END);
    if (!win) { L("FAIL OpenWindow"); shutdown(); return 24; }
    ScreenToFront(scr);
    ActivateWindow(win);
    { char b[96]; sprintf(b, "screen %ux%u game %dx%d at %d,%d", disp_w, disp_h, gw, gh, gx, gy); L(b); }

    draw_shell();
    present_full();
    run_loader();
    if (g_quit) { shutdown(); return 0; }
    memset(rtg_frame, 0, (size_t)stride_rgb * disp_h);
    present_full();

    L("machine_init");
    cc_machine_init();
    cc_audio_init();
    cc_audio_amiga_open();
    ok = 1;
    L("loop");

    while (!g_quit) {
        poll_input();
        if (!g_pause) {
            { extern long cc_audio_amiga_take_z80(void); extern void cc_audio_frame_budget(int);
              cc_audio_frame_budget((int)cc_audio_amiga_take_z80()); }   /* Z80 budget -> */
            cc_machine_run_frame();      /* ...drained in slices interleaved with the 68ks */
            cc_audio_amiga_frame();
            cc_render_frame(game, CC_W);
            present_game();
        }
        frame_pace();
        /* audio telemetry every ~600 frames: proves on HARDWARE whether the FM
         * music is keyed + generated (keyon/ymPeak>0 => level problem, not logic) */
        { static int tf = 0;   /* ~25-40s cadence: in-HDF writes are a corruption
                                * vector on a starved emulator (Indy lesson) */
          if (++tf >= 1200) { tf = 0;
            extern int cc_dbg_keyon, cc_dbg_sytc, cc_dbg_tover, cc_dbg_irq, cc_dbg_ymraw, cc_dbg_adpcma;
            char tb[96];
            sprintf(tb, "aud: keyon=%d sytc=%d tover=%d irq=%d ymraw=%d adpcma=%d",
                    cc_dbg_keyon, cc_dbg_sytc, cc_dbg_tover, cc_dbg_irq, cc_dbg_ymraw, cc_dbg_adpcma);
            L(tb);
            if (lg) fflush(lg);
          } }
    }
    shutdown();
    return ok ? 0 : 1;
}
