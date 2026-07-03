/*
 * sci_live_main.c -- S.C.I. v2 Amiga RTG runtime (Power Drift architecture).
 *
 * Runs the MAME-oracle-validated dual-68000 machine model (sci_machine) and the
 * span renderer (sci_render) in-process, presenting through an 8-bit RTG chunky
 * blit: the live TC0110PCR palette is mapped to a fixed RGB332 256-pen screen
 * CLUT (Power Drift / Side Arms method), the 320x224 game doubled to a
 * borderless 640x480 screen.  Sound is the Z80+YM2610
 * board on Paula (sci_audio + sci_audio_amiga, wall-clock ring).
 *
 * Scheduling is the Power Drift wall-clock model: GAME time holds a true 60Hz;
 * when a display frame overruns, up to 1+max_skip guest frames run before the
 * single render+present.  F1 cycles max_skip 0..3.
 *
 * ROMs are embedded (sci_romdata.S) -- a single self-contained binary.
 */
#include <exec/types.h>
#include <exec/memory.h>
#include <graphics/gfxbase.h>
#include <graphics/modeid.h>
#include <intuition/intuition.h>
#include <intuition/screens.h>
#include <devices/timer.h>
#include <dos/dos.h>
#include <proto/exec.h>
#include <proto/graphics.h>
#include <proto/intuition.h>
#include <proto/dos.h>
#include <proto/timer.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "sci_state.h"

/* WriteChunkyPixels is graphics.library V40+ (proto/graphics.h) */

struct IntuitionBase *IntuitionBase;
struct GfxBase *GfxBase;
struct Device *TimerBase;
struct Library *CyberGfxBase;

/* cybergraphics BestCModeIDTagList: the reliable way to get a real uaegfx RTG
 * mode.  graphics BestModeID alone can hand back a native PLANAR AGA mode, and
 * WriteChunkyPixels onto a planar screen does a per-frame C2P = slideshow (and
 * the stalled loop starves the audio ring = silence). */
#define CYBRBIDTG_Depth         (0x80050000UL + 0)
#define CYBRBIDTG_NominalWidth  (0x80050000UL + 1)
#define CYBRBIDTG_NominalHeight (0x80050000UL + 2)
#define CGX_INVALID_ID          0xffffffffUL
static ULONG cgx_BestCModeIDTagList(struct TagItem *taglist)
{
    register struct Library *a6 __asm("a6") = CyberGfxBase;
    register struct TagItem *a0 __asm("a0") = taglist;
    register ULONG d0 __asm("d0");
    __asm volatile("jsr -60(%%a6)" : "=r"(d0) : "r"(a6), "r"(a0) : "cc", "memory");
    return d0;
}

static FILE *logf;
static void log_msg(const char *s)
{
    if (logf) { fputs(s, logf); fputc('\n', logf); fflush(logf); }
}

#define RTG_W GAME_W
#define RTG_H GAME_H
#define SCALE 2
#define GAME_W (CC_W * SCALE)          /* 640 */
#define GAME_H (CC_H * SCALE)          /* 448 */
#define GAME_OX 0
#define GAME_OY 0
/* Amiberry's RTG display clips the last couple of screen rows; shrink the game
 * a few px (bottom strip stays black) so the arcade's very bottom line (turbo/
 * dashboard) stays visible. Tunable; matches the Chase H.Q. fix. */
#define GAME_BOTTOM_SAFE 20
#define GAME_VH (GAME_H - GAME_BOTTOM_SAFE)

#define SCI_DRIVE_LOCK_MAX_FRAMES 90
#define SCI_DRIVE_READY_STREAK 6

/* ---- raw input (family-standard CD32/joystick read) ---- */
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
#define RK_F1 0x50
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

static struct Screen *scr;
static struct Window *win;
static struct MsgPort *timer_port;
static struct timerequest *timer_io;
static uint16_t *game;             /* CC_W*CC_H arcade pens from sci_render */
static uint8_t *chunky;            /* RTG_W*RTG_H 8-bit framebuffer */
static uint8_t pen8[0x1000];       /* arcade pen -> RGB332 screen pen */
static ULONG loadrgb[1 + 256 * 3 + 1];
static ULONG frame_ticks, next_tick;
static uint8_t keydown[128];
static int g_quit, g_pause, ok, g_gear_high, g_turbo_active, g_nitro_stock = 3, g_nitro_flash;

/* ---- timing ---- */
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

