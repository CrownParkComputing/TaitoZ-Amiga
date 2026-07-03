#!/usr/bin/env python3
# make_chq_rtg_bezel.py -- Chase H.Q. in-game RTG bezel (The Bezel Project).
#
# Emits an 864x486 8-bit RGB332 bezel baked into the exe via chq_romdata.S:
# byte 0 marks the 640x448 game window (hole) centred at (112,19); every other
# byte is the rgb332() of the resized bezel art. The pen8 presenter memcpy's it
# into the framebuffer once (blit_bezel) and refreshes only the game rect each
# frame, so the cabinet art fills the margins. Screen and source are both 16:9,
# so the resize is a clean uniform scale.
from pathlib import Path
from PIL import Image

HERE = Path(__file__).resolve().parent
SRC  = HERE / "assets" / "chasehq_bezel.png"
OUT  = HERE / "data" / "chq_rtg_bezel.bin"
PREVIEW = HERE / "data" / "chq_rtg_bezel_preview.png"

RTG_W, RTG_H = 864, 486
HOLE_X, HOLE_Y, HOLE_W, HOLE_H = 112, 0, 640, 486    # == GAME_OX/OY/W/H (fills full height)

def rgb332(r, g, b):
    return ((r >> 5) << 5) | ((g >> 5) << 2) | (b >> 6)

def rgb332_to_rgb(v):
    r = (v & 0xe0); g = (v & 0x1c) << 3; b = (v & 0x03) << 6
    r |= r >> 3 | r >> 6; g |= g >> 3 | g >> 6; b |= b >> 2 | b >> 4 | b >> 6
    return (r, g, b)

def main():
    im = Image.open(SRC).convert("RGBA").resize((RTG_W, RTG_H), Image.Resampling.LANCZOS)
    bg = Image.new("RGB", (RTG_W, RTG_H), (0, 0, 0))
    bg.paste(im, (0, 0), im)
    px = bg.load()
    out = bytearray(RTG_W * RTG_H)
    for y in range(RTG_H):
        for x in range(RTG_W):
            in_game = HOLE_X <= x < HOLE_X + HOLE_W and HOLE_Y <= y < HOLE_Y + HOLE_H
            out[y * RTG_W + x] = 0 if in_game else rgb332(*px[x, y])
    OUT.parent.mkdir(parents=True, exist_ok=True)
    OUT.write_bytes(out)
    prev = Image.new("RGB", (RTG_W, RTG_H))
    ppx = prev.load()
    for y in range(RTG_H):
        for x in range(RTG_W):
            v = out[y * RTG_W + x]
            in_game = HOLE_X <= x < HOLE_X + HOLE_W and HOLE_Y <= y < HOLE_Y + HOLE_H
            ppx[x, y] = (40, 40, 40) if (v == 0 and in_game) else rgb332_to_rgb(v)
    prev.save(PREVIEW)
    print(f"wrote {OUT} ({len(out)} bytes) + preview; hole {HOLE_W}x{HOLE_H}@{HOLE_X},{HOLE_Y}")

if __name__ == "__main__":
    main()
