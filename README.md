# ESP32-P4 3D Engine

A software 3D engine for the ESP32-P4, ported from an old PowerPC demo engine
(`ReferenceCode/3d.c`), with a demo PAPP for the
[ESP32_P4_Launcher](https://github.com/giltal/ESP32_P4_Launcher).

```
engine/   e3d.h  e3d.c  e3d_render.c     the engine (plain C, no PAPP dependency in e3d.c)
demo/     main.c  build.py  icon.png      the PAPP: the original demo show, on a 400x240 surface
tools/    make_icon.py                    draws demo/icon.png
ReferenceCode/                            the original engine, demo and object files
```

## Building and running

The build uses the PAPP SDK in a launcher checkout next to this folder
(`../ESP32_P4_Launcher`), or the folder in `PAPP_LAUNCHER`. It needs a Python
with pyelftools, such as the ESP-IDF one:

```bash
~/.espressif/python_env/idf5.5_py3.12_env/Scripts/python.exe demo/build.py
```

That writes `build/e3d_demo/e3d_demo.papp`. To put it on a board running the launcher
and run it, from the launcher checkout:

```bash
python tools/papp_dev.py --port COM10 deploy ../ESP32_P4_3D_Engine/build/e3d_demo/e3d_demo.papp
```

In Git Bash, set `MSYS_NO_PATHCONV=1` before passing `/sd/...` paths to `papp_dev.py`.
To start directly at the main scene (useful for measuring), pass `main` as the app's argument:
`deploy ... -- main`.

## Demo controls

| Button | Action |
|---|---|
| A | skip to the next part of the show |
| START | restart the show |
| Y | style: shaded, flat, wireframe, fog |
| X | HUD on/off (fps, triangles, geometry/raster ms) |
| B | exit |
| D-pad, L1/R1 | in the main scene: move and turn the camera |

## Object format

These are the original text files, and one file can hold several objects:

```
<objects>
<triangles>                 per object
<v0> <v1> <v2> <R> <G> <B>  one line per triangle
<vertices>
<x> <y> <z>                 one line per vertex
```

Axes follow the original engine:
- x is depth (the camera looks along +x), y is up, and z is right.
- Outward normals are `(v0 - v1) x (v2 - v1)`.

The loader repairs inconsistent windings, which some of the letters have. It also smooths corner normals across edges below a crease angle.

## Engine

```c
e3d_t *e = e3d_create(400, 240, 2);          /* 2: use both cores */
e3d_mesh *meshes; int n = e3d_load_text(text, &meshes);
e3d_object o; e3d_object_init(&o, &meshes[0]);
e3d_mat m; e3d_rotate(&m, 0, 30, 0); e3d_transform(&o, &m);

e3d_begin(e, pixels, stride);                /* RGB565 target */
e3d_draw(e, &o);                             /* records the object */
e3d_end(e);                                  /* geometry + rasterising */
```

- **Meshes and objects.** Meshes are shared and never change. Each object is a mesh plus a model matrix. `e3d_transform` adds a transform to an object, as the original `transformObject` did.
- **Camera.** The camera uses the original's heading, pitch and tilt, and its lens (`width / 42.44 * 35`).
- **Lighting.** One point light with ambient, diffuse and Blinn-Phong specular, using Gouraud or flat shading. Lighting is worked out in view space, where the camera is at the origin. Gouraud lighting runs in fixed point with PIE (see below).
- **Floor and shadows.** The floor is drawn as a background layer. Planar shadows land on it at `shadow_y`, and a stencil darkens each pixel only once. The light's projection onto the floor is projective, so together with the view and the object's matrix it is a single 4x4 matrix: a shadow vertex costs 4 rows and one divide. A triangle faces the light exactly when its shadow winds like a front face seen from above, so the silhouette test is the sign of the shadow's screen area.
- **Effects.** Fog, a glow at the light, wireframe, and an optional background image.
- **Clipping and depth.** Triangles are clipped at the near plane, and depth is a perspective-correct 1/x z-buffer.

### How a frame runs

1. `e3d_draw` copies the object into a list.
2. In `e3d_end`, both cores take objects from that list. Each core transforms, lights, culls, clips and projects them into its own draw lists. Each primitive is sorted into the 16-row bands it touches as soon as it is made.
3. Both cores then take bands as they come free (sky bands are cheap, floor bands are not). Each band is drawn in a colour + depth + stencil tile in internal SRAM, then copied to the frame. PSRAM is written once per pixel.
4. Lighting is computed once per unique corner normal per object.
5. Back faces are culled unless an object is `E3D_DOUBLE_SIDED`.

Most of the cost on the P4 is PSRAM traffic, not arithmetic: a cache miss costs about 1 µs while the display hardware is also using PSRAM. The design keeps per-pixel work in internal SRAM for that reason.

- **Fixed point from the projection on.** Each vertex is transformed and projected once, into a screen record: 1/x in 2.30, x and y in 12.4, and a clip code (off which screen edge, behind the near plane, outside the guard band).
  - Most triangles have all three corners on screen records. They are culled with integer tests: the AND of the clip codes for off-screen, and the sign of a 64-bit screen area for back faces. They are then written straight from the records, with no float work.
  - Only triangles that cross the near plane or reach past the guard band (1800 px from the centre, so x stays inside 12.4) take the float path, which clips them.
  - A triangle is binned only into the rows whose pixel centres it covers, and one that covers none is dropped before it is written.
  - Wireframe edges use the same records, and a closed mesh draws each shared edge once.
  - The rasteriser steps edges, depth and colours in integers. Per-triangle setup and each row's start values stay in float: keeping row state in the FPU's own registers measured faster than integer planes, which ran short of integer registers. Shadow triangles skip the colour and depth gradients.
- **Primitives in SRAM.** Each core keeps its primitives (36 bytes each, written as 9 words) in 4.6 KB chunks. The first chunks are in internal SRAM, taken one per frame while the internal heap keeps 40 KB free, up to 8 per core. Further chunks come from PSRAM. Writing a primitive to PSRAM costs about 780 cycles, because each new cache line is read before it is written. In SRAM the cost is much lower, and the rasteriser reads them back from SRAM too. The band bins (16-bit indices) are in SRAM as well. The per-object scratch stays in PSRAM, where measurements showed it costs nothing extra.
- **Engine heap.** `e3d_mem.h` sends the engine's own `malloc`s to PSRAM. On the launcher, blocks under 1 KB would otherwise go to internal SRAM. The heap can also return the P4's LP SRAM, which is uncached and slow, so SRAM allocations check the address and give such blocks back.
- **Background.** `e3d_set_background_rows` fills each band's rows straight into the SRAM tile. That is much faster than `e3d_set_background`, which copies an image from PSRAM. The demo draws its sky gradient and stars this way.
- **PIE vector unit.** When `e3d_render.c` is compiled with `-march=..._xesppie` (as `demo/build.py` does), tile clears and the copy-out use the P4's 128-bit PIE loads and stores. PIE is integer-only (it has no float SIMD or divide). A PIE vertex transform from 16-bit mesh positions was built and measured: it was about 15% slower than the scheduled float transform, because each vertex still needs a divide and a float projection, and the 16-bit pass adds memory traffic. It was therefore not kept. The float transform writes its rows out so the dependent chains overlap, which cut it from about 375 to about 150 cycles per vertex when cached.
- **Batched lighting in fixed point.** Each lit object's corner normals are lit together, 8 at a time, in 16-bit fixed point with PIE:
  - The meshes keep their normals in Q14 (`e3d_mesh.qn`).
  - The light and half vectors are made per vertex in float, since they need 1/sqrt.
  - PIE turns each normal into view space, takes n.L and n.H, raises the highlight to the shininess by squaring, and makes two colour factors per channel.
  - A corner's colour is then `base * F + S` in integers, with no branches.
  - For wireframe and double-sided objects the batch runs a second time with the normal matrix negated, which lights the back faces.

  At start-up the PIE kernel is checked against a C version of the same arithmetic, which it matches bit for bit; if it ever differed, the C version would be used. Objects with a non-uniform scale or a shininess that is not a whole number are lit per corner in float as before.
- **Fog** looks its factor up in a table indexed by the exponent and top 6 mantissa bits of the depth as a float. The table is made once per density, so there is no divide per pixel.
- **What is slow on the P4.** These measurements guided the code:
  - Taken branches cost several cycles, so a 3-instruction loop runs at about 5 cycles per iteration.
  - Dependent float operations wait about 4 cycles each, and `fdiv` about 20.
  - A load from PSRAM costs about twice one from internal SRAM, even when it hits the cache.
  - PIE stores are faster to PSRAM (through the cache) than to internal SRAM.
  - Code outside the fast section runs from PSRAM. A small helper called per primitive costs far more there than inlined, which is why `push` and `bin_prim` are inline with their rare paths split out. Library calls are the same: a float to `int64_t` conversion is a call to `__fixsfdi`, and using it 12 times per triangle cut the frame rate by more than half.
  - Copying a band to the frame costs about as much as drawing into it, because each new PSRAM line is read before it is written. Drawing colour straight into the frame only moved that cost. PSRAM has no uncached alias that apps may write, unlike internal L2MEM.

  Lighting and colour code is therefore unrolled and branch-free, and its arrays stay in PSRAM.
- **PPA.** The launcher scales the 400x240 frame to the panel with the PPA, which takes about 9 ms and blocks whoever presents. The demo keeps two frame buffers and presents through a presenter thread (`papp_disp_present_ext`), so that scaling overlaps the next frame.

## Performance (400x240, Guition 7" P4 board)

Whole-run averages, from the launcher's compositor line:

| Scene | First version | Before fixed point | Now |
|---|---|---|---|
| Intro text | 40 fps | 67 fps | 67 fps (the display's pace) |
| Main scene, shaded (~1.8-3.4k triangles + ~830 shadow) | 13-16 fps | 51.9 fps | 62.6 fps |
| Main scene, flat | | | 64.2 fps |
| Main scene, wireframe | | 20.0 fps | 37.9 fps |
| Main scene, fog | | 38.1 fps | 49.6 fps |

In the shaded main scene, geometry takes about 5.5-7 ms and rasterising about 7.5-9.5 ms, both on both cores. The cores are evenly loaded in both phases. About 0.8 ms of the geometry figure is the demo's own scene update, which runs on core 0.

The rest of the frame, as cycles over both cores:

- **Rasterising.** Floor triangles (24x24 squares, about 60 pixels each, set up again for each band they touch) take about a third. Copying bands to the frame takes about a quarter. Opaque objects take about a fifth.
- **Geometry.** Writing and binning a primitive costs about 300 cycles, down from about 470. The corner colours of a triangle cost about 220 cycles.
