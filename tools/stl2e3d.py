#!/usr/bin/env python3
"""stl2e3d - turn STL (or OBJ) models into e3d objects.

    python tools/stl2e3d.py model.stl -o model.txt
    python tools/stl2e3d.py model.stl --header model.h --size 200 --color FF8020
    python tools/stl2e3d.py a.stl b.obj -o scene.txt --max-tris 3000

Reads binary and ASCII STL and Wavefront OBJ (v and f lines; faces with more
than three corners are fanned). Writes:

  -o FILE.txt     the e3d object text format: e3d_load_text / e3d_load_file
                  (one file holds every input, one object each)
  --header FILE.h a C header with the arrays and a <name>_create() function per
                  object that builds the mesh with e3d_mesh_create / e3d_mesh_finish

What it does to each model, in order:
  1. axes: e3d has y up, x as depth (away from the camera) and z to the right.
     --axes zup (the default for STL: CAD z-up, y away) maps source (x, y, z) to
     e3d (y, z, x); --axes yup (the default for OBJ: y up, z towards the viewer)
     maps it to (-z, y, x); --axes e3d keeps it. Or give three signed source
     axes for e3d x, y, z, such as --axes=+y,+z,-x.
  2. welding: corners closer than --weld (a share of the model's size) become
     one vertex, so smooth normals and the closed-mesh checks work. STL stores
     every triangle's corners separately, so this is what joins them.
  3. size: --scale multiplies; --size makes the largest extent that many units.
  4. placement: --center (default) puts the middle of the bounds at the origin;
     --base also puts the lowest point at y = 0 (for objects standing on a floor);
     --no-center leaves it.
  5. decimation: --max-tris N clusters vertices on a grid, as fine as still
     gives at most N triangles. A mesh over the engine's limit (21845 triangles,
     65535 vertices) is reduced to it.
  6. winding: STL and OBJ wind anticlockwise seen from outside; e3d's outward
     normal is (v0 - v1) x (v2 - v1), so corners are reordered. --flip turns
     them the other way. (The engine also turns closed meshes outward itself.)

Colour: --color RRGGBB for every triangle (default B4B4B4), or --stl-colors to
use the per-facet colours some binary STL writers store (VisCAM/SolidView: bit
15 set, red in 10..14, green 5..9, blue 0..4).
"""
import argparse
import math
import os
import re
import struct
import sys

MAX_TRIS = 21845      # e3d_mesh_create: 3 corner normals per triangle fit 16 bits
MAX_VERTS = 65535

PRESETS = {"zup": "+y,+z,+x", "yup": "-z,+y,+x", "e3d": "+x,+y,+z"}


# ── Reading ───────────────────────────────────────────────────────────────────

def read_stl(path):
    """-> (triangles as [(p0, p1, p2)], per-triangle (r, g, b) or None)"""
    data = open(path, "rb").read()
    if len(data) >= 84:
        n = struct.unpack_from("<I", data, 80)[0]
        if 84 + 50 * n == len(data):
            return read_stl_binary(data, n)
    try:
        text = data.decode("ascii")
    except UnicodeDecodeError:
        sys.exit(f"stl2e3d: {path}: neither a binary nor an ASCII STL")
    return read_stl_ascii(text, path)


