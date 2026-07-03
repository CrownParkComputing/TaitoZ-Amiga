#!/bin/bash
# Native host build of cc_hosttest: runs the LIVE Amiga pipeline (cc_machine dual-68k
# + cc_render + cc_audio) on the PC, renders cc_hosttest.ppm. Same code as the Amiga
# build, minus the AGA presenter -> proves the live machine+render path with no
# MAME-harness frame desync (renderer reads live spriteram, as on hardware).
set -e
cd "$(dirname "$0")"
B=/tmp/cc_hostobj; mkdir -p "$B"
CC="gcc -O2 -I. -Icores -Icores/ym -Icores/softfloat -DHAS_YM2610=1 -Wno-implicit-function-declaration"
$CC -c cores/m68kcpu.c -o $B/m68kcpu.o
$CC -c cores/m68kops.c -o $B/m68kops.o
$CC -c cores/m68kdasm.c -o $B/m68kdasm.o
$CC -c cores/softfloat/softfloat.c -o $B/softfloat.o
$CC -c cores/z80.c -o $B/z80.o
$CC -c cores/ym/fm.c -o $B/fm.o
$CC -c cores/ym/ymdeltat.c -o $B/ymdeltat.o
$CC -c cc_machine.c -o $B/cc_machine.o
$CC -c cc_render.c -o $B/cc_render.o
$CC -c cc_audio.c -o $B/cc_audio.o
$CC -c cc_hosttest.c -o $B/cc_hosttest.o
gcc -o /tmp/cc_hosttest $B/*.o -lm
echo "built /tmp/cc_hosttest"
