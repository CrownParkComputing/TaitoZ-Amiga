#!/usr/bin/env python3
"""cc_extract_roms.py -- FRESH Continental Circus (MAME `contcirc`) ROM decode.

Re-extracts every ROM straight from /home/jon/Downloads/contcirc.zip and assembles
the data/*.bin blobs the Amiga build embeds (cc_romdata.S). Each blob is produced by
faithfully implementing the exact MAME ROM_LOAD type for that region, taken from
taito_z.cpp ROM_START(contcirc):

  maincpu   0x40000  b33-ww.ic25 + b33-xx.ic26   ROM_LOAD16_BYTE  (even/odd interleave, BE)
  sub       0x40000  b33-yy.ic35 + cc_36.bin     ROM_LOAD16_BYTE
  audiocpu  0x10000  b33-30.11                   ROM_LOAD         (raw)
  tc0100scn 0x80000  b33-02.57                   ROM_LOAD16_WORD_SWAP (byteswap each word)
  sprites   0x200000 b33-06/05/04/03             ROM_LOAD64_WORD_SWAP (planes interleaved)
  tc0150rod 0x80000  b33-01.3                     ROM_LOAD16_WORD (REGION16_LE, raw -> read LE)
  spritemap 0x80000  b33-07.64                    ROM_LOAD16_WORD (REGION16_LE, raw -> read LE)
  adpcma    0x100000 b33-09.18 + b33-10.17        ROM_LOAD (concatenated)
  adpcmb    0x80000  b33-08.19                    ROM_LOAD (raw)

The pixel decoders in cc_render.c expect exactly these byte layouts (see the
gfx_layout cross-check comments below).
"""
import sys, zipfile, os

ZIP  = "/home/jon/Downloads/contcirc.zip"
OUT  = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")
os.makedirs(OUT, exist_ok=True)

def rd(zf, name):
    with zf.open(name) as f:
        return bytearray(f.read())

def load16_byte(even, odd):
    """ROM_LOAD16_BYTE pair: dest[2i]=even[i], dest[2i+1]=odd[i] (68000 big-endian)."""
    assert len(even) == len(odd)
    out = bytearray(len(even) * 2)
    out[0::2] = even
    out[1::2] = odd
    return out

def load16_word_swap(rom):
    """ROM_LOAD16_WORD_SWAP: byteswap each 16-bit word."""
    out = bytearray(len(rom))
    out[0::2] = rom[1::2]
    out[1::2] = rom[0::2]
    return out

def load64_word_swap(roms, swap):
    """ROM_LOAD64_WORD_SWAP x4 at byte-offsets 0,2,4,6 within each 8-byte group.
       ROM_GROUPWORD|ROM_REVERSE|ROM_SKIP(6): for source word i, two bytes go to
       dest[off + i*8 + 0/1]; ROM_REVERSE swaps the two bytes within the word.
       `swap` toggles the within-word byte order (resolved empirically vs the
       MAME-validated reference blob)."""
    n = len(roms[0])
    assert all(len(r) == n for r in roms)
    out = bytearray(n * 4)              # 4 planes * 0x80000 = 0x200000
    for plane, rom in enumerate(roms):   # plane 0..3 -> dest byte offset plane*2
        off = plane * 2
        for i in range(n // 2):
            b0 = rom[2 * i]
            b1 = rom[2 * i + 1]
            base = off + i * 8
            if swap:
                out[base + 0] = b1
                out[base + 1] = b0
            else:
                out[base + 0] = b0
                out[base + 1] = b1
    return out

def write(name, data, expect=None):
    p = os.path.join(OUT, name)
    with open(p, "wb") as f:
        f.write(data)
    msg = "  %-20s %8d bytes" % (name, len(data))
    if expect is not None and len(data) != expect:
        msg += "  !! expected %d" % expect
    print(msg)

def main():
    zf = zipfile.ZipFile(ZIP)
    names = set(zf.namelist())

    # --- CPU code (ROM_LOAD16_BYTE interleave) ---
    write("maincpu.bin", load16_byte(rd(zf, "b33-ww.ic25"), rd(zf, "b33-xx.ic26")), 0x40000)
    write("sub.bin",     load16_byte(rd(zf, "b33-yy.ic35"), rd(zf, "cc_36.bin")),   0x40000)

    # --- Z80 sound CPU (raw) ---
    write("audiocpu.bin", rd(zf, "b33-30.11"), 0x10000)

    # --- TC0100SCN BG/FG/TX tiles (ROM_LOAD16_WORD_SWAP) ---
    #   gfx_8x8x4_packed_msb: 32 bytes/tile, high nibble = even pixel.
    write("gfx_scn.bin", load16_word_swap(rd(zf, "b33-02.57")), 0x80000)

    # --- TC0150ROD road gfx (REGION16_LE, raw; renderer reads LE words) ---
    write("gfx_road.bin", rd(zf, "b33-01.3"), 0x80000)

    # --- Sprites (ROM_LOAD64_WORD_SWAP); swap resolved below ---
    roms = [rd(zf, "b33-06"), rd(zf, "b33-05"), rd(zf, "b33-04"), rd(zf, "b33-03")]
    swap = os.environ.get("SPR_NOSWAP") is None    # default: swap on
    write("sprites_asm.bin", load64_word_swap(roms, swap), 0x200000)

    # --- Spritemap (REGION16_LE, raw; renderer reads LE words) ---
    write("spritemap_asm.bin", rd(zf, "b33-07.64"), 0x80000)

    # --- YM2610 ADPCM-A (concatenated) + ADPCM-B (raw) ---
    write("adpcma.bin", rd(zf, "b33-09.18") + rd(zf, "b33-10.17"), 0x100000)
    write("adpcmb.bin", rd(zf, "b33-08.19"), 0x80000)

    print("done.")

if __name__ == "__main__":
    main()