def read_stl_binary(data, n):
    tris, cols = [], []
    for i in range(n):
        f = struct.unpack_from("<12fH", data, 84 + 50 * i)
        tris.append((f[3:6], f[6:9], f[9:12]))
        a = f[12]
        if a & 0x8000:
            cols.append((((a >> 10) & 31) * 255 // 31, ((a >> 5) & 31) * 255 // 31, (a & 31) * 255 // 31))
        else:
            cols.append(None)
    return tris, cols


def read_stl_ascii(text, path):
    nums = r"([-+0-9.eE]+)\s+([-+0-9.eE]+)\s+([-+0-9.eE]+)"
    verts = [tuple(float(v) for v in m) for m in re.findall(r"vertex\s+" + nums, text)]
    if not verts or len(verts) % 3:
        sys.exit(f"stl2e3d: {path}: no triangles found")
    tris = [tuple(verts[i:i + 3]) for i in range(0, len(verts), 3)]
    return tris, [None] * len(tris)


def read_obj(path):
    pos, tris = [], []
    for line in open(path, encoding="utf-8", errors="replace"):
        parts = line.split()
        if not parts:
            continue
        if parts[0] == "v":
            pos.append(tuple(float(v) for v in parts[1:4]))
        elif parts[0] == "f":
            idx = []
            for c in parts[1:]:
                i = int(c.split("/")[0])
                idx.append(i - 1 if i > 0 else len(pos) + i)
            for k in range(1, len(idx) - 1):
                tris.append((pos[idx[0]], pos[idx[k]], pos[idx[k + 1]]))
    if not tris:
        sys.exit(f"stl2e3d: {path}: no faces found")
    return tris, [None] * len(tris)


# ── Processing ────────────────────────────────────────────────────────────────

def parse_axes(spec):
    spec = PRESETS.get(spec, spec)
    out = []
    for item in spec.split(","):
        item = item.strip().lower()
        sign = -1.0 if item.startswith("-") else 1.0
        axis = item.lstrip("+-")
        if axis not in ("x", "y", "z") or len(spec.split(",")) != 3:
            sys.exit(f"stl2e3d: bad --axes {spec!r}: use zup, yup, e3d or three of +x,-y,... ")
        out.append((sign, "xyz".index(axis)))
    if sorted(a for _, a in out) != [0, 1, 2]:
        sys.exit(f"stl2e3d: --axes {spec!r} uses an axis twice")
    # the determinant of the mapping: a mirror turns windings over
    m = [[0.0] * 3 for _ in range(3)]
    for row, (s, a) in enumerate(out):
        m[row][a] = s
    det = (m[0][0] * (m[1][1] * m[2][2] - m[1][2] * m[2][1]) - m[0][1] * (m[1][0] * m[2][2] - m[1][2] * m[2][0])
           + m[0][2] * (m[1][0] * m[2][1] - m[1][1] * m[2][0]))
    return out, det < 0


def bounds(pts):
    lo = [min(p[k] for p in pts) for k in range(3)]
    hi = [max(p[k] for p in pts) for k in range(3)]
    return lo, hi


def weld(tris, cols, tol):
    """Corners within tol share a vertex. -> (pos, [(i0, i1, i2, rgb)])"""
    cell = tol if tol > 0 else 1e-9
    index, pos, faces = {}, [], []
    for t, c in zip(tris, cols):
        ids = []
        for p in t:
            key = (round(p[0] / cell), round(p[1] / cell), round(p[2] / cell))
            i = index.get(key)
            if i is None:
                i = index[key] = len(pos)
                pos.append(p)
            ids.append(i)
        if ids[0] != ids[1] and ids[1] != ids[2] and ids[0] != ids[2]:
            faces.append((ids[0], ids[1], ids[2], c))
    return pos, faces


def compact(pos, faces):
    """Drop unused vertices and repeated triangles."""
    seen, out, remap, newpos = set(), [], {}, []
    for f in faces:
        key = tuple(sorted(f[:3]))
        if key in seen:
            continue
        seen.add(key)
        ids = []
        for i in f[:3]:
            if i not in remap:
                remap[i] = len(newpos)
                newpos.append(pos[i])
            ids.append(remap[i])
        out.append((ids[0], ids[1], ids[2], f[3]))
    return newpos, out


def cluster(pos, faces, grid, lo, size):
    """Vertex clustering on a grid x grid x grid lattice over the bounds."""
    step = size / grid
    cells, members, vmap = {}, [], []
    for p in pos:
        key = tuple(int((p[k] - lo[k]) / step) for k in range(3))
        c = cells.get(key)
        if c is None:
            c = cells[key] = len(members)
            members.append([0.0, 0.0, 0.0, 0])
        acc = members[c]
        acc[0] += p[0]
        acc[1] += p[1]
        acc[2] += p[2]
        acc[3] += 1
        vmap.append(c)
    newpos = [(a[0] / a[3], a[1] / a[3], a[2] / a[3]) for a in members]
    newfaces = []
    for f in faces:
        a, b, c = vmap[f[0]], vmap[f[1]], vmap[f[2]]
        if a != b and b != c and a != c:
            newfaces.append((a, b, c, f[3]))
    return compact(newpos, newfaces)


def decimate(pos, faces, max_tris, name):
    if len(faces) <= max_tris and len(pos) <= MAX_VERTS:
        return pos, faces
    lo, hi = bounds(pos)
    size = max(hi[k] - lo[k] for k in range(3)) or 1.0
    best = None
    g_lo, g_hi = 2, 4096
    while g_lo <= g_hi:                    # the finest grid that fits
        g = (g_lo + g_hi) // 2
        p2, f2 = cluster(pos, faces, g, lo, size * 1.000001)
        if len(f2) <= max_tris and len(p2) <= MAX_VERTS:
            best = (p2, f2)
            g_lo = g + 1
        else:
            g_hi = g - 1
    if best is None:
        sys.exit(f"stl2e3d: {name}: cannot reduce to {max_tris} triangles")
    print(f"  decimated {len(faces)} -> {len(best[1])} triangles (grid {g_hi})")
    return best


def open_edges(faces):
    count = {}
    for f in faces:
        for k in range(3):
            a, b = f[k], f[(k + 1) % 3]
            e = (a, b) if a < b else (b, a)
            count[e] = count.get(e, 0) + 1
    return sum(1 for v in count.values() if v != 2)


def convert(path, args):
    name = c_name(os.path.splitext(os.path.basename(path))[0])
    is_obj = path.lower().endswith(".obj")
    tris, cols = read_obj(path) if is_obj else read_stl(path)
    print(f"{path}: {len(tris)} triangles")

    axes, mirror = parse_axes(args.axes or ("yup" if is_obj else "zup"))
    tris = [tuple(tuple(s * p[a] for s, a in axes) for p in t) for t in tris]

    lo, hi = bounds([p for t in tris for p in t])
    extent = max(hi[k] - lo[k] for k in range(3)) or 1.0
    pos, faces = weld(tris, cols, extent * args.weld)
    pos, faces = compact(pos, faces)

    k = args.scale
    if args.size:
        k = args.size / extent
    lo, hi = bounds(pos)
    off = [0.0, 0.0, 0.0]
    if not args.no_center:
        off = [-(lo[i] + hi[i]) / 2 * k for i in range(3)]
        if args.base:
            off[1] = -lo[1] * k
    pos = [tuple(p[i] * k + off[i] for i in range(3)) for p in pos]

    pos, faces = decimate(pos, faces, min(args.max_tris or MAX_TRIS, MAX_TRIS), name)

    base = tuple(int(args.color[i:i + 2], 16) for i in (0, 2, 4))
    flip = not mirror if not args.flip else mirror   # STL/OBJ anticlockwise -> e3d order
    out = []
    for f in faces:
        rgb = f[3] if (args.stl_colors and f[3]) else base
        out.append((f[0], f[2], f[1], rgb) if flip else (f[0], f[1], f[2], rgb))

    lo, hi = bounds(pos)
    holes = open_edges(out)
    print(f"  {len(out)} triangles, {len(pos)} vertices, bounds "
          f"x {lo[0]:.1f}..{hi[0]:.1f} y {lo[1]:.1f}..{hi[1]:.1f} z {lo[2]:.1f}..{hi[2]:.1f}"
          + (f", {holes} open edges" if holes else ", closed"))
    return name, pos, out


def c_name(s):
    s = re.sub(r"[^0-9a-zA-Z_]", "_", s)
    return ("m_" + s) if s[:1].isdigit() else s


# ── Writing ───────────────────────────────────────────────────────────────────

def fmt(v):
    s = f"{v:.4f}".rstrip("0").rstrip(".")
    return "0" if s in ("-0", "") else s


def cf(v):
    """a C float literal"""
    s = fmt(v)
    return (s if "." in s else s + ".0") + "f"


def write_text(path, objects):
    lines = [str(len(objects))]
    for _, pos, faces in objects:
        lines.append(str(len(faces)))
        lines += [f"{a} {b} {c} {r} {g} {bl}" for a, b, c, (r, g, bl) in faces]
        lines.append(str(len(pos)))
        lines += [f"{fmt(p[0])} {fmt(p[1])} {fmt(p[2])}" for p in pos]
    open(path, "w", encoding="ascii", newline="\n").write("\n".join(lines) + "\n")
    print(f"wrote {path}")


def write_header(path, objects, crease, sources):
    guard = [f"/* generated by tools/stl2e3d.py from {', '.join(os.path.basename(s) for s in sources)} */",
             "#pragma once", "", "#include <string.h>", '#include "e3d.h"', ""]
    body = []
    for name, pos, faces in objects:
        one = len({f[3] for f in faces}) == 1
        body.append(f"/* {name}: {len(faces)} triangles, {len(pos)} vertices */")
        body.append(f"static const float {name}_pos[{len(pos)}][3] = {{")
        body += [f"    {{ {cf(p[0])}, {cf(p[1])}, {cf(p[2])} }}," for p in pos]
        body.append("};")
        body.append(f"static const uint16_t {name}_tri[{len(faces)}][3] = {{")
        row = []
        for f in faces:
            row.append(f"{{ {f[0]}, {f[1]}, {f[2]} }}")
            if len(row) == 6:
                body.append("    " + ", ".join(row) + ",")
                row = []
        if row:
            body.append("    " + ", ".join(row) + ",")
        body.append("};")
        if not one:
            body.append(f"static const uint8_t {name}_rgb[{len(faces)}][3] = {{")
            body += ["    " + ", ".join(f"{{ {r}, {g}, {b} }}" for (_, _, _, (r, g, b)) in faces[i:i + 6]) + ","
                     for i in range(0, len(faces), 6)]
            body.append("};")
        r, g, b = faces[0][3]
        body += [
            "",
            f"/* Builds the mesh; free it with e3d_mesh_free. NULL when out of memory. */",
            f"static inline e3d_mesh *{name}_create(void)",
            "{",
            f"    e3d_mesh *m = e3d_mesh_create({len(pos)}, {len(faces)});",
            "    if (!m)",
            "        return NULL;",
            f"    memcpy(m->pos, {name}_pos, sizeof({name}_pos));",
            f"    for (int i = 0; i < {len(faces)}; i++) {{",
            f"        e3d_tri *t = &m->tris[i];",
            f"        t->v[0] = {name}_tri[i][0];",
            f"        t->v[1] = {name}_tri[i][1];",
            f"        t->v[2] = {name}_tri[i][2];",
        ]
        if one:
            body += [f"        t->r = {r};", f"        t->g = {g};", f"        t->b = {b};"]
        else:
            body += [f"        t->r = {name}_rgb[i][0];", f"        t->g = {name}_rgb[i][1];",
                     f"        t->b = {name}_rgb[i][2];"]
        body += ["    }", f"    e3d_mesh_finish(m, {cf(crease)});   /* normals, bounds, winding */",
                 "    return m;", "}", ""]
    open(path, "w", encoding="ascii", newline="\n").write("\n".join(guard + body))
    print(f"wrote {path}")


def main(argv=None):
    ap = argparse.ArgumentParser(description="Convert STL/OBJ models to e3d objects.",
                                 formatter_class=argparse.RawDescriptionHelpFormatter, epilog=__doc__)
    ap.add_argument("inputs", nargs="+", help=".stl or .obj files (one e3d object each)")
    ap.add_argument("-o", "--output", help="write the e3d text format")
    ap.add_argument("--header", help="write a C header")
    ap.add_argument("--axes", help="zup (STL default), yup (OBJ default), e3d, or e.g. +y,+z,-x")
    ap.add_argument("--scale", type=float, default=1.0, help="multiply coordinates")
    ap.add_argument("--size", type=float, help="make the largest extent this many units")
    ap.add_argument("--no-center", action="store_true", help="keep the model where it is")
    ap.add_argument("--base", action="store_true", help="centre in x and z, lowest point at y = 0")
    ap.add_argument("--weld", type=float, default=1e-5, help="weld distance, a share of the size (1e-5)")
    ap.add_argument("--max-tris", type=int, help=f"decimate to at most this many (limit {MAX_TRIS})")
    ap.add_argument("--color", default="B4B4B4", help="RRGGBB for every triangle")
    ap.add_argument("--stl-colors", action="store_true", help="use binary STL facet colours")
    ap.add_argument("--flip", action="store_true", help="reverse the triangle winding")
    ap.add_argument("--crease", type=float, default=40.0,
                    help="header only: smooth normals across edges below this angle (40)")
    args = ap.parse_args(argv)
    if not args.output and not args.header:
        ap.error("give -o and/or --header")
    if not re.fullmatch(r"[0-9a-fA-F]{6}", args.color):
        ap.error("--color takes RRGGBB, such as FF8020")
    objects = [convert(p, args) for p in args.inputs]
    names = [o[0] for o in objects]
    if len(set(names)) != len(names):
        sys.exit("stl2e3d: two inputs have the same name")
    if args.output:
        write_text(args.output, objects)
    if args.header:
        write_header(args.header, objects, args.crease, args.inputs)


if __name__ == "__main__":
    main()
