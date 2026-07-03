#!/usr/bin/env python3
import argparse
import struct
from pathlib import Path


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("icon", type=Path)
    ap.add_argument("x", type=int)
    ap.add_argument("y", type=int)
    args = ap.parse_args()

    data = bytearray(args.icon.read_bytes())
    if len(data) < 12 or data[0:2] != b"\xe3\x10":
        raise SystemExit(f"{args.icon}: not a classic Workbench icon")

    struct.pack_into(">hh", data, 8, args.x, args.y)
    args.icon.write_bytes(data)


if __name__ == "__main__":
    main()
