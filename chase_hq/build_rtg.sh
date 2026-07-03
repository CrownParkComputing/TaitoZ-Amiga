#!/bin/bash
# Chase HQ v2 Amiga RTG build (Power Drift architecture).
# 8-bit pen8 presenter (864x486, 2x game), wall-clock scheduler, ring audio.
# Output: dist/"Chase HQ v2" (side-by-side with v1 for A/B).
set -e
export PATH="/home/jon/amiga-amigaos/bin:$HOME/.local/bin:$PATH"
cd "$(dirname "$0")"
DIST="$(cd ../dist && pwd)"
AI="/home/jon/AmigaArcadePorts/ArcadeIntro"      # shared loader + F10 DIP editor
B=obj; mkdir -p "$B"
GCC="m68k-amigaos-gcc -m68030 -noixemul -O3 -fomit-frame-pointer -funroll-loops -DNDEBUG -I . -I cores -I cores/softfloat -I $AI"
GCCYM="m68k-amigaos-gcc -m68030 -noixemul -O3 -fomit-frame-pointer -funroll-loops -fno-strict-aliasing -DNDEBUG -DHAS_YM2610=1 -DCHQ_Z80_DIV_DEFAULT=2 -I . -I cores -I cores/ym"

$GCC -c cores/m68kcpu.c             -o "$B/m68kcpu.o"
$GCC -c cores/m68kops.c             -o "$B/m68kops.o"
$GCC -c cores/m68kdasm.c            -o "$B/m68kdasm.o"
$GCC -c cores/softfloat/softfloat.c -o "$B/softfloat.o"
$GCC -c chq_machine.c               -o "$B/chq_machine.o"
$GCC -c chq_render.c                -o "$B/chq_render.o"
$GCC -c amiga/chq_live_main.c       -o "$B/chq_live_main.o"
echo "== in-game RTG bezel + ArcadeIntro F10 DIP editor =="
python3 make_chq_rtg_bezel.py
$GCC -c "$AI/arcade_intro.c"        -o "$B/arcade_intro.o"
m68k-amigaos-as -m68020 "$AI/arcade_intro_glue.s" -o "$B/arcade_intro_glue.o"
m68k-amigaos-as -m68020 "$AI/tc_ptplayer.68k"     -o "$B/tc_ptplayer.o"
m68k-amigaos-as -m68020 "$AI/tc_ptplayer_glue.s"  -o "$B/tc_ptplayer_glue.o"
$GCC -fno-builtin -c chq_mathstubs.c -o "$B/chq_mathstubs.o"
$GCCYM -c chq_audio.c               -o "$B/chq_audio.o"
$GCCYM -c cores/ym/fm.c             -o "$B/fm.o"
$GCCYM -c cores/ym/ymdeltat.c       -o "$B/ymdeltat.o"
$GCC   -c cores/z80.c               -o "$B/z80.o"
$GCC   -c chq_audio_amiga.c         -o "$B/chq_audio_amiga.o"
$GCC   -c chq_romdata.S             -o "$B/chq_romdata.o"

$GCC -o "$DIST/Chase HQ v2" \
    "$B/chq_live_main.o" "$B/chq_machine.o" "$B/chq_render.o" \
    "$B/chq_audio.o" "$B/fm.o" "$B/ymdeltat.o" "$B/z80.o" "$B/chq_audio_amiga.o" \
    "$B/chq_mathstubs.o" \
    "$B/m68kcpu.o" "$B/m68kops.o" "$B/m68kdasm.o" "$B/softfloat.o" \
    "$B/arcade_intro.o" "$B/arcade_intro_glue.o" "$B/tc_ptplayer.o" "$B/tc_ptplayer_glue.o" \
    "$B/chq_romdata.o" -Wl,--start-group -lamiga -lgcc -Wl,--end-group
ls -l "$DIST/Chase HQ v2" | awk '{print "exe:",$5,"bytes"}'