/* clamp Amiberry's occasionally-backwards EClock (negative delta = ~4e9) */
static ULONG eclk_delta(ULONG later, ULONG earlier)
{
    ULONG d = later - earlier;
    return ((LONG)d < 0) ? 0 : d;
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
    /* NOTE: no floating-pad rejection here -- v1 semantics exactly.  The pdrift
     * ">=5 bits = floating" heuristic zeroed real pads on this setup. */
    return out;
}

/* ---- display ---- */

/* canonical RGB332 screen palette: pen P displays the colour P encodes */
static void make_palette(void)
{
    loadrgb[0] = 256UL << 16;
    for (int i = 0; i < 256; i++) {
        unsigned r = (unsigned)(i & 0xe0);
        unsigned g = (unsigned)((i & 0x1c) << 3);
        unsigned b = (unsigned)((i & 0x03) << 6);
        r |= r >> 3 | r >> 6;
        g |= g >> 3 | g >> 6;
        b |= b >> 2 | b >> 4 | b >> 6;
        loadrgb[1 + i * 3 + 0] = r * 0x01010101UL;
        loadrgb[1 + i * 3 + 1] = g * 0x01010101UL;
        loadrgb[1 + i * 3 + 2] = b * 0x01010101UL;
    }
    loadrgb[1 + 256 * 3] = 0;
}

/* live TC0110PCR XBGR555 palette -> RGB332 pens; gated by cc_pal_dirty */
static void rebuild_pen8(void)
{
    if (!cc_pal_dirty) return;
    cc_pal_dirty = 0;
    for (int i = 0; i < 0x1000; i++) {
        unsigned d = cc_pal[i] & 0x7fff;
        unsigned r = (d >> 2) & 0x07;          /* 5-bit R -> 3 */
        unsigned g = (d >> 7) & 0x07;          /* 5-bit G -> 3 */
        unsigned b = (d >> 13) & 0x03;         /* 5-bit B -> 2 */
        pen8[i] = (uint8_t)((r << 5) | (g << 2) | b);
    }
}

/* 320x224 pens -> centred 2x chunky; word-doubled stores + row memcpy (pdrift) */
static void present_frame(void)
{
    rebuild_pen8();
    for (int dy = 0; dy < GAME_VH; dy++) {          /* map into GAME_VH so a bottom strip stays clear */
        int sy = (dy * CC_H) / GAME_VH;
        const uint16_t *src = game + sy * CC_W;
        uint8_t *row0 = chunky + (size_t)(GAME_OY + dy) * RTG_W + GAME_OX;
        uint32_t *d32 = (uint32_t *)row0;
        for (int x = 0; x < CC_W; x += 2) {
            unsigned p0 = pen8[src[x] & 0xfff];
            unsigned p1 = pen8[src[x + 1] & 0xfff];
            d32[x >> 1] = (p0 << 24) | (p0 << 16) | (p1 << 8) | p1;
        }
    }
    WriteChunkyPixels(win->RPort, GAME_OX, GAME_OY,
                      GAME_OX + GAME_W - 1, GAME_OY + GAME_H - 1,
                      chunky + (size_t)GAME_OY * RTG_W + GAME_OX, RTG_W);
}

static void gfx_text(int x, int y, int pen, const char *s)
{
    SetAPen(win->RPort, pen);
    SetBPen(win->RPort, 0);
    SetDrMd(win->RPort, JAM1);
    Move(win->RPort, x, y);
    Text(win->RPort, (CONST_STRPTR)s, strlen(s));
}

/* HUD: game fps, display fps, skip cap, subsystem % of wall time */
static void draw_perfhud(ULONG elapsed, int disp_frames, int game_frames, int skip,
                         ULONG tm, ULONG ta, ULONG tr, ULONG tp)
{
    char line[96];
    ULONG rate = frame_ticks * 60;
    ULONG e100 = elapsed / 100;
    if (!elapsed || !rate || !e100) return;
    {
        ULONG e10 = elapsed / 10;
        ULONG g10 = ((ULONG)game_frames * rate) / e10;
        ULONG d10 = ((ULONG)disp_frames * rate) / e10;
        sprintf(line, "G%lu.%lu D%lu.%lu S%ld M%lu A%lu R%lu P%lu   ",
                (unsigned long)(g10 / 10), (unsigned long)(g10 % 10),
                (unsigned long)(d10 / 10), (unsigned long)(d10 % 10), (long)skip,
                (unsigned long)(tm / e100), (unsigned long)(ta / e100),
                (unsigned long)(tr / e100), (unsigned long)(tp / e100));
    }
    gfx_text(GAME_OX, 12, 0xfc, line);
}

