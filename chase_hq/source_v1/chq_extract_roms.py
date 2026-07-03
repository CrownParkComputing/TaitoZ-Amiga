#!/usr/bin/env python3
"""chq_extract_roms.py -- Chase H.Q. (MAME `chasehq`) ROM decode.

Builds source/data/*.bin blobs straight from /home/jon/Downloads/chasehq.zip.
The load order mirrors MAME's taito_z.cpp ROM_START(chasehq).

Generated runtime blobs:
  maincpu.bin        0x80000  MC68000 A, ROM_LOAD16_BYTE interleaved
  sub.bin            0x20000  MC68000 B, ROM_LOAD16_BYTE interleaved
  audiocpu.bin       0x10000  Z80 sound CPU
  motorcpu.bin       0x08000  motor CPU ROM, stubbed for now
  gfx_scn.bin        0x80000  TC0100SCN, ROM_LOAD16_WORD_SWAP
  gfx_road.bin       0x80000  TC0150ROD road graphics
  sprites_asm.bin    0x200000 OBJ A, ROM_LOAD64_WORD_SWAP
  sprites2_asm.bin   0x200000 OBJ B, ROM_LOAD64_WORD_SWAP
  spritemap_asm.bin  0x80000  sprite lookup map
  adpcma.bin         0x180000 YM2610 ADPCM-A
  adpcmb.bin         0x80000  YM2610 ADPCM-B
  proms.bin          priority/control PROMs, concatenated for reference
"""
import os
import sys
import zipfile
import zlib

ZIP = "/home/jon/Downloads/chasehq.zip"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")

EXPECT = {
    "b52-130.36": (0x20000, 0x4e7beb46),
    "b52-136.29": (0x20000, 0x2f414df0),
    "b52-131.37": (0x20000, 0xaa945d83),
    "b52-129.30": (0x20000, 0x0eaebc08),
    "b52-132.39": (0x10000, 0xa2f54789),
    "b52-133.55": (0x10000, 0x12232f95),
    "b52-137.51": (0x10000, 0x37abb74a),
    "27c256.ic17": (0x08000, 0xe52dfee1),
    "b52-29.27": (0x80000, 0x8366d27c),
    "b52-34.5": (0x80000, 0x7d8dce36),
    "b52-35.7": (0x80000, 0x78eeec0d),
    "b52-36.9": (0x80000, 0x61e89e91),
    "b52-37.11": (0x80000, 0xf02e47b9),
    "b52-28.4": (0x80000, 0x963bc82b),
    "b52-30.4": (0x80000, 0x1b8cc647),
    "b52-31.6": (0x80000, 0xf1998e20),
    "b52-32.8": (0x80000, 0x8620780c),
    "b52-33.10": (0x80000, 0xe6f4b8c4),
    "b52-38.34": (0x80000, 0x5b5bf7f6),
    "b52-115.71": (0x80000, 0x4e117e93),
    "b52-114.72": (0x80000, 0x3a73d6b1),
    "b52-113.73": (0x80000, 0x2c6a3a05),
    "b52-116.70": (0x80000, 0xad46983c),
    "b52-01.7": (0x00100, 0x89719d17),
    "b52-03.135": (0x00400, 0xa3f8490d),
    "b52-06.24": (0x00100, 0xfbf81f30),
    "b52-18.93": (0x00100, 0x60bdaf1a),
    "b52-18a": (0x00100, 0x6271be0d),
    "b52-49.68": (0x02000, 0x60dd2ed1),
    "b52-50.66": (0x10000, 0xc189781c),
    "b52-51.65": (0x10000, 0x30cc1f79),
    "b52-126.136": (0x00400, 0xfa2f840e),
    "b52-127.156": (0x00400, 0x77682a4f),
}


