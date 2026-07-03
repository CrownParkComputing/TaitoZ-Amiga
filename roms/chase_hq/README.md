# Chase HQ — ROMs

Arcade ROM data is **not** included in this repository.

Place your legally-obtained Chase HQ (Taito Z-System) ROM set in this
directory. The expected ROM files (as extracted by `chq_extract_roms.py`)
are:

- `maincpu.bin` — main 68000 program
- `sub.bin` — sub 68000 program
- `audiocpu.bin` — Z80 sound CPU program
- `motorcpu.bin` — motor CPU program
- `gfx_scn.bin` — TC0100SCN tilemap graphics
- `gfx_road.bin` — TC0150ROD road graphics
- `sprites_asm.bin` / `sprites2_asm.bin` / `spritemap_asm.bin` — sprite data
- `adpcma.bin` / `adpcmb.bin` — YM2610 ADPCM samples
- `proms.bin` — PROMs
- `cc_bezel.bin` / `cc_bezelpal.bin` — bezel overlay
- `cc_pal256.bin` / `cc_lut32k.bin` — palette / lookup tables

Run `chq_extract_roms.py` from the `chase_hq/source_v1/` directory to extract
these from a MAME-format ZIP.