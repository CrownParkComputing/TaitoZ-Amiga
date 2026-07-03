#!/bin/bash
# Continental Circus RTG interpreter build (m68k-amigaos, bebbo gcc).
# Dual MC68000 (Musashi) + software renderer (TC0100SCN/TC0150ROD/sprites) +
# truecolour RTG presenter (cc_rtg_main.c) + YM2610/Paula sound.
# ROMs embedded (cc_romdata.S). Output: clean dist deliverables:
# executable, icon, executable zip, direct-boot HDF, and Amiberry UAE.
set -e
export PATH="/home/jon/amiga-amigaos/bin:$HOME/.local/bin:$PATH"
cd "$(dirname "$0")"
ROOT="$(cd .. && pwd)"
REPO="$(cd ../../.. && pwd)"
B=obj_rtg
DIST="$ROOT/dist"
EXE="$DIST/Continental Circus"
ICON="$ROOT/assets/continental_circus/Continental Circus.info"
DIST_ICON="$DIST/Continental Circus.info"
HDF="$DIST/Continental Circus.hdf"
UAE="$DIST/Continental Circus.uae"
DIST_ZIP="$DIST/Continental Circus.zip"
ZIP="$REPO/packages/zips/Continental Circus.zip"
PKGROOT="$ROOT/source/$B/package"
PKGDIR="$PKGROOT/Continental Circus"
mkdir -p "$B" "$DIST" "$PKGDIR" "$REPO/packages/zips" "$REPO/packages/hdf"
find "$DIST" -mindepth 1 -maxdepth 1 \
    ! -name "Continental Circus" \
    ! -name "Continental Circus.info" \
    ! -name "Continental Circus.zip" \
    ! -name "Continental Circus.hdf" \
    ! -name "Continental Circus.uae" \
    -exec rm -rf {} +
GCC="m68k-amigaos-gcc -m68030 -noixemul -O3 -fomit-frame-pointer -funroll-loops -DNDEBUG -I . -I cores -I cores/softfloat"
GCCYM="m68k-amigaos-gcc -m68030 -noixemul -O2 -fomit-frame-pointer -fno-strict-aliasing -DNDEBUG -DHAS_YM2610=1 -DCC_ADPCMA_NUM=8 -I . -I cores -I cores/ym"

echo "== Musashi core =="
$GCC -c cores/m68kcpu.c           -o "$B/m68kcpu.o"
$GCC -c cores/m68kops.c           -o "$B/m68kops.o"
$GCC -c cores/m68kdasm.c          -o "$B/m68kdasm.o"
$GCC -c cores/softfloat/softfloat.c -o "$B/softfloat.o"

echo "== machine + renderer + presenter =="
$GCC -c cc_machine.c -o "$B/cc_machine.o"
$GCC -c cc_render.c  -o "$B/cc_render.o"
$GCC -c cc_rtg_main.c -o "$B/cc_rtg_main.o"
$GCC -fno-builtin -c cc_mathstubs.c -o "$B/cc_mathstubs.o"

echo "== sound: Z80 + YM2610 (fm/ymdeltat) + Paula =="
$GCCYM -c cc_audio.c        -o "$B/cc_audio.o"
$GCCYM -c cores/ym/fm.c     -o "$B/fm.o"
$GCCYM -c cores/ym/ymdeltat.c -o "$B/ymdeltat.o"
$GCC   -c cores/z80.c       -o "$B/z80.o"
$GCC   -c cc_audio_amiga.c  -o "$B/cc_audio_amiga.o"

echo "== embedded ROM data =="
$GCC -c cc_romdata.S -o "$B/cc_romdata.o"

echo "== link =="
$GCC -o "$EXE" \
    "$B/cc_rtg_main.o" "$B/cc_machine.o" "$B/cc_render.o" \
    "$B/cc_audio.o" "$B/fm.o" "$B/ymdeltat.o" "$B/z80.o" "$B/cc_audio_amiga.o" "$B/cc_mathstubs.o" \
    "$B/m68kcpu.o" "$B/m68kops.o" "$B/m68kdasm.o" "$B/softfloat.o" \
    "$B/cc_romdata.o" -Wl,--start-group -lamiga -lgcc -Wl,--end-group
ls -l "$EXE" 2>/dev/null | awk '{print "exe:",$5,"bytes"}'

echo "== icon + root launcher =="
if [ ! -f "$ICON" ] && [ -f "$DIST_ICON" ]; then
    cp -f "$DIST_ICON" "$ICON"
fi
cp -f "$EXE" "$ROOT/Continental Circus"
cp -f "$ICON" "$DIST_ICON"
ls -l "$ROOT/Continental Circus" "$EXE" "$DIST_ICON"

echo "== build direct-boot HDF =="
cat > "$B/startup-sequence" <<EOF
; Continental Circus RTG direct boot.

C:SetPatch QUIET
C:Version >NIL:
FailAt 21

C:MakeDir RAM:T RAM:Clipboards RAM:ENV RAM:ENV/Sys
C:Copy >NIL: ENVARC: RAM:ENV ALL NOREQ

Resident >NIL: C:Assign PURE
Resident >NIL: C:Execute PURE

Assign >NIL: ENV: RAM:ENV
Assign >NIL: T: RAM:T
Assign >NIL: CLIPS: RAM:Clipboards
Assign >NIL: REXX: S:
Assign >NIL: PRINTERS: DEVS:Printers
Assign >NIL: KEYMAPS: DEVS:Keymaps
Assign >NIL: LOCALE: SYS:Locale
Assign >NIL: LIBS: SYS:Classes ADD

