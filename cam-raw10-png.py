#!/usr/bin/env python3
"""Turn a frame saved by the sunfish camera driver (/private/var/root/cam-frame.raw: IMX363 2016x1512, MIPI-packed
RAW10: 2520 payload bytes a line, padded to 2528 by the VFE write master) into PNGs: a half-size colour image (2x2 Bayer quads -> one RGB pixel) and a half-size
grey one (the 2x2 mean, which does not depend on the Bayer order). Pure Python, no numpy.

usage: cam-raw10-png.py frame.raw [out-prefix] [--bayer RGGB|GRBG|GBRG|BGGR] [--black 64]
"""
import struct
import sys
import zlib

W, H, LINE, STRIDE = 2016, 1512, 2520, 2528     # the write master pads each line to 16 bytes


def png(path, w, h, rows, channels):
    def chunk(tag, data):
        c = struct.pack(">I", len(data)) + tag + data
        return c + struct.pack(">I", zlib.crc32(tag + data) & 0xffffffff)
    raw = b"".join(b"\x00" + bytes(r) for r in rows)
    color = 2 if channels == 3 else 0
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, color, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 6)) + chunk(b"IEND", b""))


def unpack_line(b):
    out = [0] * W
    j = 0
    for i in range(0, LINE, 5):
        lo = b[i + 4]
        out[j] = (b[i] << 2) | (lo & 3)
        out[j + 1] = (b[i + 1] << 2) | ((lo >> 2) & 3)
        out[j + 2] = (b[i + 2] << 2) | ((lo >> 4) & 3)
        out[j + 3] = (b[i + 3] << 2) | ((lo >> 6) & 3)
        j += 4
    return out


def main():
    args = [a for a in sys.argv[1:]]
    bayer, black = "RGGB", 64
    if "--bayer" in args:
        i = args.index("--bayer"); bayer = args[i + 1].upper(); del args[i:i + 2]
    if "--black" in args:
        i = args.index("--black"); black = int(args[i + 1]); del args[i:i + 2]
    src = args[0]
    prefix = args[1] if len(args) > 1 else src.rsplit(".", 1)[0]
    data = open(src, "rb").read()
    lines = min(H, len(data) // STRIDE)
    hw, hh = W // 2, lines // 2
    pos = {c: [] for c in "RGB"}
    for k, c in enumerate(bayer):           # quad positions: 0 top-left, 1 top-right, 2 bottom-left, 3 bottom-right
        pos[c].append(k)
    quads = []                              # (r, g, b, grey) per half-size pixel, black level removed
    sums = [0.0, 0.0, 0.0]
    for y in range(hh):
        a = unpack_line(data[(2 * y) * STRIDE:(2 * y + 1) * STRIDE])
        b = unpack_line(data[(2 * y + 1) * STRIDE:(2 * y + 2) * STRIDE])
        row = []
        for x in range(hw):
            q = (a[2 * x] - black, a[2 * x + 1] - black, b[2 * x] - black, b[2 * x + 1] - black)
            q = tuple(v if v > 0 else 0 for v in q)
            r = sum(q[k] for k in pos["R"]) / len(pos["R"])
            g = sum(q[k] for k in pos["G"]) / len(pos["G"])
            bl = sum(q[k] for k in pos["B"]) / len(pos["B"])
            row.append((r, g, bl, sum(q) / 4))
            sums[0] += r; sums[1] += g; sums[2] += bl
        quads.append(row)
    n = hw * hh
    mr, mg, mb = (s / n for s in sums)
    wr, wb = (mg / mr if mr else 1.0), (mg / mb if mb else 1.0)       # grey-world white balance
    greys = sorted(p[3] for row in quads for p in row)
    top = greys[int(len(greys) * 0.99)] or 1.0                          # stretch: 99th percentile -> white
    gamma = lambda v: int(255 * min(1.0, max(0.0, v / top)) ** (1 / 2.2) + 0.5)
    png(prefix + "-color.png", hw, hh, [[c for p in row for c in (gamma(p[0] * wr), gamma(p[1]), gamma(p[2] * wb))]
                                        for row in quads], 3)
    png(prefix + "-grey.png", hw, hh, [[gamma(p[3]) for p in row] for row in quads], 1)
    print(f"{lines} lines; raw means (black {black} removed) R {mr:.1f} G {mg:.1f} B {mb:.1f}; "
          f"wb R x{wr:.2f} B x{wb:.2f}; 99th pct {top:.0f}/{1023 - black}; wrote {prefix}-color.png, {prefix}-grey.png")


main()
