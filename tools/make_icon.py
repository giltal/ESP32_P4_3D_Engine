#!/usr/bin/env python3
"""Draws demo/icon.png (88x88): a lit cube on a dark tile. No dependencies."""
import os
import struct
import zlib

S = 88


def inside(px, py, poly):
    hit = False
    for i in range(len(poly)):
        (x0, y0), (x1, y1) = poly[i], poly[(i + 1) % len(poly)]
        if (y0 > py) != (y1 > py) and px < x0 + (py - y0) * (x1 - x0) / (y1 - y0):
            hit = not hit
    return hit


def main():
    cx, cy, r = 44, 46, 26
    top = [(cx, cy - r), (cx + r, cy - r // 2), (cx, cy), (cx - r, cy - r // 2)]
    left = [(cx - r, cy - r // 2), (cx, cy), (cx, cy + r), (cx - r, cy + r // 2)]
    right = [(cx, cy), (cx + r, cy - r // 2), (cx + r, cy + r // 2), (cx, cy + r)]
    faces = [(top, (255, 200, 90)), (left, (200, 40, 110)), (right, (110, 20, 70))]
    rows = []
    for y in range(S):
        row = bytearray([0])
        for x in range(S):
            t = y / (S - 1)
            c = (int(14 + 40 * t), int(16 + 20 * t), int(48 + 70 * t), 255)
            for poly, col in faces:
                if inside(x + 0.5, y + 0.5, poly):
                    c = col + (255,)
            row += bytes(c)
        rows.append(bytes(row))
    raw = b"".join(rows)

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data) & 0xFFFFFFFF)

    png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", S, S, 8, 6, 0, 0, 0))
    png += chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b"")
    out = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "demo", "icon.png")
    open(out, "wb").write(png)
    print(out)


if __name__ == "__main__":
    main()
