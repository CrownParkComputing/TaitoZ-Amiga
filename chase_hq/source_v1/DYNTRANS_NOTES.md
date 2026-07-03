# Chase HQ 68000 Dyntrans Notes

Goal: replace the Musashi main/sub 68000 interpreter path with a native 68000 to
68EC020 dynamic translator, keeping the current interpreter build as the
correctness reference until the dyntrans build boots and plays cleanly.

## Current Coverage Result

Generated with:

```sh
bash build_host_coverage.sh
CHQ_COVERAGE=/tmp/chq_pc_coverage.txt CC_COIN=1 CHQ_COIN_FRAME=60 CHQ_START_FRAME=120 CHQ_GAS_FRAME=260 /tmp/cc_hostcoverage 6000
bash build_dyntrans_report.sh
/tmp/cc_dyntrans_report /tmp/chq_pc_coverage.txt
```

Result from the sampled gameplay-style path:

```text
pcs=9756 body=7144 emit_ok=9756 emit_bad=0 unclassified=0 abs_refs=489 helper_refs=70 helper_instrs=70
```

The shared Shinobi translator core can now emit every sampled Chase HQ executed
PC after adding support for brief-index indirect terminators:

- `jsr (d8,An,Dn.w)`
- `jmp (d8,An,Dn.w)`

## Memory Model

Flat/rebased memory should work for these regions:

- main ROM
- main RAM
- shared RAM
- tile RAM and tile control RAM
- sprite RAM
- road RAM

These regions need Chase-specific translated helper calls or write logging:

- `0x400000..0x400007` TC0040IOC input register select/read/write
- `0x800000..0x800001` CPU control / sub CPU enable / Z80 reset line
- `0x820000..0x820003` TC0140SYT sound mailbox
- `0xa00000..0xa00007` TC0110PCR palette indexed address/data ports

The palette port is the important blocker for a pure Shinobi-style shadow page:
the game writes an address register and then a data register, so preserving only
the final memory value is not enough. Those writes must call helpers or append
to an ordered write log during translated execution.

Current support code:

- `cc_machine.c` exports `cc_bus_read8/16/32` and `cc_bus_write8/16/32`, so a
  dyntrans backend can use the reference bus semantics without duplicating the
  IOC/palette/sound/CPU-control behaviour.
- `cc_dyntrans_hw.c` provides an ordered hardware write log plus read helpers.
  Reads flush pending writes first, preserving ordered hardware semantics while
  allowing translated palette loops to queue writes cheaply.
- `tools/cc_xlate_plan.c` classifies absolute targets into direct RAM/VRAM
  versus helper-required hardware regions.
- `tools/cc_dyntrans_report.c` now reports `helper_refs` and `helper_instrs`,
  which is the amount of sampled translated code that needs Chase-specific
  handling rather than plain rebased instruction emission.
- `tools/chq_dyntrans_audit.sh [frames] [coverage-file]` rebuilds coverage and
  regenerates the report.

The helper split in the 6000-frame sample is:

```text
direct_helper_refs=63 pointer_helper_refs=7 pointer_loads=7
```

The pointer loads matter. Examples:

```text
000430  movea.l #$a00000, A2
000436  lea     ($2,A2), A3
00043e  move.w  D0, (A2)
000440  move.w  -(A1), (A3)
000442  dbra    D0, $43e
```

So the Chase backend needs a hardware-pointer map for address registers, not
only absolute-opcode replacement. Palette base registers are used in tight DBRA
loops and sometimes passed across local BSRs.

## Next Implementation Steps

1. Add a Chase-specific emitter layer above `shinobi_xlate.c` that detects
   absolute hardware-port operands and emits calls/log writes instead of direct
   rebased memory access.
2. Start with the common patterns seen in coverage:
   - `move.b #imm,$800001.l`
   - `move.b Dn,$800001.l` / `move.b (d16,A5),$800001.l`
   - `move.w #imm,$a00000.l`
   - `move.w Dn,$a00002.l`
   - `clr.w $a00002.l`
   - `move.w $400002.l,Dn`
3. Keep tile/sprite/shared RAM as direct flat memory and copy/mirror it to the
   existing renderer state while the dyntrans backend is being validated.
4. Bring up main CPU dyntrans first. Add sub CPU dyntrans after main boot and
   palette/video writes are stable.