/* solid rectangle via graphics.library (RGB332 identity CLUT: pen == colour) */
static void fill_rp(int x0, int y0, int x1, int y1, int pen)
{
    SetAPen(win->RPort, pen);
    RectFill(win->RPort, x0, y0, x1, y1);
}

/* Bottom-right GEAR indicator: a graphic HI/LO shifter (the active gear cell
 * lights up yellow). Nitro/turbo stock is already shown by the game itself, so
 * it is NOT duplicated here. Redrawn on top of the game rect every frame. */
/* Gear indicator: R1 = HIGH (top), L1 = LOW (bottom); the engaged gear lights up. */
static void draw_status(void)
{
    int hi = g_gear_high;
    const int gw = 66, ch = 16, gh = 2 * ch + 4;    /* wide enough for "R1 HIGH" */
    int rmargin = RTG_W - (GAME_OX + GAME_W);        /* bezel margin, if any */
    int x0, y0;

    if (g_nitro_flash) g_nitro_flash--;              /* keep flash timer bounded (not shown) */

    if (rmargin >= gw + 8) {                          /* margin available: sit off gameplay */
        x0 = GAME_OX + GAME_W + (rmargin - gw) / 2;
        y0 = GAME_VH - gh - 14;
    } else {                                          /* SCI: no margin, bottom-right inside */
        x0 = GAME_OX + GAME_W - gw - 6;
        y0 = GAME_VH - gh - 6;
    }

    fill_rp(x0 - 2, y0 - 2, x0 + gw + 1, y0 + gh + 1, 0xff);          /* white frame */
    fill_rp(x0, y0, x0 + gw, y0 + gh, 0x00);                          /* black bg    */
    fill_rp(x0 + 2, y0 + 2, x0 + gw - 2, y0 + 2 + ch, hi ? 0xfc : 0x24);   /* R1 HIGH */
    gfx_text(x0 + 5, y0 + ch - 2, hi ? 0x00 : 0x92, "R1 HIGH");
    fill_rp(x0 + 2, y0 + ch + 2, x0 + gw - 2, y0 + gh - 2, hi ? 0x24 : 0xfc); /* L1 LOW */
    gfx_text(x0 + 5, y0 + gh - 4, hi ? 0x92 : 0x00, "L1 LOW");
}

static int perfhud_enabled(void)
{
    return 0;
}

