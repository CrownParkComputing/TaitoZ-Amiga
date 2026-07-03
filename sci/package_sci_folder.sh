#!/bin/bash
# Package the S.C.I. RTG build for Amiberry as a host-folder boot volume.
set -euo pipefail
export PATH="$HOME/.local/bin:$PATH"

HERE="$(cd "$(dirname "$0")" && pwd)"
ROOT="$(cd "$HERE/.." && pwd)"
BASE_HDF="/home/jon/Amiberry/HardDrives/RTG_boot_template.hdf"
EXE="$ROOT/dist/S.C.I. v2"
INST_DIR="/home/jon/Amiberry/HardDrives/SCI_RTG_HD"
INST_UAE="/home/jon/Amiberry/Configurations/SCI-RTG.uae"
TEMPLATE_UAE="/home/jon/Amiberry/Configurations/PowerDriftDemo-RTG.uae"

[ -f "$BASE_HDF" ] || { echo "missing base hdf: $BASE_HDF" >&2; exit 1; }
[ -f "$EXE" ] || { echo "missing S.C.I. executable: $EXE" >&2; exit 1; }
[ -f "$TEMPLATE_UAE" ] || { echo "missing template uae: $TEMPLATE_UAE" >&2; exit 1; }

mkdir -p "$ROOT/dist"

BUILD="$ROOT/dist/building_$$_SCI_RTG_HD"
SS="$BUILD/S/startup-sequence"
rm -rf "$BUILD" "$BUILD.blkdev" "$BUILD.bootcode" "$BUILD.xdfmeta"
xdftool "$BASE_HDF" unpack "$BUILD"
rm -f "$BUILD/SCI"
mkdir -p "$BUILD/S"
cp -f "$EXE" "$BUILD/SCI"

cat > "$SS" <<'EOF'
; S.C.I. direct RTG boot.

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
  IF EXISTS DEVS:Monitors/VGAOnly
    DEVS:Monitors/VGAOnly
  EndIF
  IF EXISTS DEVS:Monitors/uaegfx
    DEVS:Monitors/uaegfx
  ELSE
    IF EXISTS DEVS:Monitors/more/uaegfx
      DEVS:Monitors/more/uaegfx
    EndIF
  EndIF
  C:List >NIL: DEVS:Monitors/~(#?.info|VGAOnly|uaegfx) TO T:M LFORMAT "DEVS:Monitors/%s"
  Execute T:M
  C:Delete >NIL: T:M
EndIF

C:AddDataTypes REFRESH QUIET
C:IPrefs
C:ConClip
C:Wait 2 SECS

Path >NIL: RAM: C: SYS:Utilities SYS:System S: SYS:Prefs SYS:WBStartup SYS:Tools
Echo "S.C.I. - RTG native"
Stack 200000
Echo >ENV:WHITTY_NO_GAME_LOADER "1" NOLINE
SYS:SCI
C:UAEquit
EndCLI >NIL:
EOF

rm -rf "$ROOT/dist/SCI_RTG_HD"
cp -a "$BUILD" "$ROOT/dist/SCI_RTG_HD"
rm -rf "$INST_DIR"
cp -a "$BUILD" "$INST_DIR"
rm -rf "$BUILD" "$BUILD.blkdev" "$BUILD.bootcode" "$BUILD.xdfmeta"
rm -f "$ROOT/dist/SCI_RTG.hdf" "/home/jon/Amiberry/HardDrives/SCI_RTG.hdf"

sed \
  -e "1s|.*|; S.C.I. RTG direct-boot config for Amiberry.|" \
  -e "2s|.*|; Boots the S.C.I. native RTG executable and quits on exit.|" \
  -e "s|^config_description=.*|config_description=S.C.I. RTG direct boot|" \
  -e "s|^hardfile2=.*|filesystem2=rw,DH0:SCI_RTG:$INST_DIR,1|" \
  -e "s|^uaehf0=.*|uaehf0=dir,rw,DH0:SCI_RTG:$INST_DIR,1|" \
  -e "s|^gfx_width=.*|gfx_width=640|" \
  -e "s|^gfx_height=.*|gfx_height=480|" \
  -e "s|^gfx_width_windowed=.*|gfx_width_windowed=640|" \
  -e "s|^gfx_height_windowed=.*|gfx_height_windowed=480|" \
  -e "s|^gfx_width_fullscreen=.*|gfx_width_fullscreen=640|" \
  -e "s|^gfx_height_fullscreen=.*|gfx_height_fullscreen=480|" \
  -e "s|^log_file=.*|log_file=/tmp/amiberry-sci-rtg.log|" \
  "$TEMPLATE_UAE" > "$ROOT/dist/SCI-RTG.uae"
cp -f "$ROOT/dist/SCI-RTG.uae" "$INST_UAE"

echo "installed: $INST_DIR"
echo "installed: $INST_UAE"
