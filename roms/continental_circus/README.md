# Continental Circus — ROMs

Arcade ROM data is **not** included in this repository.

Place your legally-obtained Continental Circus (Taito Z-System) ROM set in
this directory. The expected ROM files (as extracted by `cc_extract_roms.py`)
are:

- `maincpu.bin` — main 68000 program
- `sub.bin` — sub 68000 program
- `audiocpu.bin` — Z80 sound CPU program
- `gfx_scn.bin` — TC0100SCN tilemap graphics
- `gfx_road.bin` — TC0150ROD road graphics
- `sprites_asm.bin` / `spritemap_asm.bin` — sprite data
- `adpcma.bin` / `adpcmb.bin` — YM2610 ADPCM samples
- `cc_bezel.bin` / `cc_bezelpal.bin` — bezel overlay
- `cc_pal256.bin` / `cc_lut32k.bin` — palette / lookup tables

Run `cc_extract_roms.py` from the `continental_circus/` directory to extract
these from a MAME-format ZIP.