static int whitty_no_game_loader(void)
{
    char v[8];
    LONG n = GetVar((CONST_STRPTR)"WHITTY_NO_GAME_LOADER", (STRPTR)v, sizeof(v), 0);
    return n > 0 && v[0] != '0';
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
    SetRast(win->RPort, 0);
    gfx_text(RTG_W / 2 - 40, RTG_H / 2 - 40, 0xff, "S.C.I.");
    gfx_text(RTG_W / 2 - 130, RTG_H / 2 + 20, 0xff, "5 COIN  1 START  ESC EXIT  F1 SKIP CAP");
    gfx_text(RTG_W / 2 - 130, RTG_H / 2 + 34, 0xff, "RED GAS  BLUE NITRO  YELLOW GEAR");
    for (;;) {
        poll_keys();
        unsigned cd32 = read_cd32();
        int fire = !(CIAA_PRA & PORT1_FIRE) || keydown[RK_SPACE] || keydown[RK_LCTRL] ||
                   (cd32 & (CD32_RED | CD32_BLUE | CD32_YELLOW | CD32_GREEN));
        int start = keydown[RK_1] || keydown[RK_RETURN] || (cd32 & CD32_PLAY);
        int now = fire || start;
        if (keydown[RK_ESC]) { g_quit = 1; return; }
        if (((tick / 24) & 1) == 0)
            gfx_text(RTG_W / 2 - 70, RTG_H / 2 - 10, 0xff, "PRESS FIRE OR START");
        else
            gfx_text(RTG_W / 2 - 70, RTG_H / 2 - 10, 0x00, "                   ");
        if (tick > 30 && now && !prev) break;
        prev = now;
        tick++;
        frame_pace();
    }
    memset(keydown, 0, sizeof keydown);
    SetRast(win->RPort, 0);
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
    int brake = keydown[RK_DOWN];                      /* R1 freed for high gear */
    int turbo = keydown[RK_LALT] || (cd32 & (CD32_BLUE | CD32_GREEN));
    int gear_tog = keydown[RK_X] || (cd32 & CD32_YELLOW);   /* keyboard/yellow toggle fallback */
    if (!drive_enabled) brake = turbo = 0;
    if (drive_enabled) {                               /* R1 = HIGH gear, L1 = LOW gear (direct) */
        if (cd32 & CD32_RSHOULDER) gear_high = 1;
        if (cd32 & CD32_LSHOULDER) gear_high = 0;
        if (gear_tog && !gear_prev) gear_high ^= 1;
    }
    gear_prev = gear_tog;
    uint8_t in0 = 0xff, in1 = 0xff;
    int coin = keydown[RK_5] || ((cd32 & CD32_LSHOULDER) && !drive_enabled);  /* L1 coins pre-race only */
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
        drive_lock_frames = SCI_DRIVE_LOCK_MAX_FRAMES;
        drive_ready_frames = 0;
    }
    s1p = start;
    if (!drive_enabled && race_started && drive_lock_frames > 0) {
        if (race_video_active()) {
            if (++drive_ready_frames >= SCI_DRIVE_READY_STREAK) drive_enabled = 1;
        } else {
            drive_ready_frames = 0;
        }
        if (--drive_lock_frames == 0) drive_enabled = 1;
    }
    if (c1h) { in0 &= (uint8_t)~0x04; c1h--; }
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
    if (chunky) { FreeMem(chunky, (ULONG)RTG_W * RTG_H); chunky = 0; }
    if (game) { FreeMem(game, CC_W * CC_H * 2UL); game = 0; }
    if (CyberGfxBase) { CloseLibrary(CyberGfxBase); CyberGfxBase = 0; }
    if (GfxBase) { CloseLibrary((struct Library *)GfxBase); GfxBase = 0; }
    if (IntuitionBase) { CloseLibrary((struct Library *)IntuitionBase); IntuitionBase = 0; }
    if (logf) { fclose(logf); logf = 0; }
}

/* render-cache allocations (sci_render.c); freed at exit */
#define CC_BIG_MAX 4
static void *big_ptrs[CC_BIG_MAX];
static ULONG big_sizes[CC_BIG_MAX];
static int big_n;
void *cc_big_alloc(unsigned long n)
{
    void *p = AllocMem(n, MEMF_FAST | MEMF_CLEAR);
    if (!p) p = AllocMem(n, MEMF_ANY | MEMF_CLEAR);
    if (p && big_n < CC_BIG_MAX) { big_ptrs[big_n] = p; big_sizes[big_n] = n; big_n++; }
    return p;
}
static void big_free(void)
{
    while (big_n > 0) { big_n--; FreeMem(big_ptrs[big_n], big_sizes[big_n]); }
}

