# Step 01 — ROM analysis (Continental Circus, World / `contcirc`)

Source: `roms/contcirc.zip` (MAME parent set, TORRENTZIP). Driver: `taito/taito_z.cpp`.

Run: `bash extract.sh && python3 assemble.py` — verifies every ROM CRC32
against MAME and builds flat images into `out/`. All 19 parent ROMs verified OK.

## Hardware (from `mame -listxml contcirc`)

| Part        | Device            | Clock   | Role |
|-------------|-------------------|---------|------|
| maincpu     | MC68000           | 12 MHz  | game logic, road math, object lists |
| sub         | MC68000           | 12 MHz  | sprite/road servicing, shares RAM with main |
| audiocpu    | Z80               | 4 MHz   | sound driver, talks to YM2610 |
| ymsnd       | YM2610 (OPNB)     | 8 MHz   | 4 FM ch + 3 SSG + ADPCM-A (6 ch) + ADPCM-B |
| TC0100SCN   | tilemap gen       | —       | 2 scroll BG layers + 1 text layer |
| TC0150ROD   | **road line gen** | —       | pseudo-3D road raster (the "3D") |
| TC0110PCR   | palette/color     | —       | palette RAM + priority |
| TC0140SYT   | sound comm        | —       | main68k <-> Z80 mailbox |
| TC0040IOC   | I/O               | —       | inputs / DSW / coin |

Display: 320x224, 60.056 Hz, no rotation.

## ROM regions and flat images

68000 program ROMs are 16-bit, loaded `ROM_LOAD16_BYTE`: the `offset 0` ROM
feeds the high byte (D15..8), `offset 1` feeds the low byte. `assemble.py`
interleaves them into a big-endian flat image the CPU sees linearly.

| out/ image     | size | built from (MAME region) | notes |
|----------------|------|--------------------------|-------|
| maincpu.bin    | 256K | b33-ww.ic25 (even) + b33-xx.ic26 (odd) | reset SSP=0x00083ffe PC=0x000008c6, first insn `ori #$0700,sr` ✓ |
| sub.bin        | 256K | b33-yy.ic35 (even) + cc_36.bin (odd)   | reset SSP=0x00083ffe PC=0x000008d2 ✓ |
| audiocpu.bin   |  64K | b33-30.11 (Z80)          | banked Z80 sound program |
| gfx_scn.bin    | 512K | b33-02.57                | TC0100SCN tiles (8x8, 4bpp) — decode in step 03 |
| gfx_road.bin   | 512K | b33-01.3                 | TC0150ROD road gfx/line data — decode in step 03 |
| spritemap.bin  | 512K | b33-07.64                | sprite zoom/lookup map |
| sprites.bin    |   2M | b33-06,05,04,03 @0,2,4,6 | 16x8 / 16x16 zoomable sprites, 4bpp — decode in step 03 |
| adpcma.bin     |   1M | b33-09.18 + b33-10.17    | YM2610 ADPCM-A samples |
| adpcmb.bin     | 512K | b33-08.19                | YM2610 ADPCM-B samples |
| proms.bin      | 512B | b33-17.16 + b33-18.17    | color/priority PROMs |
| user2.bin      |  72K | b14-30.97 + b14-31.50    | shared Taito Z tables (road/zoom) |

The reset-vector check (`ori #$0700,sr` on main, sane SSP/PC on both) confirms
the big-endian interleave is correct — the CPU images are ready for
disassembly (step 02) and for the host-reference 68000 core (step 02b/03).

## Important correction vs. the initial scoping note

The road is **not** pure 68k arithmetic rendered into a CPU framebuffer that we
read back. Continental Circus draws through dedicated Taito custom silicon:

- **TC0150ROD** renders the road from per-scanline parameters the 68000 writes
  into road RAM, sampling the `gfx_road` ROM. We must reimplement this chip.
- Sprites are **hardware-zoomed** via a sprite list + `spritemap` lookup, not
  pre-scaled blits. We reimplement the zoom.
- **TC0100SCN** generates the two BG layers + text layer with per-layer scroll.

So this is the same shape as the other ports in this repo: emulate the CPUs,
reimplement the video chips in the renderer, validate frames against a MAME
oracle. It is *more* video hardware than 1943/Gaplus, not less.
