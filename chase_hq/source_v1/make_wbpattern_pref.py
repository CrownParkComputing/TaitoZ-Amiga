#!/usr/bin/env python3
import argparse
import struct
from pathlib import Path


def chunk(tag: bytes, payload: bytes) -> bytes:
    out = tag + struct.pack(">I", len(payload)) + payload
    if len(payload) & 1:
        out += b"\0"
    return out


def ptrn_with_path(kind: int, path: str) -> bytes:
    encoded = path.encode("latin-1")
    if len(encoded) > 255:
        raise ValueError("WBPattern path is too long")
    header = bytearray(24)
    header[17] = kind & 0xff
    header[23] = len(encoded)
    return chunk(b"PTRN", bytes(header) + encoded)


def blank_ptrn(kind: int) -> bytes:
    body = bytearray(280)
    body[17] = kind & 0xff
    body[20] = 1
    body[23] = 1
    return chunk(b"PTRN", bytes(body))


def main() -> None:
    ap = argparse.ArgumentParser()
    ap.add_argument("out", type=Path)
    ap.add_argument("path")
    args = ap.parse_args()

    payload = b"PREF"
    payload += chunk(b"PRHD", b"\0\0\0\0\0\0")
    payload += ptrn_with_path(0, args.path)
    payload += blank_ptrn(1)
    payload += blank_ptrn(2)
    args.out.write_bytes(chunk(b"FORM", payload))


if __name__ == "__main__":
    main()
