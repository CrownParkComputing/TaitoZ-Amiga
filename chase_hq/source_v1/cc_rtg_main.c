/* cc_rtg_main.c -- Chase HQ RTG truecolour interpreter presenter.
 * Dual MC68000 Musashi interpreter + shared software renderer + YM2610/Paula sound.
 * The presenter maps the renderer's 12-bit palette pens directly to RGB888 and
 * refreshes only the scaled game window through cybergraphics WritePixelArray. */
#include <exec/exec.h>
#include <exec/memory.h>
#include <dos/dos.h>
#include <graphics/gfxbase.h>
#include <graphics/displayinfo.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <devices/timer.h>
#include <utility/tagitem.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/dos.h>
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

#define RTG_W 320
#define RTG_H 256
#define CC_FAST_NATIVE_PRESENT 1
#define CHQ_DRIVE_LOCK_MAX_FRAMES 90
#define CHQ_DRIVE_READY_STREAK 6

static struct Screen *scr;
static struct Window *win;
static struct MsgPort *timer_port;
static struct timerequest *timer_io;
static uint16_t *game;
static uint8_t *rtg_frame;
static uint8_t pal_rgb[0x1000 * 3];
static uint8_t pal_rgb2[0x1000 * 6];
static int *xmap, *ymap;
static unsigned disp_w, disp_h, stride_rgb;
static int gx, gy, gw, gh;
static ULONG frame_ticks, next_tick;
static uint8_t keydown[128];
static int g_quit, g_pause, ok, g_gear_high, g_turbo_active, g_nitro_stock = 3, g_nitro_flash;
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

/* render-cache allocations (cc_render.c).  AllocMem memory is NOT reclaimed
 * automatically at process exit, so track each block and free in shutdown. */
