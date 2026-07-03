# Continental Circus — Amiga port

Amiga port of Taito's **Continental Circus** (1987, Taito Z System / MAME
`contcirc`). Follows the same methodology as the other ports in this repo
(Gaplus / 1943 / Ikari Warriors): verify ROMs → build a C host-reference model
validated frame-by-frame against a MAME oracle → coverage-harden → ship an
Amiga RTG interpreter build.

## Why this title is a good fit

- **Guest CPU == host CPU.** Twin MC68000 @ 12 MHz on a 68k Amiga. Every prior
  port translated a *foreign* ISA (Z80/6809/V30) to 68k. Here the program ROMs
  are already 68000 — disassembly, a host-reference core, and (later) native
  execution are all in-family. This is the single biggest advantage.
- **Small code.** 256K main + 256K sub of 68k code. Fits the existing pipeline.
- **Shared board.** Taito Z (Chase HQ, SCI, Ninja Warriors, Aquajack, …) — the
  video-chip models built here are reusable across the family.

## The real difficulty (corrected from initial scoping)

The "3D" is **not** CPU-drawn pixels we can read out of a framebuffer. It is
three Taito custom video chips we must reimplement, plus hardware sprite zoom:

- **TC0150ROD** — the road. The 68000 writes per-scanline road parameters
  (position, curvature/priority, palette) into road RAM; the chip rasterises the
  road plane from the `gfx_road` ROM each line. This is the headline risk.
- **Sprite zoom** — sprites are scaled in hardware via a sprite list + the
  `spritemap` ROM (zoom/lookup), not pre-scaled blits.
- **TC0100SCN** — two scrolling BG tile layers + a text layer.
- **TC0110PCR** — palette / priority.

So the architecture is: emulate maincpu + sub (68000) + audiocpu (Z80) + YM2610,
reimplement the four video chips in a renderer, and validate against MAME. It is
*more* video hardware than 1943/Gaplus, not less.

## CPU strategy (in-family advantage)

Both phases reuse existing repo tooling:

- **Host reference / oracle:** Musashi (the repo's "validated host runner") or
  LinuxJedi's pure-Rust `m68k-rs` core (vendored at
  `../PacLand/tools/mc_m68k_oracle/m68k-rs-local`). Step 02 runs the real ROMs
  through one of these in C/Rust on Linux and diff-checks video RAM + frames
  against MAME.
- **Amiga target:** the current playable baseline is an RTG interpreter build:
  Musashi runs both 68000s, the shared renderer paints the Taito video chips,
  and the presenter writes full RGB888 pixels to a Picasso96/CyberGraphX screen.
  Native 68k execution remains the later performance route.

Open decisions (see "Decisions to lock" below) — these do not block steps 01–02.

## Step plan

| Step | Dir | Status |
|------|-----|--------|
| 01 ROM analysis | `steps/01-rom-analysis` | ✅ done — 19/19 ROMs verified, flat images built, interleave confirmed via reset vectors |
| 02 host-ref model | `steps/02-host-ref` | ✅ CPUs done — dual MC68000 on Musashi boot through POST into the attract loop, 0 illegal ops, driving tilemap/road/sprite/palette RAM. Boot handshake + TC0040IOC reverse-engineered (`notes.md`). Amiga source now includes Z80/YM2610/Paula sound wiring. |
| 03 video decode | `steps/03-video-decode` | ✅ BG0+BG1+TX + palette + **TC0150ROD road** VALIDATED vs MAME oracle (road geometry pixel-perfect, correct kerbs/dashes; road = faithful port of MAME's BSD-3 `tc0150rod.cpp`). Amiga renderer includes `contcirc_draw_sprites_16x8`-style zoom sprites. |
| 04 cpu disasm/map | `steps/04-cpu-disasm` | annotate main/sub memory map, IRQ model, inter-CPU sync, IO map |
| 05 amiga build | `source/` | RTG Musashi dual-68000 build with software renderer, full-colour RGB888 painter, and YM2610/Paula audio. `source/build.sh` emits a clean dist set. Runtime tuning/validation remains. |

## Toolchain

- C cross: `m68k-amigaos-gcc` (bebbo) at `/home/jon/amiga-amigaos/bin`
  — `export PATH="/home/jon/amiga-amigaos/bin:$HOME/.local/bin:$PATH"`.
- ASM/link: `vasmm68k_mot`, `vlink` in `~/.local/bin`.
- Oracle: system `mame` (`mame contcirc`), `m68k-rs` Rust core, Musashi.
- Validation pattern: dump MAME frames to PPM (`*_oracle_NNN.ppm`) and diff
  against the host renderer, as in TigerHeli/Gaplus.

## Decisions to lock (before step 05; not blocking 02–03)

1. **Target machine** — stock A1200 (68020) is not realistic for dual-68000 +
   road at 60 Hz; this likely needs 68030/40/60 with RTG.
2. **CPU execution model on Amiga** — current baseline is interpreted Musashi;
   later options are static recompile + I/O trap shim, vs
   MMU-mapped direct execution (68030+), vs interpreted fallback for debug.
3. **Audio** — current build wires the Z80 reset line, TC0140SYT mailbox, and
   YM2610 FM/SSG/ADPCM-A/ADPCM-B mixer to a Paula-friendly 22.05 kHz stream.

## Dist

Run `source/build.sh` to rebuild the current RTG package. It emits:

- `dist/Continental Circus`
- `dist/Continental Circus.info`
- `dist/Continental Circus.zip`
- `dist/Continental Circus.hdf`
- `dist/Continental Circus.uae`

The HDF boots `ContCirc` directly, loads `uaegfx`, and opens an RTG screen. The UAE expects the installed copy at
`/home/jon/Amiberry/HardDrives/ContinentalCircus.hdf`.

Controls:

- CD32 red / joystick fire / Space / Ctrl: gas
- CD32 blue, yellow, or green / Left Alt / X: toggle gear. The RTG build shows a
  small `LO`/`HI` gear indicator in the game view.
- CD32 play / Return / 1: start
- CD32 shoulder / 5: coin
- Cursor left/right or joystick left/right: steer

## Status

The current baseline is a packaged RTG build using interpreted main/sub 68000s,
software road/tile/sprite rendering, full-colour RGB888 `WritePixelArray`
presentation, and YM2610 audio mixed to Paula at 22.05 kHz. The boot screen waits
for fire or start before launching the game. The current Amiga test build also
swaps the RTG red/blue byte store to correct blue-tinted output reported on the
target setup, uses MAME-style road/sprite priority masking, predecodes the
TC0150ROD road graphics for faster 3D road rendering, defaults to an
optimized 2x 640x448 game blit inside a 640x480 RTG screen, and runs
the dual-68000 interpreter with 8 frame slices to reduce Musashi context churn.
Remaining work is runtime validation and tuning. If this is still short of speed
on the target, the next step is native 68k execution/transcode; the interpreter
path has only minor gains left.
