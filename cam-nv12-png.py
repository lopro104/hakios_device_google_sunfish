#!/usr/bin/env python3
"""Turn a picture kept by camera_test on the Pixel 4a (/private/var/root/cam-keep-WxH-2.bin: NV12, BT.601 limited range,
as src/camera_isp.c renders it) into a PNG. Pure Python, no numpy.

usage: cam-nv12-png.py cam-keep-820x616-2.bin [out.png] [--stride N] [--rotate 90|180|270]
"""
import re
import struct
import sys
import zlib


def png(path, w, h, rows):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def main():
    args = sys.argv[1:]
    stride, rotate = None, 0
    if "--stride" in args:
        i = args.index("--stride"); stride = int(args[i + 1]); del args[i:i + 2]
    if "--rotate" in args:
        i = args.index("--rotate"); rotate = int(args[i + 1]); del args[i:i + 2]
    src = args[0]
    out = args[1] if len(args) > 1 else src.rsplit(".", 1)[0] + ".png"
    m = re.search(r"(\d+)x(\d+)", src)
    w, h = int(m.group(1)), int(m.group(2))
    data = open(src, "rb").read()
    if stride is None:
        stride = len(data) * 2 // (3 * h)               # Y plane + half-height UV plane, same stride
    clip = lambda v: 0 if v < 0 else 255 if v > 255 else int(v)
    rgb = []
    for y in range(h):
        yrow = data[y * stride:y * stride + w]
        uv = data[stride * h + (y // 2) * stride:]
        row = []
        for x in range(w):
            Y = 1.164 * (yrow[x] - 16)
            U = uv[(x // 2) * 2] - 128
            V = uv[(x // 2) * 2 + 1] - 128
            row.append((clip(Y + 1.596 * V), clip(Y - 0.392 * U - 0.813 * V), clip(Y + 2.017 * U)))
        rgb.append(row)
    for _ in range(rotate // 90 % 4):                   # clockwise quarter turns
        rgb = [list(r) for r in zip(*rgb[::-1])]
    png(out, len(rgb[0]), len(rgb), [[c for p in r for c in p] for r in rgb])
    print(f"{w}x{h} stride {stride} -> {out} ({len(rgb[0])}x{len(rgb)})")


main()
