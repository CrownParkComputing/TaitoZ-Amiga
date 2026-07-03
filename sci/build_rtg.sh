#!/bin/bash
# S.C.I. v2 Amiga RTG build (Power Drift architecture).
# 8-bit pen8 presenter (640x480, 2x game), wall-clock scheduler, ring audio.
# Output: dist/"S.C.I. v2" (side-by-side with v1 for A/B).
set -e
export PATH="/home/jon/amiga-amigaos/bin:$HOME/.local/bin:$PATH"
cd "$(dirname "$0")"
DIST="$(cd ../dist && pwd)"
B=obj; mkdir -p "$B"
GCC="m68k-amigaos-gcc -m68030 -noixemul -O3 -fomit-frame-pointer -funroll-loops -DNDEBUG -I . -I cores -I cores/softfloat"
GCCYM="m68k-amigaos-gcc -m68030 -noixemul -O3 -fomit-frame-pointer -funroll-loops -fno-strict-aliasing -DNDEBUG -DHAS_YM2610=1 -DSCI_Z80_DIV_DEFAULT=2 -I . -I cores -I cores/ym"

$GCC -c cores/m68kcpu.c             -o "$B/m68kcpu.o"
$GCC -c cores/m68kops.c             -o "$B/m68kops.o"
$GCC -c cores/m68kdasm.c            -o "$B/m68kdasm.o"
$GCC -c cores/softfloat/softfloat.c -o "$B/softfloat.o"
$GCC -c sci_machine.c               -o "$B/sci_machine.o"
$GCC -c sci_render.c                -o "$B/sci_render.o"
$GCC -c amiga/sci_live_main.c       -o "$B/sci_live_main.o"
$GCC -fno-builtin -c sci_mathstubs.c -o "$B/sci_mathstubs.o"
$GCCYM -c sci_audio.c               -o "$B/sci_audio.o"
$GCCYM -c cores/ym/fm.c             -o "$B/fm.o"
$GCCYM -c cores/ym/ymdeltat.c       -o "$B/ymdeltat.o"
$GCC   -c cores/z80.c               -o "$B/z80.o"
$GCC   -c sci_audio_amiga.c         -o "$B/sci_audio_amiga.o"
$GCC   -c sci_romdata.S             -o "$B/sci_romdata.o"

$GCC -o "$DIST/S.C.I. v2" \
    "$B/sci_live_main.o" "$B/sci_machine.o" "$B/sci_render.o" \
    "$B/sci_audio.o" "$B/fm.o" "$B/ymdeltat.o" "$B/z80.o" "$B/sci_audio_amiga.o" \
    "$B/sci_mathstubs.o" \
    "$B/m68kcpu.o" "$B/m68kops.o" "$B/m68kdasm.o" "$B/softfloat.o" \
    "$B/sci_romdata.o" -Wl,--start-group -lamiga -lgcc -Wl,--end-group
ls -l "$DIST/S.C.I. v2" | awk '{print "exe:",$5,"bytes"}'
