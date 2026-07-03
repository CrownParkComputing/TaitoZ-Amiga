#!/usr/bin/env bash
# Extract the contcirc (Continental Circus, World) parent-set ROMs into raw/.
# Clones (contcircj/, contcircu/, contcircua/) live in subdirs of the zip and
# are ignored here -- we only need the parent set.
set -euo pipefail
cd "$(dirname "$0")"

ZIP=../../roms/contcirc.zip
mkdir -p raw
# -j junk paths so clone subdir files would collide; instead extract only the
# top-level (parent) members explicitly.
PARENT_FILES=(
  b33-ww.ic25 b33-xx.ic26          # maincpu  (68000, even/odd)
  b33-yy.ic35 cc_36.bin            # sub      (68000, even/odd)
  b33-30.11                        # audiocpu (Z80)
  b33-02.57                        # tc0100scn tilemap gfx
  b33-06 b33-05 b33-04 b33-03      # sprites (4x 512K, interleaved)
  b33-07.64                        # spritemap (sprite zoom/lookup)
  b33-01.3                         # tc0150rod road gfx
  b33-09.18 b33-10.17              # ymsnd adpcm-A
  b33-08.19                        # ymsnd adpcm-B
  b14-30.97 b14-31.50              # user2 (shared Taito Z PROMs/road tables)
  b33-17.16 b33-18.17              # color/priority PROMs
)
for f in "${PARENT_FILES[@]}"; do
  unzip -o -j "$ZIP" "$f" -d raw >/dev/null
done
echo "extracted ${#PARENT_FILES[@]} parent-set files into raw/"
ls -l raw
