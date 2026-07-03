/* cc_state.h -- shared video state between cc_machine.c (writes) and cc_render.c (reads). */
#ifndef CC_STATE_H
#define CC_STATE_H
#include <stdint.h>

/* video RAM the emulated Taito chips read (owned by cc_machine.c) */
extern uint8_t  cc_scn[0x14000];     /* TC0100SCN ram */
extern uint8_t  cc_scnctl[0x10];     /* TC0100SCN ctrl */
extern uint8_t  cc_road[0x2000];     /* TC0150ROD ram */
extern uint8_t  cc_spr[0x800];       /* spriteram */
extern uint16_t cc_pal[0x1000];      /* TC0110PCR palette (raw XBGR555) */
extern int      cc_pal_dirty;        /* palette write flag; presenter clears */
extern int      cc_road_palbank;     /* contcirc_out_w bits 6-7 */

/* gfx ROMs (cc_romdata.s) */
extern uint8_t cc_gfx_scn[];   /* 0x80000  TC0100SCN tiles */
extern uint8_t cc_gfx_road[];  /* 0x80000  TC0150ROD road gfx */
extern uint8_t cc_gfx_spr[];   /* 0x200000 assembled sprite gfx */
extern uint8_t cc_gfx_spr2[];  /* 0x200000 assembled sprite gfx, bank B */
extern uint8_t cc_gfx_smap[];  /* 0x80000  spritemap (LE words) */
extern uint8_t cc_rom_motor[]; /* 0x08000  Chase H.Q. motor CPU ROM, stubbed */
extern uint8_t cc_proms[];     /* priority/control PROM reference blob */

/* fixed display palette (cc_romdata.s): 256*3 RGB bytes + 32768-entry nearest LUT
 * (legacy xRGB555 -> slot, retained for old 8-bit paths). */
extern uint8_t cc_pal256[];   /* 256 * 3 (R,G,B) */
extern uint8_t cc_lut32k[];   /* 32768 bytes, legacy palette slot 0..255 */

/* machine API (cc_machine.c) */
void cc_machine_init(void);
void cc_machine_run_frame(void);
void cc_set_inputs(uint8_t in0, uint8_t in1, uint8_t dswa, uint8_t dswb, int steer);
void cc_machine_debug(unsigned *main_pc, unsigned *sub_pc, int *sub_on, unsigned *ctrl_count, unsigned *ctrl_last);
void cc_machine_debug_cpu(unsigned *main_sr, unsigned *sub_sr, unsigned *main_cycles, unsigned *sub_cycles);
void cc_machine_debug_main_regs(unsigned regs[4]);
void cc_machine_debug_irq(unsigned *main_irqs, unsigned *sub_irqs);
void cc_machine_debug_tasks(unsigned *active, uint16_t first_words[8]);
void cc_machine_debug_bus(uint16_t *shared_1a, uint16_t *shared_1e, uint16_t *shared_a6,
                          uint16_t *sub_0802, uint16_t *sub_0804, uint16_t *sub_080a,
                          uint16_t *sub_1a58, uint16_t *sub_1a5a);
uint16_t cc_machine_debug_main_word(unsigned off);
uint32_t cc_machine_debug_main_long(unsigned off);
void cc_machine_debug_ioc(uint8_t *port, unsigned counts[16]);
void cc_machine_coverage_dump(const char *path);
uint8_t  cc_bus_read8(unsigned cpu, unsigned addr);
uint16_t cc_bus_read16(unsigned cpu, unsigned addr);
uint32_t cc_bus_read32(unsigned cpu, unsigned addr);
void     cc_bus_write8(unsigned cpu, unsigned addr, unsigned data);
void     cc_bus_write16(unsigned cpu, unsigned addr, unsigned data);
void     cc_bus_write32(unsigned cpu, unsigned addr, unsigned data);

/* sound API (cc_audio.c) -- Z80 audiocpu + TC0140SYT mailbox + YM2610 */
/* Output sample rate, shared by the YM synth (cc_audio.c) and Paula playback
 * (cc_audio_amiga.c).  11025: the fm.c YM2610 synth measured 82% of wall time
 * at 22050 on the Amiberry target -- halving the rate is the speed lever; the
 * OPN core rate-converts internally so pitch/tempo are unaffected. */
#define CC_SND_SR 11025
extern uint8_t cc_rom_audio[];        /* 0x10000 audiocpu (cc_romdata.s) */
extern uint8_t cc_adpcma[];           /* 0x180000 YM2610 ADPCM-A samples */
extern uint8_t cc_adpcmb[];           /* 0x080000 YM2610 ADPCM-B samples */
void    cc_audio_init(void);
void    cc_audio_run_frame(void);
void    cc_audio_run_cycles(int total);
void    cc_audio_render(signed char *out, int n);   /* mono signed-8 PCM @ CC_SND_SR */
void    cc_audio_amiga_open(void);                   /* Paula setup (cc_audio_amiga.c) */
void    cc_audio_amiga_frame(void);                  /* per-frame Paula feed */
void    cc_audio_amiga_close(void);                  /* stop Paula on exit */
void    cc_audio_amiga_pause(int on);                /* mute/unmute during pause */
void    cc_audio_reset_line(int clear_line);          /* TC0140SYT/Z80 reset: 1=run, 0=hold */
extern uint8_t cc_bezel_data[];   /* 320x256 AGA bezel chunky (cc_romdata.s) */
extern uint8_t cc_bezelpal[];     /* 16 x RGB bezel palette (slots 240-255) */
void    cc_syt_master_port_w(uint8_t d);   /* sub 0x200001 write */
void    cc_syt_master_comm_w(uint8_t d);   /* sub 0x200003 write */
uint8_t cc_syt_master_comm_r(void);        /* sub 0x200003 read  */

/* renderer API (cc_render.c) -- fills a 320x224 buffer of 12-bit palette PENS
 * (index into cc_pal). The presenter maps pens -> screen colors (adaptive CLUT
 * on 8-bit, or direct truecolor). */
void cc_render_frame(uint16_t *out, int stride);
/* big zeroed allocation for render caches; NULL ok (caches degrade gracefully).
 * Amiga: AllocMem FAST->ANY (cc_rtg_main.c); host: calloc (cc_hosttest.c). */
void *cc_big_alloc(unsigned long n);
#define CC_W 320
#define CC_H 240   /* MAME chasehq visible area = 320x240 (set_raw vdisp 16..256); 224 cropped the bottom (dashboard/car) */

#endif
