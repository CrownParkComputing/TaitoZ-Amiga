#!/usr/bin/env python3
"""Extract S.C.I. / Special Criminal Investigation parent-set ROM blobs.

The ROM load layout follows the local Taito Z driver ROM_START(sci). MAME is
used here as a manifest and hardware map, not as the renderer quality target.
"""
import os
import shutil
import sys
import zipfile
import zlib

ZIP = "/home/jon/Downloads/sci.zip"
OUT = os.path.join(os.path.dirname(os.path.abspath(__file__)), "data")

EXPECT = {
    "c09-37.43": (0x20000, 0x0fecea17),
    "c09-38.40": (0x20000, 0xe46ebd9b),
    "c09-42.38": (0x20000, 0xf4404f87),
    "c09-39.41": (0x20000, 0xde87bcb9),
    "c09-33.6": (0x10000, 0xcf4e6c5b),
    "c09-32.5": (0x10000, 0xa4713719),
    "c09-34.31": (0x20000, 0xa21b3151),
    "c09-05.16": (0x80000, 0x890b38f0),
    "c09-04.52": (0x80000, 0x2cbb3c9b),
    "c09-02.54": (0x80000, 0xa83a0389),
    "c09-03.53": (0x80000, 0xa31d0e80),
    "c09-01.55": (0x80000, 0x64bfea10),
    "c09-07.15": (0x80000, 0x963bc82b),
    "c09-06.37": (0x80000, 0x12df6d7b),
    "c09-14.42": (0x80000, 0xad78bf46),
    "c09-13.43": (0x80000, 0xd57c41d3),
    "c09-12.44": (0x80000, 0x56c99fa5),
    "c09-15.29": (0x80000, 0xe63b9095),
    "c09-16.17": (0x10000, 0x7245a6f6),
    "c09-17.24": (0x00400, 0x10728853),
    "c09-18.25": (0x00400, 0x643e8bfc),
    "c09-20.71": (0x00100, 0xcd8ffd80),
    "c09-23.14": (0x00100, 0xfbf81f30),
    "c09-19_pal16l8b.ic67": (0x00104, 0xa0608442),
    "c09-21_pal20l8b.ic2": (0x00144, 0x583f9214),
    "c09-22_pal16l8b.ic3": (0x00104, 0xb506d7a7),
    "c09-24_pal20l8b.ic22": (0x00144, 0x2ff83694),
    "c09-25_pal20l8b.ic25": (0x00144, 0xc69bf3fc),
    "c09-26_pal16l8b.ic26": (0x00104, 0x36a8eb27),
}


def rd(zf, name):
    with zf.open(name) as f:
        data = bytearray(f.read())
    want_size, want_crc = EXPECT[name]
    got_crc = zlib.crc32(data) & 0xffffffff
    if len(data) != want_size or got_crc != want_crc:
        raise SystemExit(
            "bad ROM %-22s got %d/%08x want %d/%08x"
            % (name, len(data), got_crc, want_size, want_crc)
        )
    print("  [OK] %-22s %7dB crc %08x" % (name, len(data), got_crc))
    return data


def load16_byte(*roms):
    assert len(roms) % 2 == 0
    out = bytearray(sum(len(r) for r in roms))
    base = 0
    for i in range(0, len(roms), 2):
        even, odd = roms[i], roms[i + 1]
        assert len(even) == len(odd)
        out[base : base + len(even) * 2 : 2] = even
        out[base + 1 : base + len(odd) * 2 : 2] = odd
        base += len(even) * 2
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


def write_compat_assets():
    src = "/home/jon/AmigaArcadePorts/Taito_Z_System/Chase_HQ/source/data"
    assets = {
        "cc_pal256.bin": 768,
        "cc_lut32k.bin": 32768,
        "cc_bezel.bin": 81920,
        "cc_bezelpal.bin": 48,
    }
    for name, size in assets.items():
        dst = os.path.join(OUT, name)
        candidate = os.path.join(src, name)
        if os.path.exists(candidate) and os.path.getsize(candidate) == size:
            shutil.copyfile(candidate, dst)
            print("  copied %-18s %8dB" % (name, size))
        else:
            write(name, bytearray(size), size)


def main():
    os.makedirs(OUT, exist_ok=True)
    if not os.path.exists(ZIP):
        print("missing %s" % ZIP, file=sys.stderr)
        return 2

    with zipfile.ZipFile(ZIP) as zf:
        blobs = {name: rd(zf, name) for name in EXPECT}

    write(
        "maincpu.bin",
        load16_byte(blobs["c09-37.43"], blobs["c09-38.40"], blobs["c09-42.38"], blobs["c09-39.41"]),
        0x80000,
    )
    write("sub.bin", load16_byte(blobs["c09-33.6"], blobs["c09-32.5"]), 0x20000)
    write("audiocpu.bin", blobs["c09-34.31"], 0x20000)
    write("gfx_scn.bin", load16_word_swap(blobs["c09-05.16"]), 0x80000)
    write("gfx_road.bin", blobs["c09-07.15"], 0x80000)
    sprites = load64_word_swap([blobs["c09-04.52"], blobs["c09-02.54"], blobs["c09-03.53"], blobs["c09-01.55"]])
    write("sprites_asm.bin", sprites, 0x200000)
    write("sprites2_asm.bin", sprites, 0x200000)
    write("spritemap_asm.bin", blobs["c09-06.37"], 0x80000)
    write("adpcma.bin", blobs["c09-14.42"] + blobs["c09-13.43"] + blobs["c09-12.44"], 0x180000)
    write("adpcmb.bin", blobs["c09-15.29"], 0x80000)
    proms = (
        blobs["c09-16.17"]
        + blobs["c09-17.24"]
        + blobs["c09-18.25"]
        + blobs["c09-20.71"]
        + blobs["c09-23.14"]
        + blobs["c09-19_pal16l8b.ic67"]
        + blobs["c09-21_pal20l8b.ic2"]
        + blobs["c09-22_pal16l8b.ic3"]
        + blobs["c09-24_pal20l8b.ic22"]
        + blobs["c09-25_pal20l8b.ic25"]
        + blobs["c09-26_pal16l8b.ic26"]
    )
    write("proms.bin", proms + bytearray(0x23000 - len(proms)), 0x23000)
    write("motorcpu.bin", bytearray(0x8000), 0x8000)
    write_compat_assets()
    print("sci ROM extraction OK")
    return 0


if __name__ == "__main__":
    sys.exit(main())