def rd(zf, name):
    with zf.open(name) as f:
        data = bytearray(f.read())
    want_size, want_crc = EXPECT[name]
    got_crc = zlib.crc32(data) & 0xffffffff
    if len(data) != want_size or got_crc != want_crc:
        raise SystemExit(
            "bad ROM %-16s got %d/%08x want %d/%08x"
            % (name, len(data), got_crc, want_size, want_crc)
        )
    print("  [OK] %-16s %7dB crc %08x" % (name, len(data), got_crc))
    return data


def load16_byte(*roms):
    """ROM_LOAD16_BYTE image, supporting offset 0/1 and 0x40000/0x40001 pairs."""
    assert len(roms) % 2 == 0
    pair_bytes = sum(len(r) for r in roms[:2])
    out = bytearray(pair_bytes * (len(roms) // 2))
    for pair in range(len(roms) // 2):
        even = roms[pair * 2]
        odd = roms[pair * 2 + 1]
        assert len(even) == len(odd)
        base = pair * len(even) * 2
        out[base + 0 : base + len(even) * 2 : 2] = even
        out[base + 1 : base + len(odd) * 2 : 2] = odd
    return out


def load16_word_swap(rom):
    out = bytearray(len(rom))
    out[0::2] = rom[1::2]
    out[1::2] = rom[0::2]
    return out


def load64_word_swap(roms):
    n = len(roms[0])
    assert all(len(r) == n for r in roms)
    out = bytearray(n * 4)
    for plane, rom in enumerate(roms):
        off = plane * 2
        for i in range(n // 2):
            base = off + i * 8
            out[base + 0] = rom[2 * i + 1]
            out[base + 1] = rom[2 * i + 0]
    return out


def write(name, data, expect):
    if len(data) != expect:
        raise SystemExit("%s size %d, expected %d" % (name, len(data), expect))
    path = os.path.join(OUT, name)
    with open(path, "wb") as f:
        f.write(data)
    print("  wrote %-18s %8dB crc %08x" % (name, len(data), zlib.crc32(data) & 0xffffffff))


def main():
    os.makedirs(OUT, exist_ok=True)
    if not os.path.exists(ZIP):
        print("missing %s" % ZIP, file=sys.stderr)
        return 2

    with zipfile.ZipFile(ZIP) as zf:
        blobs = {name: rd(zf, name) for name in EXPECT}

    write(
        "maincpu.bin",
        load16_byte(blobs["b52-130.36"], blobs["b52-136.29"], blobs["b52-131.37"], blobs["b52-129.30"]),
        0x80000,
    )
    write("sub.bin", load16_byte(blobs["b52-132.39"], blobs["b52-133.55"]), 0x20000)
    write("audiocpu.bin", blobs["b52-137.51"], 0x10000)
    write("motorcpu.bin", blobs["27c256.ic17"], 0x08000)
    write("gfx_scn.bin", load16_word_swap(blobs["b52-29.27"]), 0x80000)
    write("gfx_road.bin", blobs["b52-28.4"], 0x80000)
    write(
        "sprites_asm.bin",
        load64_word_swap([blobs["b52-34.5"], blobs["b52-35.7"], blobs["b52-36.9"], blobs["b52-37.11"]]),
        0x200000,
    )
    write(
        "sprites2_asm.bin",
        load64_word_swap([blobs["b52-30.4"], blobs["b52-31.6"], blobs["b52-32.8"], blobs["b52-33.10"]]),
        0x200000,
    )
    write("spritemap_asm.bin", blobs["b52-38.34"], 0x80000)
    write("adpcma.bin", blobs["b52-115.71"] + blobs["b52-114.72"] + blobs["b52-113.73"], 0x180000)
    write("adpcmb.bin", blobs["b52-116.70"], 0x80000)
    write(
        "proms.bin",
        blobs["b52-01.7"]
        + blobs["b52-03.135"]
        + blobs["b52-06.24"]
        + blobs["b52-18.93"]
        + blobs["b52-18a"]
        + blobs["b52-49.68"]
        + blobs["b52-50.66"]
        + blobs["b52-51.65"]
        + blobs["b52-126.136"]
        + blobs["b52-127.156"],
        0x23000,
    )
    print("chasehq ROM extraction OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
