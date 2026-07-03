# Step 02 — host-reference model (dual MC68000 on Musashi)

Goal: prove the CPU + memory model boots Continental Circus's attract logic and
drives the video hardware RAM, as a correctness oracle for the renderer and the
eventual native Amiga build. Z80 + YM2610 are **not** wired yet (next increment).

Build: `bash build.sh` → `./host [frames]`. Disasm helper: `./dis <bin> <hex> <n>`.

## Result (VERIFIED)

Both 68000s boot through power-on self-test into the game and run the full
attract loop with **zero illegal opcodes** (35M instructions / 1800 frames):

| region (RAM the custom chips read) | nonzero words @1800f |
|------------------------------------|----------------------|
| TC0100SCN tilemap (0x200000)       | 5147 / 32768         |
| TC0150ROD road RAM (0x300000)      | 1673 / 4096          |
| spriteram (0x400000)               | 142 / 896            |
| TC0110PCR palette                  | 1262 / 4096          |

State is dumped to `out/state/*.bin` each run — these are genuine attract-frame
VRAM snapshots for step-03 video decode.

## Memory maps (from taito/taito_z.cpp, `contcirc_map` / `contcirc_cpub_map`)

maincpu (video side):
```
000000-03ffff  ROM (maincpu.bin)
080000-083fff  RAM (private)
084000-087fff  RAM shared with sub ("share1")
090001         contcirc_out_w  (bit0=Z80 reset, bits6-7=road palette bank)
100000-100007  TC0110PCR palette (addr latch @off0, data @off1)
200000-20ffff  TC0100SCN tilemap RAM
220000-22000f  TC0100SCN ctrl (scroll etc.)
300000-301fff  TC0150ROD road RAM ("root ram")
400000-4006ff  spriteram
```
sub (I/O + sound side):
```
000000-03ffff  ROM (sub.bin)
080000-083fff  RAM (private)
084000-087fff  RAM shared with main ("share1")
100001 (byte)  R: input_bypass_r   W: TC0040IOC portreg_w
100003 (byte)  R: watchdog_r(pet)  W: TC0040IOC port_w (selects m_port)
200001 (byte)  W: TC0140SYT master_port_w        (sound, stubbed)
200003 (byte)  RW: TC0140SYT master_comm_r/w     (sound, stubbed -> 0)
```
Both CPUs: IRQ6 autovector once per frame at vblank (irq6_line_hold). They
free-run from reset (out_w bit0 gates the *Z80*, not the sub-68k).

## Boot handshake (reverse-engineered) — shared RAM @ 0x84000 (= A5 base)

1. main masks IRQs, inits TC0100SCN ctrl, RAM-tests private/shared/video/road/
   spriteram regions (test routine @0xdb2, walks 0/ffff/aaaa/5555/0), then a ROM
   checksum (@0xe9c: word-sum of 0..0x3fffe must be 0).
2. main writes **5 → shared[0x566]** to release the sub from its wait loop @0x8ec.
3. sub RAM-tests its own RAM + ROM checksum, writes **1 → shared[0x568]** ("alive").
4. sub runs the **TC0040IOC self-test**: selects port 2 (IN0), requires
   `(IN0 & 0x0c) == 0` (IN0 bits 2,3 = COIN2/COIN1, **active-high** → 0 at idle).
   On failure it sets shared[0x56a]=0xffff (error 9) → which also pushes main into
   its error handler. This was the initial stall (stub returned 0xff for IN0).
5. sub reads DSWA→shared[0x57a], DSWB→shared[0x57c]; both CPUs `jmp 0x78a` (game).

Status codes: shared[0x568] ∈ {1 ok-so-far, 2 RAM-fail, 4 ROM-fail};
shared[0x56a] != 0 ⇒ IOC/error.

## TC0040IOC idle input values (default DIPs, cockpit cabinet)

| m_port | source     | idle |
|--------|------------|------|
| 0      | DSWA       | 0xff |
| 1      | DSWB       | 0xcf |
| 2      | IN0        | 0x13 |
| 3      | IN1        | 0x1f |
| 4      | coin regs  | m_regs[4] |
| 7      | IN2        | 0xff |
| 8/9    | steering   | 0x00 (centered: 0xff80+0x80) |

## Next increment

- Step 03: decode TC0100SCN tiles (gfx_scn.bin, 8x8 4bpp) + TC0110PCR palette,
  render the captured `out/state` tilemap, diff against the live MAME oracle
  frame (BEST PLAYERS / title). Then sprites (zoom) and the TC0150ROD road.
- Wire Z80 + YM2610 + TC0140SYT (sound) — the sub already drives the SYT mailbox.
