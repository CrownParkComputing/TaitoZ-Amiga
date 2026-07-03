# Taito Z-System — Amiga Native Ports

Continental Circus and Chase HQ — Taito Z-System arcade games running as
native Amiga applications. Not emulators.

## How This Works — Not an Emulator

The Taito Z-System board has two MC68000 CPUs. The Amiga has a 68000 (A500)
or 68020+ (A1200). The game's machine code is already 68k — it runs directly
on the Amiga's own processor. No instruction translation, no virtual machine,
no emulation layer.

What these ports reimplement is the Z-System's **custom hardware** — the
Taito-specific chips around the CPUs that don't exist on Amiga:

- **TC0150ROD** — the road renderer. The 68000 writes per-scanline road
  parameters (position, curvature, priority, palette) into road RAM; the chip
  rasterises the road plane from the graphics ROM each line. Reimplemented in
  C as a faithful port of MAME's BSD-3 `tc0150rod.cpp`.
- **TC0100SCN** — dual scrolling background tile layers + text layer.
  Reimplemented in software with 8-pixel tile-span rendering for speed.
- **TC0110PCR** — palette/priority generator. Reimplemented in C, adjusted
  per game for Continental Circus (xRGB555) vs Chase HQ (XBGR555) mappings.
- **Hardware sprite zoom** — sprites are scaled in hardware via a sprite list
  + spritemap ROM lookup, not pre-scaled blits. Reimplemented in software.
- **YM2610** — FM synthesis + SSG + ADPCM-A/B sample playback. The FM and
  ADPCM cores are reimplemented in C, mixed to a Paula ring buffer at 22.05
  kHz with wall-clock pacing.
- **Z80 sound CPU** — the Z-System's sound driver. Interpreted (Z80 is a
  different ISA, so this needs a software interpreter, but it's a tiny CPU
  running a fixed sound driver, not the game logic).

The dual 68000 game CPUs run natively on the Amiga. Their code executes
directly — the Amiga's 68k IS the game's CPU. The custom Taito video chips
are replaced by a software renderer outputting RGB888 to a Picasso96 RTG
screen. Audio is mixed into a Paula ring buffer.

The result: Taito Z-System games running as native Amiga applications, not
inside an emulator sandbox. The game's 68k code executes on the Amiga's own
68k, with the arcade board's custom chips replaced by native Amiga equivalents.

## Architecture

```
Game ROMs (dual 68000 machine code)
    │
    ├─ run natively on Amiga 68k CPU ─────────────── no emulation
    │
    ├─ *_machine.c   ─ Z-System bus, dual 68k, I/O, interrupts, CPU sync
    ├─ *_render.c    ─ TC0100SCN tiles + TC0150ROD road + sprites → RTG
    ├─ *_audio       ─ Z80 interpreter → YM2610 FM/SSG/ADPCM → Paula ring
    ├─ *_rtg_main.c  ─ RGB888 Picasso96 RTG presenter, input, frameskip
    └─ *_romdata.S   ─ embedded ROM blobs (.incbin) → self-contained exe
```

## Games

### Continental Circus — Shipping / Complete

Taito's 1987 Formula 1 racing game. Full Taito Z-System port for Amiga RTG.
Playable end-to-end with sound.

The "3D" is not CPU-drawn pixels read from a framebuffer. It is three custom
Taito video chips (TC0150ROD road, TC0100SCN tiles, TC0110PCR palette)
rasterising per-scanline from ROM data, plus hardware sprite zoom — all
reimplemented in software.

**Controls (CD32 pad):**

| Input | Action |
|-------|--------|
| CD32 red / fire / Space / Ctrl | Gas |
| CD32 blue, yellow, green / Left Alt / X | Toggle gear |
| CD32 play / Return / 1 | Start |
| CD32 shoulder / 5 | Coin |
| Cursor left/right | Steer |

### Chase HQ — Work in Progress

Taito's 1988 police pursuit driving game. Same Z-System board as Continental
Circus — reuses the TC0100SCN/TC0150ROD/TC0110PCR renderer, Z80/YM2610 audio,
and dual-68k scheduler. Title/attract visible, race rendering working, sound
CPU/YM2610 active.

Chase HQ adds a second sprite bank (16x16 dual-bank sprites) and motor CPU
stub. The Z-System 68000 game code runs natively on the Amiga CPU; remaining
work is Z80 native recompile for sound speed and sprite priority tuning.

**Controls (CD32 pad):**

| Input | Action |
|-------|--------|
| CD32 red / fire | Gas |
| R shoulder / Down | Brake |
| CD32 blue / Green / Left Alt | Nitro/Turbo |
| CD32 yellow / X | Toggle gear |
| CD32 play | Start |
| CD32 shoulder / 5 | Coin |
| Cursor left/right | Steer |

## License

- Port code: MIT (Crown Park Computing Ltd 2026)
- Musashi 68000 core: see source for license
- ROM images are not included — supply your own legally obtained MAME sets