BindDrivers
C:Mount >NIL: DEVS:DOSDrivers/~(#?.info)

IF EXISTS DEVS:Monitors
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
EndIF

C:AddDataTypes REFRESH QUIET
C:IPrefs
C:ConClip
C:Wait 1 SECS

Path >NIL: RAM: C: SYS:Utilities SYS:System S: SYS:Prefs SYS:WBStartup SYS:Tools
Echo "Continental Circus RTG"
Stack 65536
SYS:ContCirc
IF EXISTS C:UAEquit
  C:UAEquit
EndIF
EndCLI >NIL:
EOF
rm -f "$HDF"
BASE_HDF="/home/jon/Amiberry/HardDrives/RTG_boot_template.hdf"
if [ ! -f "$BASE_HDF" ]; then BASE_HDF="/home/jon/Amiberry/HardDrives/SaintDragon_RTG.hdf"; fi
if [ ! -f "$BASE_HDF" ]; then BASE_HDF="/home/jon/Amiberry/HardDrives/RTG1.hdf"; fi
if [ -f "$BASE_HDF" ]; then
    cp -f "$BASE_HDF" "$HDF"
    xdftool "$HDF" delete S/startup-sequence >/dev/null 2>&1 || true
    xdftool "$HDF" delete cc_log.txt >/dev/null 2>&1 || true
    xdftool "$HDF" delete ContCirc >/dev/null 2>&1 || true
    xdftool "$HDF" delete ContCirc.info >/dev/null 2>&1 || true
    xdftool "$HDF" delete BlackTiger >/dev/null 2>&1 || true
    xdftool "$HDF" delete BlackTiger.info >/dev/null 2>&1 || true
    xdftool "$HDF" delete "Black Tiger" >/dev/null 2>&1 || true
    xdftool "$HDF" delete "Black Tiger.info" >/dev/null 2>&1 || true
    xdftool "$HDF" delete StDragon >/dev/null 2>&1 || true
    xdftool "$HDF" delete StDragon.info >/dev/null 2>&1 || true
    xdftool "$HDF" delete Commando >/dev/null 2>&1 || true
    xdftool "$HDF" delete Commando.info >/dev/null 2>&1 || true
    xdftool "$HDF" delete SideArms >/dev/null 2>&1 || true
    xdftool "$HDF" delete SideArms.info >/dev/null 2>&1 || true
    xdftool "$HDF" delete gunsmoke >/dev/null 2>&1 || true
    xdftool "$HDF" delete gunsmoke.info >/dev/null 2>&1 || true
    xdftool "$HDF" write "$EXE" ContCirc \
        + write "$DIST_ICON" ContCirc.info \
        + write "$B/startup-sequence" S/startup-sequence
else
    cat > "$B/startup-sequence" <<EOF
Stack 65536
ContCirc
EOF
    xdftool "$HDF" create size=10M \
        + format CONTCIRC ffs \
        + boot install \
        + write "$EXE" ContCirc \
        + write "$DIST_ICON" ContCirc.info \
        + makedir S \
        + write "$B/startup-sequence" S/startup-sequence
fi

echo "== write Amiberry RTG config =="
cat > "$UAE" <<EOF
; Continental Circus RTG direct-boot config for Amiberry.
; Tech: RTG RGB888 painter, dual-68000 Musashi interpreter, predecoded TC0150ROD road,
; TC0100SCN tilemaps, zoom sprites, Z80 + YM2610 mixed to Paula.

[config]
config_description=Continental Circus RTG Interpreter Painter - predecoded road
config_version=8.2.0
config_hardware=1
config_host=1
use_gui=no
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
ntsc=true
chipset_refreshrate=60.055992
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
floppy0type=-1
hardfile2=rw,DH0:/home/jon/Amiberry/HardDrives/ContinentalCircus.hdf,32,1,2,512,0,,uae0,0
uaehf0=hdf,rw,DH0:/home/jon/Amiberry/HardDrives/ContinentalCircus.hdf,32,1,2,512,0,,uae0,0
gfx_display=0
gfx_display_rtg=0
gfx_width=640
gfx_height=480
gfx_width_windowed=640
gfx_height_windowed=480
gfx_width_fullscreen=640
gfx_height_fullscreen=480
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
joyport0=none
joyport1=joy0
joyport1mode=cd32joy
input.config=0
input.joystick_deadzone=33
log_file=/tmp/amiberry-contcirc-rtg.log
log_console=1
fpu_model=68882
EOF

echo "== build executable zip =="
rm -rf "$PKGDIR"
mkdir -p "$PKGDIR"
cp -f "$EXE" "$PKGDIR/Continental Circus"
cp -f "$DIST_ICON" "$PKGDIR/Continental Circus.info"
cp -f "$ROOT/README.md" "$PKGDIR/ReadMe"
rm -f "$DIST_ZIP" "$ZIP"
(cd "$PKGROOT" && zip -q -r "$DIST_ZIP" "Continental Circus")
cp -f "$DIST_ZIP" "$ZIP"
cp -f "$HDF" "$REPO/packages/hdf/Continental Circus.hdf" 2>/dev/null || true
ls -l "$EXE" "$DIST_ICON" "$DIST_ZIP" "$HDF" "$UAE"
