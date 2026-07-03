#!/bin/bash
set -e
cd "$(dirname "$0")"
B=/tmp/chq_covobj; mkdir -p "$B"
CC="gcc -O2 -I. -Icores -Icores/ym -Icores/softfloat -DHAS_YM2610=1 -Wno-implicit-function-declaration"
$CC -c cores/z80.c -o $B/z80.o
$CC -c cores/ym/fm.c -o $B/fm.o
$CC -c cores/ym/ymdeltat.c -o $B/ymdeltat.o
$CC -c cc_audio.c -o $B/cc_audio.o
$CC -c chq_covplay.c -o $B/chq_covplay.o
gcc -o /tmp/chq_covplay $B/z80.o $B/fm.o $B/ymdeltat.o $B/cc_audio.o $B/chq_covplay.o -lm
echo built /tmp/chq_covplay
