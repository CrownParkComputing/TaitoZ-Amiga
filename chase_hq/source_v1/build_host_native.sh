#!/bin/bash
# Host build of cc_hosttest with the NATIVE (Rust statically-recompiled) audio Z80
# in place of the cores/z80.c interpreter (-DCHQ_AUDIO_NATIVE=1) -> /tmp/cc_hosttest_native.
#
# This is the Stage-1B validation gate for the Chase HQ sound-driver transcode:
#   env CC_COIN=1 CC_AUDIODUMP=1 CHQ_COIN_FRAME=1200 CHQ_START_FRAME=1320 \
#       CHQ_GAS_FRAME=1380 /tmp/cc_hosttest_native 6000
# must print the SAME "RAW:" line (counters + ymHash FNV over every YM write) as the
# interpreter build (/tmp/cc_hosttest, from build_host.sh).
#
# Pipeline: tools/z80_recompiler/recompile_chasehq_audio decodes data/audiocpu.bin
# (fixed 0x0000-0x3fff only; the banked window is data) seeded with the runtime
# coverage dump, emits ../build/chq_audio_native/gencrate (gitignored -- ROM-derived),
# which is then cargo-built for the HOST (feature "host") and linked in place of z80.o.
set -e
cd "$(dirname "$0")"
GAME_DIR="$(cd .. && pwd)"
RC_DIR="$PWD/tools/z80_recompiler"
AUD_GEN="$GAME_DIR/build/chq_audio_native/gencrate"
COV="$PWD/tools/z80cov/chq_z80cov_race_20260702.txt"

echo "== generate native Chase HQ sound-Z80 transcode =="
mkdir -p "$AUD_GEN/src"
( cd "$RC_DIR" && cargo build --release --bin recompile_chasehq_audio -q )
"$RC_DIR/target/release/recompile_chasehq_audio" data/audiocpu.bin "$AUD_GEN/src/lib.rs" @"$COV"
cat > "$AUD_GEN/Cargo.toml" <<'EOF'
[package]
name = "chq_audio_z80"
version = "0.1.0"
edition = "2021"

[lib]
crate-type = ["staticlib", "lib"]
path = "src/lib.rs"

[features]
host = []

[profile.release]
panic = "abort"
opt-level = "s"
lto = true
EOF

echo "== host build of the generated crate (feature host) =="
( cd "$AUD_GEN" && cargo build --release --features host -q )
HOST_A="$AUD_GEN/target/release/libchq_audio_z80.a"

echo "== host build of cc_hosttest with -DCHQ_AUDIO_NATIVE=1 (no z80.o) =="
B=/tmp/cc_hostobj_native; mkdir -p "$B"
CC="gcc -O2 -I. -Icores -Icores/ym -Icores/softfloat -DHAS_YM2610=1 -DCHQ_AUDIO_NATIVE=1 -Wno-implicit-function-declaration"
$CC -c cores/m68kcpu.c -o $B/m68kcpu.o
$CC -c cores/m68kops.c -o $B/m68kops.o
$CC -c cores/m68kdasm.c -o $B/m68kdasm.o
$CC -c cores/softfloat/softfloat.c -o $B/softfloat.o
$CC -c cores/ym/fm.c -o $B/fm.o
$CC -c cores/ym/ymdeltat.c -o $B/ymdeltat.o
$CC -c cc_machine.c -o $B/cc_machine.o
$CC -c cc_render.c -o $B/cc_render.o
$CC -c cc_audio.c -o $B/cc_audio.o
$CC -c cc_hosttest.c -o $B/cc_hosttest.o
gcc -o /tmp/cc_hosttest_native $B/*.o "$HOST_A" -lm -lpthread -ldl
echo "built /tmp/cc_hosttest_native"
