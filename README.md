# Taito Z-System Amiga RTG Ports

Continental Circus and Chase HQ — Taito Z-System arcade ports for Amiga RTG.

Both games run on the same Taito Z-System arcade board: dual MC68000 CPUs,
Z80 sound CPU, YM2610 FM/ADPCM audio, and TC0100SCN tilemaps / TC0150ROD road
renderer / TC0110PCR palette custom chips. The ports use a software renderer
outputting RGB888 to a Picasso96 RTG screen, with Paula audio driven via a
ring buffer.

## Games

### Continental Circus — Shipping / Complete

Full Taito Z-System emulation port for Amiga RTG. The game is playable
end-to-end with sound.

**Controls (CD32 pad):**

| CD32 Button | Action |
|---|---|
| Red / Fire | Gas |
| Blue / Yellow / Green | Gear toggle |
| Play / 1 | Start |
| Shoulder / 5 | Coin |
| Left / Right | Steer |

### Chase HQ — Work in Progress

Taito Z-System port of Chase HQ using the newer v2 architecture. Title and
attract mode are visible, race rendering is working, the Z80 sound CPU and
YM2610 audio are active, and road / sprite rendering is functional.

Remaining work: performance tuning (Z80 native recompilation planned) and
sprite priority details.

**Controls (CD32 pad):**

| CD32 Button | Action |
|---|---|
| Red / Fire | Gas |
| R shoulder / Down | Brake |
| Blue / Green / LAlt | Nitro / Turbo |
| Yellow / X | Gear toggle |
| Play | Start |
| Shoulder / 5 | Coin |

## Repository Layout

```
continental_circus/   Continental Circus source (build.sh, cc_*.c/h/S, cores/, etc.)
chase_hq/            Chase HQ v2 source (build_rtg.sh, chq_*.c/h/S, amiga/, cores/, etc.)
chase_hq/source_v1/  Older Chase HQ source for reference (build.sh, tools/, etc.)
shared/              Shared Taito Z cores (Musashi m68k, Z80, YM2610 FM cores)
assets/              Boxart / loader images
roms/                README placeholders — ROMs are not included
```

## Shared Cores

Both games share the same emulation cores, stored under `shared/`:

- **Musashi 68000** — MC68000 interpreter (m68kcpu, m68kops, m68kdasm, m68kfpu,
  softfloat)
- **Z80** — Z80 sound CPU emulator (z80.c / z80emu.h)
- **YM2610** — Yamaha YM2610 FM / SSG / ADPCM audio core (fm.c/h, ymdeltat.c/h,
  ay8910.h, fm_tables.h)

## Building

Each game has its own build scripts. See the individual directories for
details:

- `continental_circus/build.sh` — Amiga RTG build (cross-compile)
- `continental_circus/build_host.sh` — Host build for testing
- `chase_hq/build_rtg.sh` — Amiga RTG build (cross-compile)
- `chase_hq/build_host.sh` — Host build for testing

## ROMs

ROM data is **not** included in this repository. You must supply your own
legally-obtained arcade ROMs. See `roms/` for details.

## License

MIT License — see [LICENSE](LICENSE).

Copyright © 2026 Crown Park Computing Ltd.