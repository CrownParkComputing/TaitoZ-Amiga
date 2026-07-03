#!/bin/bash
# S.C.I. v2 host harness: the LIVE pipeline (sci_machine + sci_render +
# sci_audio) on the PC. Parity gate: output must match source/'s host build.
set -e
cd "$(dirname "$0")"
B=/tmp/sci_hostobj; mkdir -p "$B"
CC="gcc -O2 -I. -Icores -Icores/ym -Icores/softfloat -DHAS_YM2610=1 -Wno-implicit-function-declaration"
$CC -c cores/m68kcpu.c -o $B/m68kcpu.o
$CC -c cores/m68kops.c -o $B/m68kops.o
$CC -c cores/m68kdasm.c -o $B/m68kdasm.o
$CC -c cores/softfloat/softfloat.c -o $B/softfloat.o
$CC -c cores/z80.c -o $B/z80.o
$CC -c cores/ym/fm.c -o $B/fm.o
$CC -c cores/ym/ymdeltat.c -o $B/ymdeltat.o
$CC -c sci_machine.c -o $B/sci_machine.o
$CC -c sci_render.c -o $B/sci_render.o
$CC -c sci_audio.c -o $B/sci_audio.o
$CC -c sci_hosttest.c -o $B/sci_hosttest.o
gcc -o /tmp/sci_hosttest $B/*.o -lm
echo "built /tmp/sci_hosttest"
