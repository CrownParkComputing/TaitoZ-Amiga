#!/bin/bash
# Chase HQ RTG interpreter build (m68k-amigaos, bebbo gcc).
# Dual MC68000 (Musashi) + software renderer (TC0100SCN/TC0150ROD/sprites) +
# truecolour RTG presenter (cc_rtg_main.c) + YM2610/Paula sound.
# ROMs embedded (cc_romdata.S). Output: clean dist deliverables:
# executable, icon, executable zip, self-contained Workbench/Picasso96 HDF, and Amiberry UAE.
set -e
export PATH="/home/jon/amiga-amigaos/bin:$HOME/.local/bin:$PATH"
cd "$(dirname "$0")"
ROOT="$(cd .. && pwd)"
REPO="$(cd ../../.. && pwd)"
B=obj_rtg
DIST="$ROOT/dist"
EXE="$DIST/Chase HQ"
ICON="$ROOT/Chase HQ.info"
DIST_ICON="$DIST/Chase HQ.info"
ICON_PNG="$ROOT/source/assets/ChaseHQ.png"
ICON_TOOL="$REPO/Capcom_Z80_Dual_YM2203/shared_source/make_workbench_icon.py"
BG_TOOL="$REPO/tools/make_launcher_bg.py"
LAUNCHER_W=1280
LAUNCHER_H=720
EMU_WINDOW_W=1280
EMU_WINDOW_H=720
WBPATTERN_TOOL="$ROOT/source/make_wbpattern_pref.py"
ICON_POS_TOOL="$ROOT/source/patch_workbench_icon_pos.py"
HDF="$DIST/Chase HQ.hdf"
HD_DIR="$DIST/WhittyArcade_HD"
UAE="$DIST/Chase HQ.uae"
DIST_ZIP="$DIST/Chase HQ.zip"
ZIP="$REPO/packages/zips/Chase HQ.zip"
PKGROOT="$ROOT/source/$B/package"
PKGDIR="$PKGROOT/Chase HQ"
STAGE="$B/hdf_stage"
BOOTROOT="$STAGE/RTG-Boot"
PIMIGA_SYSTEM="/home/jon/Amiberry/HardDrives/Pimiga/System"
CLEAN_LOADWB="$ROOT/source/prefs/LoadWB"
SCREENMODE_PREFS=""
SYSTEM_CONFIGURATION="$ROOT/source/prefs/system-configuration"
LAUNCHER_BIN="$REPO/tools/arcade_launcher"
COMMANDO_EXE="$REPO/Capcom_Z80_Dual_YM2203/Commando/dist/Commando"
COMMANDO_ICON="$REPO/Capcom_Z80_Dual_YM2203/Commando/dist/Commando.info"
mkdir -p "$B" "$DIST" "$PKGDIR" "$REPO/packages/zips" "$REPO/packages/hdf"
find "$DIST" -mindepth 1 -maxdepth 1 \
    ! -name "Chase HQ" \
    ! -name "Chase HQ.info" \
    ! -name "Chase HQ.zip" \
    ! -name "Chase HQ.hdf" \
    ! -name "Chase HQ.uae" \
    ! -name "ChaseHQ_boot_*" \
    ! -name "_known_good_*" \
    -exec rm -rf {} +
GCC="m68k-amigaos-gcc -m68030 -noixemul -O3 -fomit-frame-pointer -funroll-loops -DNDEBUG -I . -I cores -I cores/softfloat"
GCCYM="m68k-amigaos-gcc -m68030 -noixemul -O3 -fomit-frame-pointer -funroll-loops -fno-strict-aliasing -DNDEBUG -DHAS_YM2610=1 -DCHQ_Z80_DIV_DEFAULT=2 -I . -I cores -I cores/ym"

echo "== Musashi core =="
$GCC -c cores/m68kcpu.c           -o "$B/m68kcpu.o"
$GCC -c cores/m68kops.c           -o "$B/m68kops.o"
$GCC -c cores/m68kdasm.c          -o "$B/m68kdasm.o"
$GCC -c cores/softfloat/softfloat.c -o "$B/softfloat.o"

echo "== machine + renderer + presenter =="
# The presenter needs the native-audio define too: the perf HUD appends the
# K<FM key-ons> H<ymHash> audio telemetry only in CHQ_AUDIO_NATIVE builds.
if [ "${CHQ_AUDIO_NATIVE:-0}" = "1" ]; then
    PRESENT_DEFS="-DCHQ_AUDIO_NATIVE=1"
else
    PRESENT_DEFS=""
fi
$GCC -c cc_machine.c -o "$B/cc_machine.o"
$GCC -c cc_render.c  -o "$B/cc_render.o"
$GCC $PRESENT_DEFS -c cc_rtg_main.c -o "$B/cc_rtg_main.o"
$GCC -fno-builtin -c cc_mathstubs.c -o "$B/cc_mathstubs.o"

