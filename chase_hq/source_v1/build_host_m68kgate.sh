#!/bin/bash
# Host build of cc_hosttest where the NATIVE audio-Z80 transcode is executed as REAL
# m68k MACHINE CODE under a second (symbol-renamed) Musashi 68000 -> /tmp/cc_hosttest_m68kgate.
#
# This is the m68k-CODEGEN validation gate for the Chase HQ sound-driver transcode:
# build_host_native.sh proves the generated RUST is bit-exact vs the interpreter, but
# it is host-x86 codegen; THIS gate runs the exact m68k object the Amiga executes:
#   env CC_COIN=1 CC_AUDIODUMP=1 CHQ_COIN_FRAME=1200 CHQ_START_FRAME=1320 \
#       CHQ_GAS_FRAME=1380 /tmp/cc_hosttest_m68kgate 6000
# must print the SAME "RAW:" line as /tmp/cc_hosttest (interpreter) and
# /tmp/cc_hosttest_native (host-compiled crate).
#
# Reuses /tmp/cc_hostobj_native objects (build_host_native.sh must have run first,
# so the m68k crate and the host reference correspond to the same generated lib.rs).
set -e
cd "$(dirname "$0")"
GAME_DIR="$(cd .. && pwd)"
AUD_GEN="$GAME_DIR/build/chq_audio_native/gencrate"
B=/tmp/cc_hostobj_m68kgate; mkdir -p "$B"

if [ ! -d /tmp/cc_hostobj_native ] || [ ! -f /tmp/cc_hostobj_native/cc_audio.o ]; then
    echo "run build_host_native.sh first (needs /tmp/cc_hostobj_native + fresh gencrate)" >&2
    exit 1
fi

echo "== m68k build of the generated crate (the exact code the Amiga runs) =="
( cd "$AUD_GEN" && cargo +nightly build -Z build-std=core --target m68k-unknown-none-elf --release -q )
AUD_A="$AUD_GEN/target/m68k-unknown-none-elf/release/libchq_audio_z80.a"
RUST_BIN=$(ls -d ~/.rustup/toolchains/nightly-*/lib/rustlib/*/bin | head -1)
rm -rf "$B/aud_rsobj"; mkdir -p "$B/aud_rsobj"
( cd "$B/aud_rsobj" && "$RUST_BIN/llvm-ar" x "$AUD_A" )
for o in "$B"/aud_rsobj/chq_audio_z80-*.o; do
  "$RUST_BIN/llvm-objcopy" --remove-section .comment --remove-section .note.GNU-stack \
      --remove-section .llvmbc --remove-section .llvmcmd --prefix-symbols=_ "$o" "$o" 2>/dev/null || true
done
M68KOBJ=$(ls "$B"/aud_rsobj/chq_audio_z80-*.o | head -1)
echo "m68k object: $M68KOBJ"

echo "== second Musashi copy, renamed to g68k_ (coexists with cc_machine's instance) =="
CC="gcc -O2 -I. -Icores -Icores/ym -Icores/softfloat -DHAS_YM2610=1 -Wno-implicit-function-declaration"
$CC -c cores/m68kcpu.c -o "$B/g68k_m68kcpu.o"
$CC -c cores/m68kops.c -o "$B/g68k_m68kops.o"
# rename every DEFINED symbol and every UNDEFINED m68k* symbol (API + callbacks +
# internals + the default_* handlers); libc/softfloat imports stay shared.
{ nm --defined-only "$B/g68k_m68kcpu.o" "$B/g68k_m68kops.o" 2>/dev/null | awk 'NF==3{print $3, "g68k_"$3}'
  nm -u             "$B/g68k_m68kcpu.o" "$B/g68k_m68kops.o" 2>/dev/null | awk '$2 ~ /^m68k/{print $2, "g68k_"$2}'
} | sort -u > "$B/g68k_map.txt"
objcopy --redefine-syms="$B/g68k_map.txt" "$B/g68k_m68kcpu.o" "$B/g68k_m68kcpu.o"
objcopy --redefine-syms="$B/g68k_map.txt" "$B/g68k_m68kops.o" "$B/g68k_m68kops.o"

echo "== gate shim =="
$CC -DCHQ_AUDIO_NATIVE=1 -c tools/chq_m68k_gate.c -o "$B/chq_m68k_gate.o"

echo "== link cc_hosttest_m68kgate (native cc_audio + gate, NO host crate) =="
gcc -o /tmp/cc_hosttest_m68kgate \
    /tmp/cc_hostobj_native/m68kcpu.o /tmp/cc_hostobj_native/m68kops.o \
    /tmp/cc_hostobj_native/m68kdasm.o /tmp/cc_hostobj_native/softfloat.o \
    /tmp/cc_hostobj_native/fm.o /tmp/cc_hostobj_native/ymdeltat.o \
    /tmp/cc_hostobj_native/cc_machine.o /tmp/cc_hostobj_native/cc_render.o \
    /tmp/cc_hostobj_native/cc_audio.o /tmp/cc_hostobj_native/cc_hosttest.o \
    "$B/chq_m68k_gate.o" "$B/g68k_m68kcpu.o" "$B/g68k_m68kops.o" \
    -lm -lpthread -ldl
echo "built /tmp/cc_hosttest_m68kgate  (CHQ_M68KOBJ=$M68KOBJ)"
echo "$M68KOBJ" > /tmp/chq_m68kobj_path.txt
