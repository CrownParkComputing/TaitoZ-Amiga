# Step 03 — video decode (TC0100SCN tilemaps + TC0110PCR palette)

Status: **BG0 + BG1 + TX + palette + TC0150ROD road VALIDATED vs MAME oracle.**
The host-emulated attract frame renders as the correct Continental Circus scene
(sky/clouds, coastal bay, forested hills, "© 1987 TAITO CORPORATION JAPAN /
ALL RIGHTS RESERVED"). The road renders with pixel-perfect geometry (S-curve
matches MAME exactly), correct red/white kerbs and white centre dashes. Sprites
(hardware zoom) are the remaining video TODO. Proof images: `proof_*.png`.

## TC0150ROD road — ported from MAME (don't reinvent the wheel)

The road is a **faithful port of MAME `taito/tc0150rod.cpp::draw()`**
(BSD-3-Clause, © Nicola Salmoria), in `render.c::draw_road()`. No standalone repo
exists; FBNeo's road is a port of the same code. contcirc call params:
`type=1, y_offs=-3, palette_offs=road_palbank<<6, road_trans=0`. Road RAM
@0x300000 (per-line A/B clip/body/colbank words, control @word 0xfff) + road gfx
ROM `gfx_road.bin` (2bpp, 0x200 B/tile = edge line + body line). Drawn between BG
and TX. `road_palbank=3` for contcirc attract (from `out_w` 0x090001 bits 6-7).
Output: 320x224 `road_layer` (0x8000 = transparent), composited over BG, under TX.

Note: road *body* color varies by stage (Continental Circus tracks are different
countries — grey/yellow/etc. roads); validated via geometry + kerbs + complete
internally-consistent frames, since MAME's `video:snapshot()` frame doesn't
reliably sync with the memory-dump frame (it lags / lands on attract scene-cuts).

## Tools
- `render.c` → `./render [gx gy]` — decodes captured state to PPMs:
  `bg0.ppm bg1.ppm tx.ppm` (full 512x512 maps) + `frame.ppm` (320x224 composite).
  State path via `STATE=` env (default `../02-host-ref/out/state/`; oracle = `./oracle_`).
  Shared decode logic — destined for the Amiga renderer (`game_render.c`).
- `contcirc_dump.lua` — MAME oracle dumper. Run:
  `cd steps/03-video-decode && DUMP_FRAME=600 mame contcirc -rompath ~/Downloads \
   -autoboot_script contcirc_dump.lua -video soft -window -nothrottle`
  → `oracle_{scn_ram,scn_ctrl,road_ram,spriteram,palette}.bin` + `oracle_road_palbank.txt`.
  Palette is written **little-endian** to match the host dump + renderer (a BE bug
  here caused byte-swapped colors / speckled clouds — fixed). road_palbank is
  captured via a write-tap on out_w (0x090001). NOTE: `video:snapshot()` does not
  reliably grab the same frame as the memory dump — use the dumped `oracle_*.bin`
  as ground truth (render them), not the PNG snapshot.

## Decode reference (from MAME tc0100scn.cpp / tc0110pcr.cpp / taito_z_v.cpp)

TC0100SCN RAM (byte offsets, single-width):
```
0x0000  BG0 tilemap   64x64, 2 words/tile: attr=w[2i], code=w[2i+1]
0x4000  TX  tilemap   64x64, 1 word/tile:  a=w[i]
0x6000  TX char-gen   256 chars, 8x8 2bpp, 16 B/char
0x8000  BG1 tilemap   (same format as BG0)
0xc000  BG0 rowscroll / 0xc400 BG1 rowscroll
```
- BG color = `attr & 0xff` (contcirc sets no colbank); flip = `attr>>14`.
  palette index = `color*16 + pix`. Tile gfx = `gfx_8x8x4_packed_msb`
  (gfx_scn.bin, 32 B/tile, hi nibble = left pixel).
- TX code = `a & 0xff`, color = `(a>>8)&0x3f`; palette index = `color*4 + pix`.
- Palette (TC0110PCR, m_shift=0): raw xRGB555 word →
  `R=pal5(d>>10) G=pal5(d>>5) B=pal5(d>>0)`, pal5(v)=(v<<3)|(v>>2). pix 0 = transparent.
- Composite order (screen_update_contcirc): bottom = BG[`(ctrl[6]>>3)&1`] OPAQUE,
  then the other BG, then **road** (TC0150ROD, palbank = ctrl-driven), then TX,
  then sprites. Scroll = `-ctrl[0..5]`.

## Validation method (the loop to reuse for road + sprites)

1. `contcirc_dump.lua` dumps MAME's authoritative VRAM at frame N.
2. Render the SAME `oracle_*` with `render.c` → proves the *decoder*.
3. Render the host capture (`02-host-ref/out/state`) at the same frame → proves
   the *emulated chips*. host@900 vs oracle@900 palette = 2713/4096 byte-identical
   (the rest drift because the host has no sound CPU yet, so attract timing differs
   slightly). Both render to the correct scene.

## Next
- TC0150ROD road: read tc0150rod.cpp; road RAM @0x300000 (per-line params) +
  gfx_road.bin; draw between BG and TX with `road_palbank<<6`.
- Sprites: contcirc_draw_sprites_16x8 in taito_z_v.cpp (hardware zoom + spritemap).