#define CC_BIG_MAX 4
static void *big_ptrs[CC_BIG_MAX];
static unsigned long big_sizes[CC_BIG_MAX];
static int big_n;
void *cc_big_alloc(unsigned long n)
{
    void *p = afast(n);
    if (p && big_n < CC_BIG_MAX) { big_ptrs[big_n] = p; big_sizes[big_n] = n; big_n++; }
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

static ULONG eclk(void)
{
    struct EClockVal ev;
    if (!TimerBase) return 0;
    ReadEClock(&ev);
    return ev.ev_lo;
}

/* Emulated EClock has been observed stepping BACKWARDS by a few ticks between
 * tightly spaced reads (Amiberry under host load).  A negative delta cast to
 * ULONG is ~4e9 and poisons every accumulator it touches, so clamp to 0. */
static ULONG eclk_delta(ULONG later, ULONG earlier)
{
    ULONG d = later - earlier;
    return ((LONG)d < 0) ? 0 : d;
}

static void frame_pace_n(int n)
{
    struct EClockVal ev;
    ULONG now, step;
    if (!TimerBase || !frame_ticks) { while (n-- > 0) WaitTOF(); return; }
    if (n < 1) n = 1;
    step = frame_ticks * (ULONG)n;
    ReadEClock(&ev);
    now = ev.ev_lo;
    if ((LONG)(now - next_tick) > (LONG)step) {   /* fell behind: resync, no debt */
        next_tick = now;
        return;
    }
    next_tick += step;
    do {
        ReadEClock(&ev);
        now = ev.ev_lo;
    } while ((LONG)(now - next_tick) < 0);
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
    draw_text_center("CHASE HQ", 22, 4, 236, 218, 152);
    draw_text_center("TAITO Z SYSTEM RTG INTERPRETER", (int)disp_h - 74, 2, 170, 210, 230);
    draw_text_center("5 COIN  1 START  ESC EXIT", (int)disp_h - 48, 2, 220, 220, 220);
    draw_text_center("RED GAS  BLUE NITRO  YELLOW GEAR", (int)disp_h - 26, 2, 220, 220, 220);
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

static int whitty_no_game_loader(void)
{
    char v[8];
    LONG n;
    n = GetVar("WHITTY_NO_GAME_LOADER", v, sizeof(v), 0);
    return n > 0 && v[0] != '0';
}

static int perfhud_enabled(void)
{
    char v[8];
    LONG n;
    n = GetVar("CHQPERF", v, sizeof(v), 0);
    if (n > 0) return v[0] != '0';
    return 1;   /* default ON while tuning (launchers bypass the SetEnv script) */
}

/* On-screen perf HUD (default on; SetEnv CHQPERF 0 hides): once a second,
 * GAME fps (guest frames -- should hold ~60), DISPLAY fps, skip cap, and the
 * percent of wall time in machine / audio / render / present.  Drawn in the
 * top border strip, outside the game rect, so present_game never overwrites it.
 * CHQ_AUDIO_NATIVE builds append `K<FM key-ons> H<ymHash low 16>`: K growing
 * steadily on the title screen = the music sequencer is alive and being fed
 * real time; H is the rolling FNV over every YM write (same value the host
 * gates print), so target-vs-host divergence is visible without ears. */
#ifdef CHQ_AUDIO_NATIVE
extern int cc_dbg_keyon;
extern unsigned cc_dbg_ymhash;
#endif
static void draw_perfhud(ULONG elapsed, int disp_frames, int game_frames, int skip,
                         ULONG tm, ULONG ta, ULONG tr, ULONG tp)
{
    char line[96];
    ULONG rate = frame_ticks * 60;
    ULONG e100 = elapsed / 100;
    if (!elapsed || !rate || !e100) return;
    {
        ULONG e10 = elapsed / 10;
        ULONG g10 = ((ULONG)game_frames * rate) / e10;   /* counts<=250, rate<1M: fits */
        ULONG d10 = ((ULONG)disp_frames * rate) / e10;
        sprintf(line, "G%lu.%lu D%lu.%lu S%ld M%lu A%lu R%lu P%lu",
                (unsigned long)(g10 / 10), (unsigned long)(g10 % 10),
                (unsigned long)(d10 / 10), (unsigned long)(d10 % 10), (long)skip,
                (unsigned long)(tm / e100), (unsigned long)(ta / e100),
                (unsigned long)(tr / e100), (unsigned long)(tp / e100));
        /* audio split: Z=Z80 exec, Y=YM synth, F=ring flush/copy (of A) */
        {
            extern ULONG cc_prof_z80, cc_prof_synth, cc_prof_flush;
            sprintf(line + strlen(line), " Z%lu Y%lu F%lu",
                    (unsigned long)(cc_prof_z80 / e100),
                    (unsigned long)(cc_prof_synth / e100),
                    (unsigned long)(cc_prof_flush / e100));
            cc_prof_z80 = cc_prof_synth = cc_prof_flush = 0;
        }
#ifdef CHQ_AUDIO_NATIVE
        sprintf(line + strlen(line), " K%lu H%04lx",
                (unsigned long)cc_dbg_keyon,
                (unsigned long)(cc_dbg_ymhash & 0xffff));
#endif
    }
#ifdef CHQ_AUDIO_NATIVE
#define PERFHUD_W 322   /* room for the appended K<keyons> H<hash16> */
#else
#define PERFHUD_W 262
#endif
    fill_rect(2, 2, PERFHUD_W, 11, 4, 6, 8);
    for (char *p = line; *p; p++) if (*p >= 'a' && *p <= 'z') *p -= 32;
    draw_text(line, 4, 4, 1, 255, 255, 120);
    cgx_WritePixelArray(rtg_frame + (size_t)2 * stride_rgb + (size_t)2 * 3, 0, 0,
                        (UWORD)stride_rgb, win->RPort, 2, 2, PERFHUD_W, 11, RECTFMT_RGB);
}

static void build_geometry(void)
{
#if CC_FAST_NATIVE_PRESENT
    gw = CC_W;
    gh = CC_H;
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
    /* Power Drift pattern: the machine sets cc_pal_dirty on any palette write,
     * so the steady-state frame skips this entirely (the old FNV checksum over
     * the 16KB palette ran every frame). */
    if (!cc_pal_dirty) return;
    cc_pal_dirty = 0;
    for (int i = 0; i < 0x1000; i++) {
        unsigned c = cc_pal[i] & 0x7fff;
        unsigned r = c & 0x1f;
        unsigned g = (c >> 5) & 0x1f;
        unsigned b = (c >> 10) & 0x1f;
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
}

static void present_game(void)
{
    if (!win || !rtg_frame || !game) return;
    rebuild_palette();
    if (gw == CC_W && gh == CC_H) {
        for (int sy = 0; sy < CC_H; sy++) {
            uint8_t *dst = rtg_frame + (size_t)(gy + sy) * stride_rgb + (size_t)gx * 3;
            const uint16_t *src = game + (size_t)sy * CC_W;
            for (int x = 0; x < CC_W; x++) {
                const uint8_t *rgb = pal_rgb + (size_t)(src[x] & 0xfff) * 3;
                store_rgb(dst, rgb[0], rgb[1], rgb[2]);
                dst += 3;
            }
        }
    } else if (gw == CC_W * 2 && gh == CC_H * 2) {
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
    cgx_WritePixelArray(rtg_frame + (size_t)gy * stride_rgb + (size_t)gx * 3, 0, 0,
                        (UWORD)stride_rgb, win->RPort, (UWORD)gx, (UWORD)gy,
                        (UWORD)gw, (UWORD)gh, RECTFMT_RGB);
    {
        char stock[2];
        int ox = gx + 8;
        int oy = gy + gh + 3;
        if (oy + 10 > (int)disp_h) oy = gy + gh - 13;
        if (oy < 0) oy = 0;
        stock[0] = (char)('0' + (g_nitro_stock > 9 ? 9 : g_nitro_stock));
        stock[1] = 0;
        fill_rect(ox - 3, oy - 2, 146, 13, 4, 6, 8);
        fill_rect(ox - 2, oy - 1, 144, 11, 26, 30, 34);
        draw_text("GEAR", ox, oy, 1, 255, 236, 120);
        draw_text(g_gear_high ? "HI" : "LO", ox + 34, oy, 1, 255, 255, 255);
        draw_text("NITRO", ox + 62, oy, 1,
                  (g_turbo_active || g_nitro_flash) ? 255 : 210,
                  (g_turbo_active || g_nitro_flash) ? 88 : 210,
                  (g_turbo_active || g_nitro_flash) ? 48 : 210);
        draw_text(stock, ox + 110, oy, 1, 255, 255, 255);
        cgx_WritePixelArray(rtg_frame + (size_t)oy * stride_rgb + (size_t)(ox - 3) * 3, 0, 0,
                            (UWORD)stride_rgb, win->RPort, (UWORD)(ox - 3), (UWORD)oy,
                            146, 13, RECTFMT_RGB);
        if (g_nitro_flash) g_nitro_flash--;
    }
}

static int race_video_active(void)
{
    for (unsigned i = 0; i < sizeof(cc_spr); i++)
        if (cc_spr[i]) return 1;
    for (unsigned i = 0; i < sizeof(cc_road); i++)
        if (cc_road[i]) return 1;
    return 0;
}

static void poll_input(void)
{
    static int c1p, s1p, c1h, s1h, p_prev, gear_prev, gear_high, turbo_prev;
    static int race_started, drive_enabled, drive_lock_frames, drive_ready_frames;
    int steer = 0;
    poll_keys();
    if (keydown[RK_ESC]) g_quit = 1;
    if (keydown[RK_P] && !p_prev) { g_pause = !g_pause; cc_audio_amiga_pause(g_pause); }
    p_prev = keydown[RK_P];

    unsigned cd32 = read_cd32();
    unsigned v = JOY1DAT;
    int right = (v >> 1) & 1;
    int left = (v >> 9) & 1;
    if (drive_enabled) {
        if (keydown[RK_RIGHT] || right) steer = +0x40;
        if (keydown[RK_LEFT] || left) steer = -0x40;
    }

    int gas = !(CIAA_PRA & PORT1_FIRE) || keydown[RK_UP] || keydown[RK_SPACE] || keydown[RK_LCTRL] ||
              (cd32 & CD32_RED);
    int brake = keydown[RK_DOWN] || (cd32 & CD32_RSHOULDER);
    int turbo = keydown[RK_LALT] || (cd32 & (CD32_BLUE | CD32_GREEN));
    int gear = keydown[RK_X] || (cd32 & CD32_YELLOW);
    if (!drive_enabled) brake = turbo = gear = 0;
    if (gear && !gear_prev) gear_high ^= 1;
    gear_prev = gear;
    uint8_t in0 = 0x33, in1 = 0x3f;
    int coin = keydown[RK_5] || (cd32 & CD32_LSHOULDER);
    int start = keydown[RK_1] || keydown[RK_RETURN] || (cd32 & CD32_PLAY);
    if (coin && !c1p) {
        c1h = 8;
        gear_high = 0;
        g_gear_high = 0;
        race_started = 0;
        drive_enabled = 0;
        drive_lock_frames = 0;
        drive_ready_frames = 0;
    }
    c1p = coin;
    if (start && !s1p) {
        s1h = 8;
        g_nitro_stock = 3;
        g_turbo_active = 0;
        g_nitro_flash = 0;
        g_gear_high = 0;
        gear_high = 0;
        turbo_prev = 0;
        race_started = 1;
        drive_enabled = 0;
        drive_lock_frames = CHQ_DRIVE_LOCK_MAX_FRAMES;
        drive_ready_frames = 0;
    }
    s1p = start;
    if (!drive_enabled && race_started && drive_lock_frames > 0) {
        if (race_video_active()) {
            if (++drive_ready_frames >= CHQ_DRIVE_READY_STREAK) drive_enabled = 1;
        } else {
            drive_ready_frames = 0;
        }
        if (--drive_lock_frames == 0) drive_enabled = 1;
    }
    if (c1h) { in0 |= 0x04; c1h--; }
    if (s1h) { in1 &= (uint8_t)~0x08; s1h--; }
    if (drive_enabled && gear_high) in1 &= (uint8_t)~0x10;
    if (!race_started) gas = 0;
    if (gas) in1 &= (uint8_t)~0x20;
    if (turbo && !turbo_prev) {
        if (g_nitro_stock > 0) g_nitro_stock--;
        g_nitro_flash = 18;
    }
    turbo_prev = turbo;
    g_turbo_active = turbo;
    if (turbo) in1 &= (uint8_t)~0x01;
    if (brake) in0 &= (uint8_t)~0x20;
    g_gear_high = gear_high;
    cc_set_inputs(in0, in1, 0xff, 0xff, steer);
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
    while (big_n > 0) { big_n--; FreeMem(big_ptrs[big_n], big_sizes[big_n]); big_ptrs[big_n] = 0; }
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
#ifndef __amigaos__
    lg = fopen("cc_log.txt", "w");
#endif
    L("chasehq_rtg start");
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
    if (!whitty_no_game_loader())
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

    {
        /* Wall-clock guest-frame scheduler (Power Drift model): GAME time runs
         * at a true 60Hz regardless of display rate.  When a display frame
         * takes longer than 1/60s, run up to 1+max_skip guest frames to catch
         * up, then render once -- so the game holds full speed and only the
         * display rate floats.  F1 cycles max_skip 0..3. */
        int hud = perfhud_enabled() && TimerBase != 0;
        int max_skip = 2, f1_prev = 0;
        ULONG per = frame_ticks ? frame_ticks : 1;
        /* STEADY-CADENCE scheduler (Indiana Jones v8 lesson: sporadic skips =
         * judder; a constant guest:display ratio at even spacing looks smooth
         * even at a low display rate).  cad = guest frames per present, held
         * FIXED and paced to cad*per so game time is an exact 60Hz and every
         * present is evenly spaced.  cad rises after 12 consecutive overrun
         * loops, falls only after ~1.5s of consistent headroom (hysteresis =
         * no oscillation).  The old wall-clock due/carry burst scheduler ran
         * 1,3,2,3... guest frames per present = the "disjointed" road. */
        int cad = 1, over_run = 0, under_run = 0;
        ULONG pt_mach = 0, pt_aud = 0, pt_rend = 0, pt_pres = 0, pt_start = eclk();
        int pdisp = 0, pgame = 0;
        while (!g_quit) {
            ULONG loop_t0 = eclk();
            poll_input();
            if (keydown[0x50] && !f1_prev) max_skip = (max_skip + 1) & 3;  /* F1 */
            f1_prev = keydown[0x50];
            if (!g_pause) {
                int due = cad;
                if (due > 1 + max_skip) due = 1 + max_skip;
                ULONG mach_acc = 0, aud_acc = 0;
                for (int i = 0; i < due; i++) {
                    ULONG ta = eclk();
                    cc_machine_run_frame();
                    ULONG tb = eclk();
                    /* refill Paula after EVERY guest frame: with catch-up the
                     * display loop can span 2-3 guest frames, and one refill
                     * per loop let the ring lead drain = jerky sound. */
                    cc_audio_amiga_frame();
                    ULONG tc = eclk();
                    mach_acc += eclk_delta(tb, ta);
                    aud_acc  += eclk_delta(tc, tb);
                }
                ULONG t2 = eclk();
                cc_render_frame(game, CC_W);
                ULONG t3 = eclk();
                present_game();
                /* cadence controller: busy time vs the due*per budget */
                {
                    ULONG busy = eclk_delta(eclk(), loop_t0);
                    ULONG budget = per * (ULONG)due;
                    if (busy > budget + per / 8) {
                        under_run = 0;
                        if (++over_run >= 12 && cad < 1 + max_skip) { cad++; over_run = 0; }
                    } else if (due >= 2 && busy + per < budget) {
                        over_run = 0;
                        if (++under_run >= 90 && cad > 1) { cad--; under_run = 0; }
                    } else { over_run = 0; under_run = 0; }
                }
                if (hud) {
                    ULONG t4 = eclk();
                    pt_mach += mach_acc; pt_aud += aud_acc;
                    pt_rend += eclk_delta(t3, t2); pt_pres += eclk_delta(t4, t3);
                    pgame += due;
                    if (++pdisp >= 50) {
                        draw_perfhud(eclk_delta(t4, pt_start), pdisp, pgame, cad,
                                     pt_mach, pt_aud, pt_rend, pt_pres);
                        pt_mach = pt_aud = pt_rend = pt_pres = 0;
                        pdisp = pgame = 0;
                        pt_start = eclk();
                    }
                }
            }
            frame_pace_n(g_pause ? 1 : cad);   /* pace the whole loop to cad frames */
        }
    }
    shutdown();
    return ok ? 0 : 1;
}