if [ "${CHQ_AUDIO_NATIVE:-0}" = "1" ]; then
    # NATIVE sound Z80 (Stage 1B): Rust static recompile of the driver replaces the
    # cores/z80.c interpreter. Host-validated bit-exact vs the interpreter build
    # (build_host_native.sh: RAW counters + ymHash + PCM dump all identical).
    # Generated code is ROM-derived and lives under build/ (gitignored).
    #
    # VALIDATION (2026-07-03): ALSO run build_host_m68kgate.sh after ANY generator/
    # prelude/rustc change -- it executes the ACTUAL m68k object under a Musashi
    # 68000 inside the same gate and must print the same RAW line + PCM.  The host
    # gate alone CANNOT see LLVM m68k backend miscompiles (one was reproduced when
    # the prelude's black_box CCR workarounds were lifted -- they are load-bearing).
    # The ELF->hunk path below is verified exact (all 26,686 relocs + jump tables
    # convert correctly, checked reloc-by-reloc into the final exe), so vlink -r is
    # NOT a correctness hazard; the earlier target symptom (no music, jerky) was the
    # driver running ~73 emulated-68k cycles per Z80 cycle and starving the Paula
    # ring (underrun guard zeroed it every frame).  The generator now emits table
    # dispatch + localized hot state (~29% cheaper, Musashi-gate measured).
    echo "== sound: NATIVE Z80 transcode (Rust->m68k) + YM2610 (fm/ymdeltat) + Paula =="
    RC_DIR="$PWD/tools/z80_recompiler"
    AUD_GEN="$ROOT/build/chq_audio_native/gencrate"
    AUD_COV="$PWD/tools/z80cov/chq_z80cov_race_20260702.txt"
    mkdir -p "$AUD_GEN/src"
    ( cd "$RC_DIR" && cargo build --release --bin recompile_chasehq_audio -q )
    "$RC_DIR/target/release/recompile_chasehq_audio" data/audiocpu.bin "$AUD_GEN/src/lib.rs" @"$AUD_COV"
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
    ( cd "$AUD_GEN" && cargo +nightly build -Z build-std=core --target m68k-unknown-none-elf --release -q )
    AUD_A="$AUD_GEN/target/m68k-unknown-none-elf/release/libchq_audio_z80.a"
    RUST_BIN=$(ls -d ~/.rustup/toolchains/nightly-*/lib/rustlib/*/bin | head -1)
    rm -rf "$B/aud_rsobj"
    mkdir -p "$B/aud_rsobj"
    ( cd "$B/aud_rsobj" && "$RUST_BIN/llvm-ar" x "$AUD_A" )
    # prefix `_`: ELF `chq_aud_run` -> `_chq_aud_run` == amigaos C name chq_aud_run,
    # and the crate's chq_aud_* C hook imports resolve to their cc_audio.c names.
    for o in "$B"/aud_rsobj/*.o; do
      "$RUST_BIN/llvm-objcopy" --remove-section .comment --remove-section .note.GNU-stack \
          --remove-section .llvmbc --remove-section .llvmcmd --prefix-symbols=_ "$o" "$o" 2>/dev/null || true
    done
    # bebbo's amigaos ld can't read m68k ELF objects; convert the crate's single
    # cgu object to a hunk-format relocatable with vlink -r. LTO folds the whole
    # transcode into that one object (its only imports are abort + the chq_aud_*
    # hooks -- no compiler_builtins needed), so the rest of the archive is dropped.
    vlink -r -b amigahunk -o "$B/chq_aud_native.o" "$B"/aud_rsobj/chq_audio_z80-*.o
    $GCCYM -DCHQ_AUDIO_NATIVE=1 -c cc_audio.c -o "$B/cc_audio.o"
    Z80_OBJS=("$B/chq_aud_native.o")
else
    echo "== sound: Z80 + YM2610 (fm/ymdeltat) + Paula =="
    $GCCYM -c cc_audio.c        -o "$B/cc_audio.o"
    $GCC   -c cores/z80.c       -o "$B/z80.o"
    Z80_OBJS=("$B/z80.o")
fi
$GCCYM -c cores/ym/fm.c     -o "$B/fm.o"
$GCCYM -c cores/ym/ymdeltat.c -o "$B/ymdeltat.o"
$GCC   -c cc_audio_amiga.c  -o "$B/cc_audio_amiga.o"

echo "== embedded ROM data =="
$GCC -c cc_romdata.S -o "$B/cc_romdata.o"

echo "== link =="
$GCC -o "$EXE" \
    "$B/cc_rtg_main.o" "$B/cc_machine.o" "$B/cc_render.o" \
    "$B/cc_audio.o" "$B/fm.o" "$B/ymdeltat.o" "${Z80_OBJS[@]}" "$B/cc_audio_amiga.o" "$B/cc_mathstubs.o" \
    "$B/m68kcpu.o" "$B/m68kops.o" "$B/m68kdasm.o" "$B/softfloat.o" \
    "$B/cc_romdata.o" -Wl,--start-group -lamiga -lgcc -Wl,--end-group
ls -l "$EXE" 2>/dev/null | awk '{print "exe:",$5,"bytes"}'

echo "== icon + root launcher =="
if [ ! -f "$ICON" ] && [ -f "$DIST_ICON" ]; then
    cp -f "$DIST_ICON" "$ICON"
fi
if [ ! -f "$ICON" ] && [ -f "$REPO/Taito_Z_System/Continental_Circus/Continental Circus.info" ]; then
    cp -f "$REPO/Taito_Z_System/Continental_Circus/Continental Circus.info" "$ICON"
fi
if [ -f "$ICON_PNG" ] && [ -f "$ICON_TOOL" ]; then
    python3 "$ICON_TOOL" "$ICON" "$ICON_PNG" "$ICON"
fi
cp -f "$EXE" "$ROOT/Chase HQ"
cp -f "$ICON" "$DIST_ICON"
ls -l "$ROOT/Chase HQ" "$EXE" "$DIST_ICON"

echo "== build self-contained RTG Workbench HD folder =="
cat > "$B/startup-sequence" <<EOF
; WhittyArcade PiMiga Workbench boot.
; Lean arcade startup: keep PiMiga Workbench/prefs/icons/RTG, skip bundled software assigns.

C:SetPatch QUIET
FailAt 21

Resident >NIL: C:Assign PURE
Resident >NIL: C:Execute PURE
Resident >NIL: C:IconX PURE
Resident >NIL: C:WBRun PURE

IF EXISTS L:env-handler
  C:MakeDir RAM:T RAM:Clipboards
  L:env-handler
ELSE
  C:MakeDir RAM:T RAM:Clipboards RAM:ENV RAM:ENV/Sys
  C:Copy >NIL: ENVARC: RAM:ENV ALL NOREQ
  Assign >NIL: ENV: RAM:ENV
EndIF

Assign >NIL: T: RAM:T
Assign >NIL: CLIPS: RAM:Clipboards
Assign >NIL: REXX: S:
Assign >NIL: PRINTERS: DEVS:Printers
Assign >NIL: KEYMAPS: DEVS:Keymaps
Assign >NIL: LOCALE: SYS:Locale
Assign >NIL: LIBS: SYS:Libs
Assign >NIL: LIBS: SYS:Classes ADD
Assign >NIL: DATATYPES: DEVS:DataTypes
Assign >NIL: Picasso96: SYS:System/Picasso96

IF EXISTS SYS:System/MUI
  Assign >NIL: MUI: SYS:System/MUI
  IF EXISTS MUI:Libs
    Assign >NIL: ADD LIBS: MUI:Libs
  EndIF
  IF EXISTS MUI:Locale
    Assign >NIL: ADD LOCALE: MUI:Locale
  EndIF
EndIF

BindDrivers
C:Mount >NIL: DEVS:DOSDrivers/~(#?.info)

IF EXISTS DEVS:Monitors/uaegfx
  DEVS:Monitors/uaegfx
ELSE
  IF EXISTS DEVS:Monitors/more/uaegfx
    DEVS:Monitors/more/uaegfx
  EndIF
EndIF

IF EXISTS DEVS:Monitors/PAL
  DEVS:Monitors/PAL
EndIF
IF EXISTS DEVS:Monitors/NTSC
  DEVS:Monitors/NTSC
EndIF

C:AddDataTypes REFRESH QUIET
C:IPrefs
C:ConClip

Path >NIL: RAM: C: SYS:Utilities SYS:Rexxc SYS:System S: SYS:Prefs SYS:WBStartup SYS:Tools SYS:Tools/Commodities
Echo "WhittyArcade PiMiga Workbench"
C:LoadWB
C:Wait 2 SECS
IF EXISTS SYS:ArcadeLauncher
  C:Run >NIL: SYS:ArcadeLauncher
EndIF
EndCLI >NIL:
EOF
rm -f "$HDF"
rm -rf "$HD_DIR"
if [ ! -d "$PIMIGA_SYSTEM" ]; then
    echo "ERROR: missing PiMiga System folder: $PIMIGA_SYSTEM" >&2
    exit 1
fi
if [ ! -f "$CLEAN_LOADWB" ]; then
    echo "ERROR: missing clean Workbench LoadWB: $CLEAN_LOADWB" >&2
    exit 1
fi
rm -rf "$STAGE"
mkdir -p "$BOOTROOT"
pimiga_keep=(
    ".backdrop"
    "C" "Classes" "Data" "Datatypes" "Devs" "Fonts" "Gadgets" "L" "Libs"
    "Locale" "Picasso96" "Prefs" "Rexxc" "S" "Storage" "System" "Tools"
    "Utilities" "WBStartup" "images"
    "Disk.info" "Disk.info.black" "Devs.info" "Locale.info" "Picasso96.info"
    "Prefs.info" "Storage.info" "System.info" "Tools.info" "Utilities.info"
    "WBStartup.info" "colortable" "nil" "usergadgets.defs" "window.class"
)
for item in "${pimiga_keep[@]}"; do
    if [ -e "$PIMIGA_SYSTEM/$item" ]; then
        cp -a "$PIMIGA_SYSTEM/$item" "$BOOTROOT/"
    fi
done
find "$BOOTROOT" -name '_UAEFSDB.___' -delete
if [ -d "$BOOTROOT/WBStartup" ]; then
    find "$BOOTROOT/WBStartup" -mindepth 1 -maxdepth 1 -exec rm -rf {} +
fi
rm -rf "$BOOTROOT/System/Scalos" "$BOOTROOT/System/Scalos.info" \
       "$BOOTROOT/System/SAMBA" "$BOOTROOT/System/SAMBA.info" \
       "$BOOTROOT/System/BMenu" "$BOOTROOT/System/BMenu.info" \
       "$BOOTROOT/System/Executive" "$BOOTROOT/System/Executive.info" \
       "$BOOTROOT/System/TMeter" "$BOOTROOT/System/TMeter.info" \
       "$BOOTROOT/Prefs/Env-Archive/Scalos" "$BOOTROOT/Prefs/Presets/Scalos" \
       "$BOOTROOT/System/Copper/Disable/Prefs/Env-Archive/Scalos" \
       "$BOOTROOT/System/Copper/Enable/Prefs/Env-Archive/Scalos"
rm -f "$BOOTROOT/C/FixScalos" "$BOOTROOT/C/LoadWB scalos working one" \
      "$BOOTROOT/Prefs/Env-Archive/mui/SCALOS_"*.prefs \
      "$BOOTROOT/S/Env-Archive/mui/SCALOS_"*.prefs \
      "$BOOTROOT/Locale/Help/System/FixScalos.txt" \
      "$BOOTROOT/Locale/Help/System/Scalos_Info.guide"
cp -f "$CLEAN_LOADWB" "$BOOTROOT/C/LoadWB"
rm -f "$BOOTROOT/S/SnoopDos.log" "$BOOTROOT/S/user-startup" "$BOOTROOT/S/User-Startup"
mkdir -p "$BOOTROOT/S" "$BOOTROOT/WBStartup" "$BOOTROOT/Prefs/Env-Archive/Sys" "$BOOTROOT/Prefs/Presets" "$BOOTROOT/Devs"
rm -f "$BOOTROOT/Prefs/Env-Archive/Sys/ScreenMode.prefs" \
      "$BOOTROOT/Prefs/Env-Archive/Sys/screenmode.prefs" \
      "$BOOTROOT/Prefs/Env-Archive/Sys/Screenmode.prefs"
cp -f "$B/startup-sequence" "$BOOTROOT/S/startup-sequence"
cp -f "$B/startup-sequence" "$BOOTROOT/S/Startup-Sequence"
cp -f "$EXE" "$BOOTROOT/Chase HQ"
cp -f "$DIST_ICON" "$BOOTROOT/Chase HQ.info"

stage_game() {
    local name="$1"
    local slug="$2"
    local src="$3"
    local icon="$4"
    local asset_dir="$5"
    local artwork="$6"
    local exe_name="$7"
    local hardware="$8"
    local status="$9"
    shift 9
    local notes="$1"
    local year="$2"
    local maker="$3"
    local scores_dir="$4"
    local gamedir="$BOOTROOT/Games/$slug"
    local savedir="$BOOTROOT/Saves/$slug"
    local artdir="$BOOTROOT/Artwork/$slug"
    local art_path="SYS:Artwork/$slug.bg"

    if [ ! -f "$src" ]; then
        echo "skip launcher game, missing executable: $name ($src)"
        return
    fi

    mkdir -p "$gamedir" "$savedir" "$artdir"
    cp -f "$src" "$gamedir/$exe_name"
    if [ -f "$icon" ]; then
        cp -f "$icon" "$gamedir/$exe_name.info"
    fi

    if [ -d "$asset_dir" ]; then
        cp -f "$asset_dir"/*.png "$artdir/" 2>/dev/null || true
        cp -f "$asset_dir"/*.info "$artdir/" 2>/dev/null || true
        cp -f "$asset_dir"/*.mod "$artdir/" 2>/dev/null || true
    fi
    if [ -f "$artwork" ]; then
        cp -f "$artwork" "$BOOTROOT/Artwork/$slug.png"
        if [ -f "$BG_TOOL" ]; then
            python3 "$BG_TOOL" "$artwork" "$BOOTROOT/Artwork/$slug.bg" "$LAUNCHER_W" "$LAUNCHER_H"
        else
            art_path="SYS:Artwork/$slug.png"
        fi
    else
        art_path="SYS:Artwork/$slug"
    fi
    if [ ! -f "$BOOTROOT/Artwork/$slug.bg" ]; then
        art_path=""
    fi
    if [ -d "$scores_dir" ]; then
        cp -f "$scores_dir"/* "$gamedir/" 2>/dev/null || true
        cp -f "$scores_dir"/* "$savedir/" 2>/dev/null || true
    fi

    cat > "$BOOTROOT/S/Launch-$slug" <<EOF
Stack 65536
SetEnv WHITTY_NO_GAME_LOADER 1
CD "SYS:Games/$slug"
"$exe_name"
CD SYS:
SetEnv WHITTY_NO_GAME_LOADER 0
EOF
    printf '%s|Execute SYS:S/Launch-%s|%s|%s|%s|%s|SYS:Saves/%s/%s.state|%s|%s\n' \
        "$name" "$slug" "$hardware" "$status" "$notes" "$art_path" "$slug" "$slug" "$year" "$maker" \
        >> "$BOOTROOT/S/ArcadeLauncher.cfg"
}

if [ -f "$LAUNCHER_BIN" ]; then
    cp -f "$LAUNCHER_BIN" "$BOOTROOT/ArcadeLauncher"
    mkdir -p "$BOOTROOT/Games" "$BOOTROOT/Saves" "$BOOTROOT/Artwork"
    : > "$BOOTROOT/S/ArcadeLauncher.cfg"
    cat > "$BOOTROOT/S/StartArcadeLauncher" <<EOF
IF EXISTS SYS:ArcadeLauncher
  SYS:ArcadeLauncher
EndIF
EOF

    stage_game "Chase HQ" "ChaseHQ" "$EXE" "$DIST_ICON" "$ROOT/source/assets" "$ICON_PNG" \
        "Chase HQ" "Taito Z System / RTG" "Installed" "Dual 68000, Z80, YM2610, RTG road renderer" "1988" "Taito" ""
    stage_game "Continental Circus" "ContinentalCircus" "$REPO/Taito_Z_System/Continental_Circus/dist/Continental Circus" "$REPO/Taito_Z_System/Continental_Circus/dist/Continental Circus.info" "$REPO/Taito_Z_System/Continental_Circus/assets" "$REPO/Taito_Z_System/Continental_Circus/assets/contcirc_loader.png" \
        "Continental Circus" "Taito Z System / RTG" "Installed" "Taito racing hardware sibling port" "1987" "Taito" ""

    stage_game "1942" "1942" "$REPO/Capcom_Z80_Dual_YM2203/1942/dist/1942" "$REPO/Capcom_Z80_Dual_YM2203/1942/dist/1942.info" "$REPO/Capcom_Z80_Dual_YM2203/1942/assets" "$REPO/Capcom_Z80_Dual_YM2203/1942/assets/1942_loader.png" \
        "1942" "Capcom Z80 / RTG" "Installed" "Vertical shooter board port" "1984" "Capcom" ""
    stage_game "1943" "1943" "$REPO/Capcom_Z80_Dual_YM2203/1943/dist/1943" "$REPO/Capcom_Z80_Dual_YM2203/1943/dist/1943.info" "$REPO/Capcom_Z80_Dual_YM2203/1943/assets" "$REPO/Capcom_Z80_Dual_YM2203/1943/assets/1943.png" \
        "1943" "Capcom Z80 / RTG" "Installed" "Battle of Midway port" "1987" "Capcom" ""
    stage_game "1943 Kai" "1943Kai" "$REPO/Capcom_Z80_Dual_YM2203/1943_Kai/dist/1943 Kai" "$REPO/Capcom_Z80_Dual_YM2203/1943_Kai/dist/1943 Kai.info" "$REPO/Capcom_Z80_Dual_YM2203/1943_Kai/assets" "$REPO/Capcom_Z80_Dual_YM2203/1943_Kai/assets/1943kai.png" \
        "1943 Kai" "Capcom Z80 / RTG" "Installed" "Japanese 1943 Kai set" "1987" "Capcom" ""
    stage_game "Black Tiger" "BlackTiger" "$REPO/Capcom_Z80_Dual_YM2203/Black_Tiger/dist/Black Tiger" "$REPO/Capcom_Z80_Dual_YM2203/Black_Tiger/dist/Black Tiger.info" "$REPO/Capcom_Z80_Dual_YM2203/Black_Tiger/assets" "$REPO/Capcom_Z80_Dual_YM2203/Black_Tiger/assets/blacktiger_loader.png" \
        "Black Tiger" "Capcom Z80 / RTG" "Installed" "Capcom platform board port" "1987" "Capcom" ""
    stage_game "Commando" "Commando" "$REPO/Capcom_Z80_Dual_YM2203/Commando/dist/Commando" "$REPO/Capcom_Z80_Dual_YM2203/Commando/dist/Commando.info" "$REPO/Capcom_Z80_Dual_YM2203/Commando/assets" "$REPO/Capcom_Z80_Dual_YM2203/Commando/assets/commando_loader.png" \
        "Commando" "Capcom Z80 / RTG" "Installed" "Run and gun board test port" "1985" "Capcom" ""
    stage_game "Gun.Smoke" "GunSmoke" "$REPO/Capcom_Z80_Dual_YM2203/Gun_Smoke/dist/Gun.Smoke" "$REPO/Capcom_Z80_Dual_YM2203/Gun_Smoke/dist/Gun.Smoke.info" "$REPO/Capcom_Z80_Dual_YM2203/Gun_Smoke/assets" "$REPO/Capcom_Z80_Dual_YM2203/Gun_Smoke/assets/gunsmoke_loader.png" \
        "Gun.Smoke" "Capcom Z80 / RTG" "Installed" "Western shooter board port" "1985" "Capcom" ""
    stage_game "Side Arms" "SideArms" "$REPO/Capcom_Z80_Dual_YM2203/Side_Arms/dist/Side Arms" "$REPO/Capcom_Z80_Dual_YM2203/Side_Arms/dist/Side Arms.info" "$REPO/Capcom_Z80_Dual_YM2203/Side_Arms/assets" "$REPO/Capcom_Z80_Dual_YM2203/Side_Arms/assets/sidearms_loader.png" \
        "Side Arms" "Capcom Z80 / RTG" "Installed" "Capcom side-scrolling shooter" "1986" "Capcom" ""

    stage_game "Galaga88" "Galaga88" "$REPO/Namco_6809_CUS30/Galaga88/dist/Galaga88" "$REPO/Namco_6809_CUS30/Galaga88/dist/Galaga88.info" "$REPO/Namco_6809_CUS30/Galaga88/assets" "$REPO/Namco_6809_CUS30/Galaga88/assets/galaga88_icon_preview.png" \
        "Galaga88" "Namco 6809 CUS30 / RTG" "Installed" "Namco shooter hardware port" "1987" "Namco" ""
    stage_game "Gaplus" "Gaplus" "$REPO/Namco_Gaplus/Gaplus/dist/Gaplus" "$REPO/Namco_Gaplus/Gaplus/dist/Gaplus.info" "$REPO/Namco_Gaplus/Gaplus/assets" "$REPO/Namco_Gaplus/Gaplus/assets/gaplus_loader.png" \
        "Gaplus" "Namco Gaplus / RTG" "Installed" "Namco Gaplus hardware port" "1984" "Namco" "$REPO/Namco_Gaplus/Gaplus/scores"
    stage_game "Metro-Cross" "MetroCross" "$REPO/Namco_6809_CUS30/Metro-Cross/dist/Metro-Cross" "$REPO/Namco_6809_CUS30/Metro-Cross/dist/Metro-Cross.info" "$REPO/Namco_6809_CUS30/Metro-Cross/assets" "$REPO/Namco_6809_CUS30/Metro-Cross/assets/metrocross_loader.png" \
        "Metro-Cross" "Namco 6809 CUS30 / RTG" "Installed" "Namco scrolling action port" "1985" "Namco" ""
    stage_game "Pac-Land" "PacLand" "$REPO/Namco_6809_CUS30/Pac-Land/dist/Pac-Land" "$REPO/Namco_6809_CUS30/Pac-Land/dist/Pac-Land.info" "$REPO/Namco_6809_CUS30/Pac-Land/assets" "$REPO/Namco_6809_CUS30/Pac-Land/assets/pacland_loader.png" \
        "Pac-Land" "Namco 6809 CUS30 / RTG" "Installed" "Namco side-scrolling platform port" "1984" "Namco" "$REPO/Namco_6809_CUS30/Pac-Land/scores"
    stage_game "Sky Kid" "SkyKid" "$REPO/Namco_6809_CUS30/Sky_Kid/dist/Sky Kid" "$REPO/Namco_6809_CUS30/Sky_Kid/dist/Sky Kid.info" "$REPO/Namco_6809_CUS30/Sky_Kid/assets" "$REPO/Namco_6809_CUS30/Sky_Kid/assets/skykid_loader.png" \
        "Sky Kid" "Namco 6809 CUS30 / RTG" "Installed" "Namco flight shooter port" "1985" "Namco" ""

    stage_game "Green Beret" "GreenBeret" "$REPO/Konami_GreenBeret_Hardware/Green_Beret_Rush_n_Attack/dist/Green Beret" "$REPO/Konami_GreenBeret_Hardware/Green_Beret_Rush_n_Attack/dist/Green Beret.info" "$REPO/Konami_GreenBeret_Hardware/Green_Beret_Rush_n_Attack/assets" "$REPO/Konami_GreenBeret_Hardware/Green_Beret_Rush_n_Attack/assets/greenberet_loader.png" \
        "Green Beret" "Konami Green Beret / RTG" "Installed" "Konami run and gun port" "1985" "Konami" ""
    stage_game "Jail Break" "JailBreak" "$REPO/Konami_GreenBeret_Hardware/Jail_Break/dist/Jail Break" "$REPO/Konami_GreenBeret_Hardware/Jail_Break/dist/Jail Break.info" "$REPO/Konami_GreenBeret_Hardware/Jail_Break/assets" "$REPO/Konami_GreenBeret_Hardware/Jail_Break/assets/jailbrek_rtg_bezel_preview.png" \
        "Jail Break" "Konami Green Beret / RTG" "Installed" "Konami action board port" "1986" "Konami" ""

    stage_game "Moon Cresta" "MoonCresta" "$REPO/Nichibutsu_MoonCresta_Hardware/Moon_Cresta/prebuilt/Moon Cresta" "$REPO/Nichibutsu_MoonCresta_Hardware/Moon_Cresta/prebuilt/Moon Cresta.info" "$REPO/Nichibutsu_MoonCresta_Hardware/Moon_Cresta/assets" "$REPO/Nichibutsu_MoonCresta_Hardware/Moon_Cresta/assets/mooncresta_loader.png" \
        "Moon Cresta" "Nichibutsu Moon Cresta / RTG" "Installed" "Nichibutsu shooter hardware port" "1980" "Nichibutsu" ""
    stage_game "Terra Cresta" "TerraCresta" "$REPO/Nichibutsu_TerraCresta_Hardware/Terra_Cresta/dist/Terra Cresta" "$REPO/Nichibutsu_TerraCresta_Hardware/Terra_Cresta/dist/Terra Cresta.info" "$REPO/Nichibutsu_TerraCresta_Hardware/Terra_Cresta/assets" "$REPO/Nichibutsu_TerraCresta_Hardware/Terra_Cresta/assets/terracre_loader.png" \
        "Terra Cresta" "Nichibutsu Terra Cresta / RTG" "Installed" "Nichibutsu shooter hardware port" "1985" "Nichibutsu" ""

    stage_game "Tiger Heli" "TigerHeli" "$REPO/Toaplan_TigerHeli_Hardware/Tiger_Heli/dist/Tiger Heli" "$REPO/Toaplan_TigerHeli_Hardware/Tiger_Heli/dist/Tiger Heli.info" "$REPO/Toaplan_TigerHeli_Hardware/Tiger_Heli/assets" "$REPO/Toaplan_TigerHeli_Hardware/Tiger_Heli/assets/tigerheli_loader.png" \
        "Tiger Heli" "Toaplan Tiger Heli / RTG" "Installed" "Toaplan helicopter shooter port" "1985" "Toaplan" "$REPO/Toaplan_TigerHeli_Hardware/Tiger_Heli/scores"
    stage_game "Ikari Warriors" "IkariWarriors" "$REPO/SNK_IkariWarriors_Hardware/Ikari_Warriors/dist/Ikari Warriors" "$REPO/SNK_IkariWarriors_Hardware/Ikari_Warriors/dist/Ikari Warriors.info" "$REPO/SNK_IkariWarriors_Hardware/Ikari_Warriors/assets" "$REPO/SNK_IkariWarriors_Hardware/Ikari_Warriors/assets/ikari_loader.png" \
        "Ikari Warriors" "SNK Ikari / RTG" "Installed" "SNK rotary shooter port" "1986" "SNK" "$REPO/SNK_IkariWarriors_Hardware/Ikari_Warriors/scores"

    stage_game "Saint Dragon" "SaintDragon" "$REPO/Jaleco_MegaSystem1/Saint_Dragon/dist/Saint Dragon" "$REPO/Jaleco_MegaSystem1/Saint_Dragon/dist/Saint Dragon.info" "$REPO/Jaleco_MegaSystem1/Saint_Dragon/assets" "$REPO/Jaleco_MegaSystem1/Saint_Dragon/assets/stdragon_loader.png" \
        "Saint Dragon" "Jaleco Mega System 1 / RTG" "Installed" "Jaleco shooter hardware port" "1989" "Jaleco" ""
    stage_game "R-Type" "RType" "$REPO/Irem_M72/R-Type/dist/R-Type" "$REPO/Irem_M72/R-Type/dist/R-Type.info" "$REPO/Irem_M72/R-Type/assets" "$REPO/Irem_M72/R-Type/assets/rtype_loader.png" \
        "R-Type" "Irem M72 / RTG" "Installed" "Irem M72 shooter hardware port" "1987" "Irem" ""
    stage_game "Shinobi" "Shinobi" "$REPO/Sega_System16/Shinobi/dist/Shinobi" "$REPO/Sega_System16/Shinobi/dist/Shinobi.info" "$REPO/Sega_System16/Shinobi/assets" "$REPO/Sega_System16/Shinobi/assets/shinobi_loader.png" \
        "Shinobi" "Sega System 16 / RTG" "Installed" "Sega System 16 action port" "1987" "Sega" ""
fi
printf ':Chase HQ\n' > "$BOOTROOT/.backdrop"
if [ -f "$BOOTROOT/Disk.info" ] && [ -f "$ICON_POS_TOOL" ]; then
    python3 "$ICON_POS_TOOL" "$BOOTROOT/Disk.info" 48 84
fi
if [ -f "$ICON_PNG" ]; then
    cp -f "$ICON_PNG" "$BOOTROOT/Prefs/Presets/ChaseHQ.png"
    if [ -f "$WBPATTERN_TOOL" ]; then
        python3 "$WBPATTERN_TOOL" "$B/WBPattern.prefs" "SYS:Prefs/Presets/ChaseHQ.png"
        cp -f "$B/WBPattern.prefs" "$BOOTROOT/Prefs/Env-Archive/Sys/WBPattern.prefs"
    fi
fi
if [ -f "$SCREENMODE_PREFS" ]; then
    cp -f "$SCREENMODE_PREFS" "$BOOTROOT/Prefs/Env-Archive/Sys/screenmode.prefs"
fi
if [ -f "$SYSTEM_CONFIGURATION" ]; then
    cp -f "$SYSTEM_CONFIGURATION" "$BOOTROOT/Devs/system-configuration"
fi
mkdir -p "$HD_DIR"
cp -a "$BOOTROOT"/. "$HD_DIR"/

echo "== write Amiberry RTG config =="
cat > "$UAE" <<EOF
; Chase HQ RTG PiMiga Workbench/Picasso96 config for Amiberry.
; Tech: RTG RGB888 painter, dual-68000 Musashi interpreter, predecoded TC0150ROD road,
; TC0100SCN tilemaps, zoom sprites, Z80 + YM2610 mixed to Paula.
; Boots a lean PiMiga-derived WhittyArcade Workbench HD folder with ArcadeLauncher.

[config]
config_description=WhittyArcade PiMiga RTG Workbench launcher
config_version=8.2.0
config_hardware=1
config_host=1
use_gui=no
amiberry.soundcard=1
amiberry.middle_mouse=true
amiberry.right_control_is_right_win=true
amiga_model=A1200
chipset=aga
chipset_compatible=A1200
cpu_type=68030
cpu_model=68030
cpu_compatible=false
cpu_cycle_exact=false
cpu_speed=max
address_space_24=false
cpu_24bit_addressing=false
ntsc=false
chipset_refreshrate=49.920410
comp_trustbyte=direct
comp_trustword=direct
comp_trustlong=direct
comp_trustnaddr=direct
comp_constjump=true
comp_flushmode=hard
cachesize=16384
chipmem_size=4
fastmem_size=8
z3mem_size=512
z3mem_start=0x40000000
bogomem_size=0
gfxcard_size=16
gfxcard_type=ZorroIII
gfxcard_hardware_vblank=false
gfxcard_hardware_sprite=false
gfxcard_multithread=false
gfxcard_zerocopy=true
rtg_nocustom=false
rtg_modes=0x3ffe
rtg_noautomodes=false
kickstart_rom_file=/home/jon/Amiberry/ROMs/kick40068.A1200.rom
nr_floppies=0
floppy0=
floppy0type=-1
floppy1=
floppy1type=-1
floppy2=
floppy2type=-1
floppy3=
floppy3type=-1
filesystem2=rw,DH0:WhittyArcade:$HD_DIR,0
uaehf0=dir,rw,DH0:WhittyArcade:$HD_DIR,0
gfx_display=0
gfx_display_rtg=0
gfx_framerate=1
gfx_width=$EMU_WINDOW_W
gfx_height=$EMU_WINDOW_H
gfx_x_windowed=64
gfx_y_windowed=48
gfx_width_windowed=$EMU_WINDOW_W
gfx_height_windowed=$EMU_WINDOW_H
gfx_width_fullscreen=$EMU_WINDOW_W
gfx_height_fullscreen=$EMU_WINDOW_H
gfx_fullscreen=0
gfx_fullscreen_amiga=false
gfx_fullscreen_picasso=false
gfx_linemode=none
gfx_center_horizontal=smart
gfx_center_vertical=smart
gfx_keep_aspect=true
gfx_colour_mode=32bit
gfx_api=sdl3
gfx_api_options=hardware
gfx_backbuffers=2
gfx_backbuffers_rtg=1
gfx_vsync=false
gfx_vsync_picasso=false
immediate_blits=true
sound=1
sound_output=exact
sound_channels=stereo
sound_frequency=44100
sound_interpol=anti
sound_volume=80
sound_auto=true
joyport0=mouse
joyport1=joy0
joyport1mode=cd32joy
input.config=0
input.joymouse_speed_analog=100
input.joymouse_speed_digital=10
input.joystick_deadzone=33
log_file=/tmp/amiberry-chasehq-rtg.log
log_console=1
fpu_model=68882
bsdsocket_emu=false
synchronize_clock=false
EOF

echo "== build executable zip =="
rm -rf "$PKGDIR"
mkdir -p "$PKGDIR"
cp -f "$EXE" "$PKGDIR/Chase HQ"
cp -f "$DIST_ICON" "$PKGDIR/Chase HQ.info"
cp -f "$ROOT/README.md" "$PKGDIR/ReadMe"
rm -f "$DIST_ZIP" "$ZIP"
(cd "$PKGROOT" && zip -q -r "$DIST_ZIP" "Chase HQ")
cp -f "$DIST_ZIP" "$ZIP"
ls -ld "$HD_DIR"
ls -l "$EXE" "$DIST_ICON" "$DIST_ZIP" "$UAE"