int main(void)
{
    IntuitionBase = (struct IntuitionBase *)OpenLibrary((CONST_STRPTR)"intuition.library", 39);
    GfxBase = (struct GfxBase *)OpenLibrary((CONST_STRPTR)"graphics.library", 39);
    CyberGfxBase = OpenLibrary((CONST_STRPTR)"cybergraphics.library", 0);
    if (!IntuitionBase || !GfxBase) { shutdown(); return 20; }
    logf = fopen("PROGDIR:sci_live.log", "w");
    log_msg("sci start");
    open_timer();

    game = (uint16_t *)AllocMem(CC_W * CC_H * 2UL, MEMF_ANY | MEMF_CLEAR);
    chunky = (uint8_t *)AllocMem((ULONG)RTG_W * RTG_H, MEMF_ANY | MEMF_CLEAR);
    if (!game || !chunky) { shutdown(); return 21; }
    make_palette();

    {
        ULONG mode = CGX_INVALID_ID;
        if (CyberGfxBase) {
            struct TagItem tags[4];
            tags[0].ti_Tag = CYBRBIDTG_NominalWidth;  tags[0].ti_Data = RTG_W;
            tags[1].ti_Tag = CYBRBIDTG_NominalHeight; tags[1].ti_Data = RTG_H;
            tags[2].ti_Tag = CYBRBIDTG_Depth;         tags[2].ti_Data = 8;
            tags[3].ti_Tag = TAG_DONE;                tags[3].ti_Data = 0;
            mode = cgx_BestCModeIDTagList(tags);
        }
        if (mode == CGX_INVALID_ID || mode == INVALID_ID)
            mode = BestModeID(BIDTAG_NominalWidth, RTG_W,
                              BIDTAG_NominalHeight, RTG_H,
                              BIDTAG_Depth, 8, TAG_DONE);
        {
            char b[64];
            sprintf(b, "mode id %08lx (cgx %ld)", (unsigned long)mode,
                    (long)(CyberGfxBase != 0));
            log_msg(b);
        }
        if (mode == INVALID_ID || mode == CGX_INVALID_ID) { log_msg("FAIL no RTG mode"); shutdown(); return 23; }
        scr = OpenScreenTags(0,
                             SA_DisplayID, mode,
                             SA_Width, RTG_W, SA_Height, RTG_H, SA_Depth, 8,
                             SA_Quiet, TRUE, SA_ShowTitle, FALSE, TAG_END);
    }
    if (!scr) { log_msg("FAIL OpenScreen"); shutdown(); return 23; }
    {
        char b[64];
        sprintf(b, "screen %ldx%ld depth %ld", (long)scr->Width, (long)scr->Height,
                (long)scr->RastPort.BitMap->Depth);
        log_msg(b);
    }
    LoadRGB32(&scr->ViewPort, loadrgb);

    win = OpenWindowTags(0, WA_CustomScreen, (ULONG)scr, WA_Left, 0, WA_Top, 0,
                         WA_Width, RTG_W, WA_Height, RTG_H,
                         WA_Backdrop, TRUE, WA_Borderless, TRUE, WA_Activate, TRUE,
                         WA_RMBTrap, TRUE, WA_IDCMP, IDCMP_RAWKEY, TAG_END);
    if (!win) { shutdown(); return 24; }
    ScreenToFront(scr);
    ActivateWindow(win);
    log_msg("window open");

    if (!whitty_no_game_loader())
        run_loader();
    log_msg("loader skipped/done");
    if (g_quit) { shutdown(); return 0; }

    cc_machine_init();
    log_msg("machine init");
    cc_audio_init();
    log_msg("audio init");
    cc_audio_amiga_open();
    log_msg("paula open");
    ok = 1;

    {
        int hud = perfhud_enabled() && TimerBase != 0;
        int max_skip = 2, f1_prev = 0;
        ULONG per = frame_ticks ? frame_ticks : 1;
        ULONG sched_last = eclk(), carry = 0;
        ULONG pt_mach = 0, pt_aud = 0, pt_rend = 0, pt_pres = 0, pt_start = eclk();
        int pdisp = 0, pgame = 0;
        while (!g_quit) {
            poll_input();
            if (keydown[RK_F1] && !f1_prev) max_skip = (max_skip + 1) & 3;
            f1_prev = keydown[RK_F1];
            if (!g_pause) {
                ULONG now = eclk();
                ULONG dt = eclk_delta(now, sched_last) + carry;
                int due = (int)(dt / per);
                sched_last = now;
                carry = dt % per;
                if (due < 1) due = 1;
                if (due > 1 + max_skip) { due = 1 + max_skip; carry = 0; }
                ULONG mach_acc = 0, aud_acc = 0;
                for (int i = 0; i < due; i++) {
                    ULONG ta = eclk();
                    cc_machine_run_frame();
                    ULONG tb = eclk();
                    cc_audio_amiga_frame();
                    ULONG tc = eclk();
                    mach_acc += eclk_delta(tb, ta);
                    aud_acc  += eclk_delta(tc, tb);
                }
                ULONG t2 = eclk();
                cc_render_frame(game, CC_W);
                ULONG t3 = eclk();
                present_frame();
                draw_status();
                if (hud) {
                    ULONG t4 = eclk();
                    pt_mach += mach_acc; pt_aud += aud_acc;
                    pt_rend += eclk_delta(t3, t2); pt_pres += eclk_delta(t4, t3);
                    pgame += due;
                    if (++pdisp >= 50) {
                        draw_perfhud(eclk_delta(t4, pt_start), pdisp, pgame, max_skip,
                                     pt_mach, pt_aud, pt_rend, pt_pres);
                        pt_mach = pt_aud = pt_rend = pt_pres = 0;
                        pdisp = pgame = 0;
                        pt_start = eclk();
                    }
                }
            } else {
                sched_last = eclk();
                carry = 0;
            }
            frame_pace();
        }
    }
    big_free();
    shutdown();
    return ok ? 0 : 1;
}
