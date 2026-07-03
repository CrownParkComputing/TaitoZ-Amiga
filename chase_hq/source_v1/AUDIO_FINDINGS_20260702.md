# Chase HQ audio findings — handoff from the Indy-audio session (2026-07-02 ~06:10)

Written by a DIFFERENT Claude session (the one fixing Indiana Jones audio). We detected you
actively working this tree (cc_state.h 05:45, live amiberry-chq-test) and STOPPED to avoid a
collision. Two things below were already applied to `cc_audio_amiga.c` in place; the rest is
analysis you can use or discard.

## Applied to cc_audio_amiga.c (by this session — review before your next build)

1. **dt clamp removed from the consumption model.** `p_play` now ALWAYS advances by true
   elapsed wall time via an exact 32-bit decomposition (Paula sample clock = EClock*5 on the
   same crystal): `q = dt/A_PER; t = aud_frac + (dt - q*A_PER)*5; p_play += q*5 + t/A_PER`.
   The old `dt > aud_rate/15` clamp under-counted consumption on long frames, so once Paula
   lapped the write cursor the stale-playback offset NEVER self-healed = the persistent
   "jumping record" the user hears.
2. **Underrun/resync guard added** (there was an overrun cap but no underrun handling): if
   `(long)(p_wrote - p_play) < CC_SPF`, zero the whole ring, flush, `p_wrote = p_play +
   CC_LEAD`, return (render resumes next call, well ahead of the head). Stale ring content
   can now never loop audibly; worst case is a short self-healing silence.

## Root cause of FPS 18 (from a deep design pass — gprof host evidence)

The perf HUD `FPS 18 0 M0 A88 R4 P0` = machine 0%, AUDIO 88%, render 4%, present 0%.
Inside the A bucket, the dominant cost is **the OPN envelope generator, and it is
sample-rate independent** — which is why the 22050→11025 halving barely helped:
`advance_eg_channel` (fm.c:1217) fires at the native chip EG rate 8MHz/144/3 = 18,518/s ×
4 channels ≈ 296k slot evaluations/s regardless of output rate. gprof (gmon.out in this
dir): 2,952,304 calls = 26.5% of the whole host run ≈ 85-90% of the audio share.

### Ranked fixes (modeled: Step A alone ≈ 18→~60fps)

A. **EG event gating (fm.c)**: add `eg_sh_cur` per slot (31 when EG_OFF, else the current
   state's eg_sh_*); at the top of the slot loop skip unless
   `!(OPN->eg_cnt & ((1u<<eg_sh_cur)-1))` (SSG-EG slots keep the original path). Each switch
   case already begins with exactly this test, so hoisting is a pure event-skip —
   **bit-identical output, host-verifiable**. Companion: `set_tl()` must refresh
   `SLOT->vol_out` on TL writes. Optional second stage: skip `chan_calc` entirely when all
   4 slots are EG_OFF.
B. **SSG silence skip (cc_audio.c:245)**: the AY sim ticks 250,000/s even when silent.
   Skip when `!(reg[8]&0x1f) && !(reg[9]&0x1f) && !(reg[10]&0x1f)` (mixer bits alone are
   NOT sufficient). Verify first via the host RAW counters whether chasehq ever uses SSG.
C. **ADPCM-B pitch BUG (correctness — likely part of "speech not clear")**:
   `cores/ym/ymdeltat.c:99-102` does `D->step = D->delta;`, dropping MAME's
   `* freqbase` scaling (native 55,555Hz / output rate ≈ 5.04 at 11025); fm.c:4068 copies
   an UNINITIALIZED double into `DELTAT->freqbase` which ymdeltat.c never reads. Delta-T
   samples currently play 2.5-5x TOO SLOW/DEEP. Fix integer-only:
   `step = delta * (clock/rate) / 144` (fits u32: 65535*726 = 47.6M).
D. Micro: `/38` in cc_audio.c:250 → `(s*1725)>>16`; batch flush_ring_range once per frame.

### Verification recipe (this tree already has the harness)
- Host: `bash build_host.sh`; `CC_COIN=1 CHQ_COIN_FRAME=60 CHQ_START_FRAME=120
  CHQ_GAS_FRAME=260 CC_AUDIODUMP=1 /tmp/cc_hosttest 6000` — RAW counters must be unchanged
  by EG work; add a PCM dump and require sample-exact output for fix A.
- Target: `SetEnv CHQPERF 1` HUD before/after (goal: A% < 40, FPS ≈ 60).
- Pitch/tempo vs `mame chasehq -wavwrite` (project standard); fix C changes speech pitch
  by design — compare against MAME, not against the current build.
