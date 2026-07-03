#!/usr/bin/env bash
# Build the contcirc step-02 host reference (native, Musashi 68000 core).
set -euo pipefail
cd "$(dirname "$0")"

M=/home/jon/arcade-amiga-template/src/cores/m68k
CC=${CC:-gcc}
CFLAGS="-O2 -w -I $M -I $M/softfloat"
mkdir -p obj

# Musashi core (m68kcpu.c #includes m68kfpu.c; softfloat is a separate TU)
$CC $CFLAGS -c "$M/m68kcpu.c"          -o obj/m68kcpu.o
$CC $CFLAGS -c "$M/m68kops.c"          -o obj/m68kops.o
$CC $CFLAGS -c "$M/m68kdasm.c"         -o obj/m68kdasm.o
$CC $CFLAGS -c "$M/softfloat/softfloat.c" -o obj/softfloat.o

# Harness
$CC $CFLAGS -c host.c -o obj/host.o
$CC obj/host.o obj/m68kcpu.o obj/m68kops.o obj/m68kdasm.o obj/softfloat.o -lm -o host
echo "built ./host"
