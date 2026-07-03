#!/usr/bin/env python3
"""Verify contcirc parent-set ROMs against MAME CRCs and build flat images.

Outputs (into out/):
  maincpu.bin   256K  big-endian 68000 program (ic25 even / ic26 odd interleaved)
  sub.bin       256K  big-endian 68000 sub program (ic35 even / cc_36 odd)
  audiocpu.bin   64K  Z80 sound program
  gfx_scn.bin   512K  TC0100SCN tilemap gfx (raw, decode in step 03)
  gfx_road.bin  512K  TC0150ROD road gfx (raw)
  spritemap.bin 512K  sprite zoom/lookup table (raw)
  sprites.bin     2M  4x512K sprite gfx, MAME load order (raw, decode in step 03)
  adpcma.bin      1M  YM2610 ADPCM-A samples
  adpcmb.bin    512K  YM2610 ADPCM-B samples
  proms.bin            color/priority PROMs concatenated
  user2.bin            shared Taito Z PROM/road tables concatenated

This is a faithful, lossless staging step: CPU images are interleaved to the
flat layout the CPU sees; everything else is staged raw with its decode deferred.
"""
import os, sys, zlib, json

HERE = os.path.dirname(os.path.abspath(__file__))
RAW = os.path.join(HERE, "raw")
OUT = os.path.join(HERE, "out")

# name -> (size, crc32) from `mame -listxml contcirc`
EXPECT = {
    "b33-ww.ic25": (0x20000, 0xf5c92e42),
    "b33-xx.ic26": (0x20000, 0xe7c1d1fa),
    "b33-yy.ic35": (0x20000, 0x16522f2d),
    "cc_36.bin":   (0x20000, 0xa1732ea5),
    "b33-30.11":   (0x10000, 0xd8746234),
    "b33-02.57":   (0x80000, 0xf6fb3ba2),
    "b33-06":      (0x80000, 0x2cb40599),
    "b33-05":      (0x80000, 0xbddf9eea),
    "b33-04":      (0x80000, 0x8df866a2),
    "b33-03":      (0x80000, 0x4f6c36d9),
    "b33-01.3":    (0x80000, 0xf11f2be8),
    "b33-07.64":   (0x80000, 0x151e1f52),
    "b33-09.18":   (0x80000, 0x1e6724b5),
    "b33-10.17":   (0x80000, 0xe9ce03ab),
    "b33-08.19":   (0x80000, 0xcaa1c4c8),
    "b14-30.97":   (0x10000, 0xdccb0c7f),
    "b14-31.50":   (0x2000,  0x5c6b013d),
    "b33-17.16":   (0x100,   0x7b7d8ff4),
    "b33-18.17":   (0x100,   0xfbf81f30),
}

def load(name):
    with open(os.path.join(RAW, name), "rb") as f:
        return f.read()

def interleave16(even, odd):
    """Big-endian 68000 image: even ROM -> high byte (offset 0), odd -> low."""
    assert len(even) == len(odd), (len(even), len(odd))
    out = bytearray(len(even) * 2)
    out[0::2] = even
    out[1::2] = odd
    return bytes(out)

def write(name, data):
    p = os.path.join(OUT, name)
    with open(p, "wb") as f:
        f.write(data)
    return p

def main():
    os.makedirs(OUT, exist_ok=True)
    report = {"verify": [], "outputs": []}
    ok = True
    blobs = {}
    for name, (size, crc) in EXPECT.items():
        data = load(name)
        got = zlib.crc32(data) & 0xffffffff
        good = (len(data) == size) and (got == crc)
        ok = ok and good
        blobs[name] = data
        report["verify"].append({
            "file": name, "size": len(data), "want_size": size,
            "crc": f"{got:08x}", "want_crc": f"{crc:08x}", "ok": good,
        })
        print(f"  [{'OK ' if good else 'BAD'}] {name:14s} {len(data):7d}B crc {got:08x} (want {crc:08x})")
    if not ok:
        print("\nERROR: ROM verification failed", file=sys.stderr)
        return 2

    outs = [
        ("maincpu.bin",   interleave16(blobs["b33-ww.ic25"], blobs["b33-xx.ic26"])),
        ("sub.bin",       interleave16(blobs["b33-yy.ic35"], blobs["cc_36.bin"])),
        ("audiocpu.bin",  blobs["b33-30.11"]),
        # b33-02 is ROM_LOAD16_WORD_SWAP in MAME (tc0100scn region) -> byteswap each word
        ("gfx_scn.bin",   bytes(b for pair in zip(blobs["b33-02.57"][1::2], blobs["b33-02.57"][0::2]) for b in pair)),
        ("gfx_road.bin",  blobs["b33-01.3"]),
        ("spritemap.bin", blobs["b33-07.64"]),
        # MAME "sprites" region load order: b33-06,05,04,03 at offsets 0,2,4,6
        ("sprites.bin",   blobs["b33-06"] + blobs["b33-05"] + blobs["b33-04"] + blobs["b33-03"]),
        ("adpcma.bin",    blobs["b33-09.18"] + blobs["b33-10.17"]),
        ("adpcmb.bin",    blobs["b33-08.19"]),
        ("proms.bin",     blobs["b33-17.16"] + blobs["b33-18.17"]),
        ("user2.bin",     blobs["b14-30.97"] + blobs["b14-31.50"]),
    ]
    print()
    for name, data in outs:
        p = write(name, data)
        c = zlib.crc32(data) & 0xffffffff
        report["outputs"].append({"file": name, "size": len(data), "crc": f"{c:08x}"})
        print(f"  wrote out/{name:14s} {len(data):7d}B crc {c:08x}")

    with open(os.path.join(OUT, "report.json"), "w") as f:
        json.dump(report, f, indent=2)
    print("\nstep01 OK -> out/report.json")
    return 0

if __name__ == "__main__":
    sys.exit(main())
