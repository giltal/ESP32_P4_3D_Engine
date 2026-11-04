#!/usr/bin/env python3
"""Writes the sample models used by the examples:

    knot.stl   a (2,3) torus knot, binary STL, z up, about 3.8k triangles

Run it again only to change them; the files are in the repository.
"""
import math
import os
import struct

HERE = os.path.dirname(os.path.abspath(__file__))


def sub(a, b): return (a[0] - b[0], a[1] - b[1], a[2] - b[2])
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def dot(a, b): return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]
def norm(a):
    n = math.sqrt(dot(a, a)) or 1.0
    return (a[0] / n, a[1] / n, a[2] / n)


def knot(p=2, q=3, seg=160, sides=12, R=30.0, r=12.0, tube=5.5):
    def curve(t):
        k = R + r * math.cos(q * t)
        return (k * math.cos(p * t), k * math.sin(p * t), r * math.sin(q * t))
    rings = []
    for i in range(seg):
        t = 2 * math.pi * i / seg
        c, c2 = curve(t), curve(t + 1e-3)
        T = norm(sub(c2, c))
        N = norm(cross(T, (0.0, 0.0, 1.0)))
        B = cross(T, N)
        rings.append([tuple(c[k] + tube * (math.cos(2 * math.pi * j / sides) * N[k] +
                                          math.sin(2 * math.pi * j / sides) * B[k]) for k in range(3))
                      for j in range(sides)])
    tris = []
    for i in range(seg):
        a, b = rings[i], rings[(i + 1) % seg]
        for j in range(sides):
            j2 = (j + 1) % sides
            tris += [(a[j], b[j], b[j2]), (a[j], b[j2], a[j2])]
    # anticlockwise seen from outside: positive signed volume
    vol = sum(dot(t[0], cross(t[1], t[2])) for t in tris)
    if vol < 0:
        tris = [(t[0], t[2], t[1]) for t in tris]
    return tris


def write_stl(path, tris):
    with open(path, "wb") as f:
        f.write(b"e3d sample".ljust(80, b" "))
        f.write(struct.pack("<I", len(tris)))
        for t in tris:
            n = norm(cross(sub(t[1], t[0]), sub(t[2], t[0])))
            f.write(struct.pack("<12fH", *n, *t[0], *t[1], *t[2], 0))
    print(f"wrote {path}: {len(tris)} triangles")


if __name__ == "__main__":
    write_stl(os.path.join(HERE, "knot.stl"), knot())
