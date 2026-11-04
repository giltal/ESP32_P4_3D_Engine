# e3d user guide

e3d is a software 3D engine for the ESP32-P4. It draws lit, depth-tested triangles into
an RGB565 frame buffer using both CPU cores and the P4's PIE vector unit. There is no GPU
involved: on a 400x240 frame it runs scenes of a few thousand triangles, with shadows,
at about 60 fps.

This guide covers:

1. [What the engine does](#1-what-the-engine-does)
2. [Getting it into your project](#2-getting-it-into-your-project) (ESP-IDF, PAPP)
3. [A first program](#3-a-first-program)
4. [Coordinates and conventions](#4-coordinates-and-conventions)
5. [Meshes](#5-meshes)
6. [Objects and transforms](#6-objects-and-transforms)
7. [The camera](#7-the-camera)
8. [Light and materials](#8-light-and-materials)
9. [Floor and shadows](#9-floor-and-shadows)
10. [Backgrounds](#10-backgrounds)
11. [Styles and effects](#11-styles-and-effects)
12. [The frame loop and getting pixels on screen](#12-the-frame-loop-and-getting-pixels-on-screen)
13. [Models from STL and OBJ files](#13-models-from-stl-and-obj-files)
14. [Performance](#14-performance)
15. [Memory, threads and porting](#15-memory-threads-and-porting)
16. [Troubleshooting](#16-troubleshooting)
17. [API reference](#17-api-reference)

---

## 1. What the engine does

- **Triangles:** flat or Gouraud shaded, sub-pixel accurate, with a perspective-correct
  depth buffer (1/depth). Triangles crossing the near plane are clipped.
- **Lighting:** one point light. Each surface gets ambient light plus diffuse and
  Blinn-Phong specular light. Normals are smooth across edges below a crease angle and
  sharp across steeper ones.
- **Floor:** a floor layer is drawn as the background. Planar shadows from every other
  object fall on it.
- **Effects:** fog, a glow at the light, wireframe, unlit objects, a background image or
  a background row function, and 5x7 overlay text.
- **Meshes and objects:** meshes are shared and never change after loading. Any number of
  objects can use one mesh, each with its own model matrix, flags and colour.
- **Speed:**
  - Geometry and rasterising run on both cores.
  - The frame is drawn in 16-row bands, in internal SRAM.
  - Lighting is batched in PIE fixed point.
  - Most of the pipeline is integer.

  Section 14 has the numbers.

Limits, so you can plan around them:

| | |
|---|---|
| Vertices per mesh | 65535 |
| Triangles per mesh | 21845 |
| Lights | 1 point light |
| Pixel format | RGB565 only |
| Texture mapping | none (colours are per triangle, or per object) |
| Transparency | none |
| Shadows | onto the floor plane only, not onto other objects |

---

## 2. Getting it into your project

The engine lives in the `e3d/` folder. It is plain C with a small platform layer
(`e3d/src/e3d_port.h`) that picks one of three backends:

| Built with | Backend | Used for |
|---|---|---|
| ESP-IDF (`ESP_PLATFORM` is defined) | FreeRTOS tasks, `heap_caps`, `esp_timer` | your own firmware |
| the PAPP SDK (no `ESP_PLATFORM`) | the launcher's PAPP API | apps for the [ESP32_P4_Launcher](https://github.com/giltal/ESP32_P4_Launcher) |
| `-DE3D_HOST` | the C library, one thread | desktop tests |

### 2.1 As an ESP-IDF component

`e3d/` is an ESP-IDF component (`CMakeLists.txt`, `idf_component.yml`). There are two
ways to use it.

**From a checkout.** Point your project's top-level `CMakeLists.txt` at the folder,
before `project()`:

```cmake
set(EXTRA_COMPONENT_DIRS "path/to/ESP32_P4_3D_Engine/e3d")
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(my_app)
```

**From git, with the component manager.** Add this to `main/idf_component.yml`:

```yaml
dependencies:
  e3d:
    git: https://github.com/giltal/ESP32_P4_3D_Engine.git
    path: e3d
```

Then add `e3d` to your component's `REQUIRES` (or `PRIV_REQUIRES`) and write
`#include "e3d.h"`.

The component compiles itself with `-O2`, whatever the project's optimisation level. It
also compiles the renderer with the P4's PIE extension (`-march=..._xesppie`).

These sdkconfig settings matter:

| Setting | Why |
|---|---|
| `CONFIG_SPIRAM=y` (and your board's PSRAM mode and speed) | Meshes, the draw lists and your frame buffers go to PSRAM. |
| `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_360=y` | Full speed. |
| `CONFIG_FREERTOS_NUMBER_OF_CORES=2` (the default) | The second core takes half of the work. |
| `CONFIG_ESP_MAIN_TASK_STACK_SIZE=16384`, or a task of your own with 16 KB or more | The task that draws needs a deep stack. The demo's main thread has 24 KB; the engine's own worker has 8 KB. |
| `CONFIG_COMPILER_OPTIMIZATION_PERF=y` | Optional: speeds up your own code. The engine is `-O2` already. |

The engine's worker task runs on core 1, at the priority of the task that calls
`e3d_create`. Its functions marked fast are placed in internal RAM (`IRAM_ATTR`), about
16 KB.

`examples/idf/guition_p4_7` is a complete firmware for the Guition JC1060P470C (7"
1024x600). It brings up the panel and scales the frame with the PPA (section 12).

### 2.2 As part of a PAPP

The PAPP examples build with `examples/papp/papp_common.py`. It compiles
`e3d/src/e3d.c` and `e3d/src/e3d_render.c` into the app, with `-O2` and PIE for the
renderer. A new app's `build.py` is three lines:

```python
sys.path.insert(0, "path/to/ESP32_P4_3D_Engine/examples/papp")
import papp_common
papp_common.build("my_app", HERE, ["main.c"])
```

The build needs:

- The PAPP SDK from a launcher checkout. It looks in `../ESP32_P4_Launcher` next to this
  repository, or in the folder in `PAPP_LAUNCHER`.
- A Python with pyelftools, such as the ESP-IDF one.

```bash
~/.espressif/python_env/idf5.5_py3.12_env/Scripts/python.exe examples/papp/hello_cube/build.py
```

Deploy from the launcher checkout. In Git Bash, set `MSYS_NO_PATHCONV=1` first.

```bash
python tools/papp_dev.py --port COM10 deploy ../ESP32_P4_3D_Engine/build/e3d_hello_cube/e3d_hello_cube.papp
```

### 2.3 The examples

| Example | Kind | Shows |
|---|---|---|
| `examples/papp/hello_cube` | PAPP | the smallest program: a mesh built in code, a matrix, a frame |
| `examples/papp/shadows` | PAPP | floor, shadows, a moving light and its glow, the four styles, statistics |
| `examples/papp/mesh_viewer` | PAPP | an STL model converted at build time, models from files, an orbit camera, a background function |
| `examples/papp/demo` | PAPP | the full show from the original engine (letters, logos, bouncing balls) |
| `examples/idf/guition_p4_7` | ESP-IDF firmware | the engine with no launcher: the panel driver, PPA scaling, double buffering |

---

## 3. A first program

The program has four parts:

- A renderer.
- A mesh, and an object that places it.
- A camera and a light.
- A loop that draws into a buffer and shows it.

```c
#include "e3d.h"

e3d_t *e = e3d_create(400, 240, 2);            /* 400x240, both cores */
e3d_mesh *ball = e3d_mesh_sphere(60, 24, 16, 230, 80, 60);

e3d_object obj;
e3d_object_init(&obj, ball);
e3d_move(&obj.m, 300, 0, 0);                   /* 300 units in front of the camera */

*e3d_cam(e) = (e3d_camera){ .pos = { 0, 0, 0 } };              /* looks along +x */
*e3d_light_get(e) = (e3d_light){ .pos = { 0, 400, -300 }, .r = 255, .g = 255, .b = 255 };
e3d_params_get(e)->clear_rgb565 = 0x0000;

for (;;) {
    uint16_t *pixels = next_buffer();          /* yours: 400x240 RGB565 */
    e3d_begin(e, pixels, 400);                 /* stride in pixels */
    e3d_draw(e, &obj);
    e3d_end(e);                                /* everything is drawn here */
    show(pixels);                              /* yours */
}
```

`examples/papp/hello_cube/main.c` is this program as a complete PAPP.

---

## 4. Coordinates and conventions

e3d keeps the axes of the engine it was ported from:

```
        y  (up)
        |   x  (depth: the camera looks along +x)
        |  /
        | /
        |/_____ z  (right on screen)
```

- **Units** are whatever you like. The default lens shows a 1-unit object at a distance
  of 1 as about 330 pixels wide on a 400-pixel frame. So an object 100 units across,
  300 units away, is about 110 pixels.
- **Triangles** face outward when `(v0 - v1) x (v2 - v1)` points out of the object. You
  rarely need to care:
  - Closed meshes are turned outward automatically.
  - Open meshes follow the way most of their triangles are wound.
  - `E3D_DOUBLE_SIDED` draws both sides.
- **Colours** are 0..255 per channel in meshes (`r`, `g`, `b`) and `0xRRGGBB` in the
  object, floor and light calls. Frame values are RGB565 (`clear_rgb565`, `fog_rgb565`,
  text colours).
- **Angles** are in degrees everywhere.

---

## 5. Meshes

A mesh holds:

- `pos[]`: the vertex positions.
- `tris[]`: three vertex indices and a colour per triangle.
- What `e3d_mesh_finish` computes from those: corner normals, a bounding sphere
  (`center`, `radius`) and whether the mesh is `open`.

Never change a mesh after it is finished; make another one instead.

### 5.1 Building one in code

```c
e3d_mesh *m = e3d_mesh_create(nverts, ntris);
for (...) { m->pos[i][0] = x; m->pos[i][1] = y; m->pos[i][2] = z; }
for (...) { m->tris[t].v[0] = a; m->tris[t].v[1] = b; m->tris[t].v[2] = c;
            m->tris[t].r = 255; m->tris[t].g = 128; m->tris[t].b = 0; }
e3d_mesh_finish(m, 40.f);      /* crease angle in degrees */
...
e3d_mesh_free(m);
```

**The crease angle** decides where shading is smooth:

- Edges whose faces meet at less than this angle are shaded smoothly across.
- Steeper edges stay sharp.
- 30 to 45 suits most models.
- 0 makes every triangle flat-looking while keeping Gouraud speed; use `E3D_FLAT` for
  real flat shading.
- 180 smooths everything.

**Vertices can be shared or not.**

- A cube with 8 shared corners still gets sharp edges, because the crease splits its
  normals.
- Sharing vertices does help the closed-mesh test, which needs every edge used by exactly
  two triangles.

### 5.2 Ready-made meshes

```c
e3d_mesh *e3d_mesh_sphere(float radius, int slices, int stacks, int r, int g, int b);
e3d_mesh *e3d_mesh_floor(float x0, float z0, float x1, float z1, float y, int n,
                         uint32_t rgb_a, uint32_t rgb_b);   /* n x n chequered tiles */
```

A sphere with few slices makes a faceted shape:

- `(r, 4, 2, ...)` is an octahedron.
- `(r, 6, 3, ...)` is a gem.

Draw these with `E3D_FLAT`.

### 5.3 The text format

This is the original engine's object format. One file can hold several objects.

```
<objects>
<triangles>                     -- per object
<v0> <v1> <v2> <R> <G> <B>      -- one line per triangle, colour 0..255
<vertices>
<x> <y> <z>                     -- one line per vertex
```

```c
e3d_mesh *meshes;
int n = e3d_load_file("/sd/models/scene.txt", &meshes);   /* or e3d_load_text(string, &meshes) */
...                                                        /* meshes[0] .. meshes[n-1] */
e3d_meshes_free(meshes, n);
```

- Both functions return 0 on a format error.
- The loader smooths with a 45 degree crease, and repairs windings as `e3d_mesh_finish`
  does.
- `tools/stl2e3d.py` writes this format from STL and OBJ files (section 13).

---

## 6. Objects and transforms

```c
typedef struct {
    const e3d_mesh *mesh;
    e3d_mat  m;          /* model -> world */
    uint32_t flags;      /* E3D_* */
    int32_t  color;      /* 0xRRGGBB for the whole object, -1 = the mesh's colours */
} e3d_object;
```

`e3d_object_init` sets the identity matrix, no flags and the mesh's colours.
`e3d_draw` copies the object, so it can live on the stack. The mesh must stay alive
until `e3d_end` returns.

### 6.1 Matrices

`e3d_mat` is a 4x4 float matrix that acts as `v' = M v`. Column 3 is the translation.
Each of these calls **makes** a matrix, replacing what was in it:

```c
e3d_identity(&m);
e3d_move(&m, dx, dy, dz);
e3d_scale(&m, sx, sy, sz);                     /* 0 means 1 */
e3d_rotate(&m, ax, ay, az);                    /* degrees: X first, then Y, then Z */
e3d_rotate_about(&m, point, ax, ay, az, deg);  /* deg around the axis (ax, ay, az) through point */
e3d_scale_about(&m, point, sx, sy, sz);
```

Combine them with `e3d_mul(&a, &b)`, which gives `a = a * b`: **b acts first**. The
usual model matrix is *scale, then rotate, then move*:

```c
e3d_mat move, rot, scale;
e3d_move(&move, x, y, z);
e3d_rotate(&rot, 0, heading, 0);
e3d_scale(&scale, 2, 2, 2);
e3d_mul(&move, &rot);        /* move * rot */
e3d_mul(&move, &scale);      /* move * rot * scale: scale acts first */
obj.m = move;
```

`e3d_transform(&obj, &t)` puts `t` on top of what the object already has
(`obj.m = t * obj.m`), the way the original engine's `transformObject` did. It suits
motions that build up, such as "move a bit more this frame".

Accumulated float matrices drift slowly over thousands of frames. For long-running
motion, build the matrix from the time or the frame number each frame, as the examples do.

`e3d_apply(&m, v)` transforms a point. `e3d_object_pos(&obj, out)` gives the object's
centre in world space.

### 6.2 Flags

| Flag | Effect |
|---|---|
| `E3D_FLOOR` | the floor layer: drawn first as background, no depth test, receives shadows (section 9) |
| `E3D_FLAT` | one colour per triangle |
| `E3D_WIRE` | edges only; shared edges of a closed mesh are drawn once |
| `E3D_DOUBLE_SIDED` | no back-face culling; back faces are lit from their side |
| `E3D_NO_SHADOW` | casts no shadow |
| `E3D_UNLIT` | the triangle colours as they are, with no lighting |
| `E3D_HIDDEN` | skipped by `e3d_draw` |

`e3d_object_color(&obj, r, g, b)` paints the whole object one colour. Setting
`obj.color = -1` goes back to the mesh's colours.

---

## 7. The camera

```c
typedef struct {
    float pos[3];
    float heading;     /* around y, from +x towards +z (turn right) */
    float pitch;       /* up from the xz plane; negative looks down */
    float tilt;        /* roll around the view direction */
} e3d_camera;

e3d_camera *cam = e3d_cam(e);    /* change it before e3d_begin */
```

With `heading = 0` and `pitch = 0` the camera looks along +x.

`e3d_camera_move(e, dx, dy, dz, dheading, dpitch, dtilt)` moves the camera relative to
itself, as the original `transformCamera` did.

**Aiming at a point.** The view direction is `(cos p cos h, sin p, cos p sin h)`. To look
at a target from position `c`:

```c
float d[3] = { t[0] - c[0], t[1] - c[1], t[2] - c[2] };
cam->heading = atan2f(d[2], d[0]) * 57.29578f;
cam->pitch   = atan2f(d[1], sqrtf(d[0] * d[0] + d[2] * d[2])) * 57.29578f;
```

**Orbiting a target** at distance `r`, azimuth `az` and elevation `el` (from
`mesh_viewer`):

```c
cam->heading = az;
cam->pitch   = -el;
cam->pos[0]  = t[0] - r * cosf(el) * cosf(az);
cam->pos[1]  = t[1] + r * sinf(el);
cam->pos[2]  = t[2] - r * cosf(el) * sinf(az);
```

To frame an object, put the camera at about `3.6 * mesh->radius` for a 240-pixel frame
height.

**The lens.** `e3d_set_focal(e, f)` sets the pixels per unit at depth 1. The default is
`width / 42.44 * 35`, about 330 for a 400-pixel frame, which is like a 35 mm lens. A
larger `f` zooms in.

---

## 8. Light and materials

```c
e3d_light *l = e3d_light_get(e);
*l = (e3d_light){ .pos = { 0, 800, -300 }, .r = 255, .g = 240, .b = 220 };   /* colour 0..255 */

e3d_params *p = e3d_params_get(e);
```

| `e3d_params` field | Default | Meaning |
|---|---|---|
| `ambient` | 0.35 | the share of the surface colour lit by nothing |
| `diffuse` | 0.95 | the strength of the diffuse light |
| `specular` | 0.85 | the strength of the highlight (white, scaled by the light colour) |
| `shininess` | 24 | the highlight's exponent; **whole numbers are fastest** (see below) |
| `shadows` | true | planar shadows on the floor |
| `shadow_y` | 1 | the height of the plane shadows fall on (just above a floor at 0) |
| `shadow_dark` | 0.45 | how much floor colour a shadow keeps: 0 is black, 1 is no shadow |
| `fog_density` | 0 | 0 is off; fog = exp(-depth * density) |
| `fog_rgb565` | grey | the colour of the fog |
| `clear_rgb565` | black | the background when there is no image and no row function |

Lighting is computed in view space, once per unique corner normal per object, in 16-bit
fixed point with PIE. Two kinds of object fall back to a slower float path:

- Objects with a non-uniform scale.
- Settings where `shininess` is not a whole number.

Keep scales uniform and `shininess` whole when speed matters.

---

## 9. Floor and shadows

The floor is an object with `E3D_FLOOR`:

```c
e3d_mesh *fl = e3d_mesh_floor(0, -400, 900, 400, 0, 12, 0xD8D0B8, 0x506070);
e3d_object floor_o;
e3d_object_init(&floor_o, fl);
floor_o.flags = E3D_FLOOR;
...
e3d_begin(e, px, stride);
e3d_draw(e, &floor_o);        /* first */
e3d_draw(e, &ball);           /* objects drawn after the floor cast shadows */
e3d_end(e);
```

- The floor is drawn first, as a background layer, with no depth test. It has to be
  flat, and lie below everything else in the scene.
- **Draw the floor before the objects that should cast shadows.** Only objects drawn
  after it in the frame cast shadows.
- An object casts a shadow only when all of these hold:
  - `shadows` is on.
  - The object has no `E3D_NO_SHADOW`.
  - The light is above `shadow_y`.
- Shadows are projected from the point light onto the plane `y = shadow_y`. They darken
  floor pixels only, and each pixel only once, so overlapping shadows do not get darker.
  They do not fall on other objects.
- Each shadow triangle costs about as much as a cheap triangle. `e3d_stats.shadow_tris`
  counts them.

---

## 10. Backgrounds

Everything an object does not cover shows the background. There are three choices,
from cheapest to dearest:

1. **A clear colour**, `params->clear_rgb565`. This is the default.
2. **A row function.** The engine calls it for each background row of each band, into
   internal SRAM:

   ```c
   static void sky(uint16_t *row, int y, int w, void *ctx)
   {
       uint16_t c = gradient[y];
       for (int x = 0; x < w; x++) row[x] = c;
   }
   e3d_set_background_rows(e, sky, NULL);
   ```

   It is called **from both cores at once**, so it may only write `row`. Use read-only
   tables made beforehand. The demo's starry sky and the viewer's gradient work this way.
3. **An image**, `e3d_set_background(e, img)`: a `width x height` RGB565 array copied
   under each frame. Each band then copies its rows from PSRAM, which costs much more
   than filling them. Prefer a row function when the background can be computed.

Pass `NULL` to either call to go back to the clear colour.

---

## 11. Styles and effects

- **Shaded** (the default): Gouraud, smooth across edges below the crease angle.
- **Flat** (`E3D_FLAT`): one colour per triangle. About as fast as shaded; looks faceted.
- **Wireframe** (`E3D_WIRE`): lines with depth test.
  - Wire objects light both sides, so add `E3D_DOUBLE_SIDED` to see the back edges.
  - Lines cost more per pixel than triangles. A dense wire mesh is slower than the
    same mesh shaded.
- **Fog** (`params->fog_density`, `fog_rgb565`): blends towards the fog colour by depth.
  - Try 0.0005 for a large scene, or 0.002 for a small one.
  - It costs about 20% of the frame.
- **Light glow:** `e3d_draw_light(e)` between `e3d_begin` and `e3d_end` draws a soft
  glow at the light's position.
- **Overlay text:** `e3d_text(px, stride, w, h, x, y, "TEXT", rgb565, scale)` after
  `e3d_end`.
  - It draws 5x7 characters: A-Z, 0-9, and `. : - / %` and space. Lower case is drawn
    as upper case.
  - It writes straight into the frame.

---

## 12. The frame loop and getting pixels on screen

A frame looks like this:

```c
/* change the camera, light, params and objects */
e3d_begin(e, pixels, stride);    /* the target for this frame */
e3d_draw(e, &a);                 /* records; nothing is drawn yet */
e3d_draw(e, &b);
e3d_draw_light(e);               /* optional */
e3d_end(e);                      /* both cores: geometry, then rasterising */
/* overlays (e3d_text), then show the buffer */
```

- `e3d_end` writes every pixel of the frame once, so there is no need to clear it.
- While `e3d_end` runs, the engine owns both cores, and the target must not be on screen.
- **Use two buffers:** draw one while the other is shown or scaled.

**In a PAPP**, the launcher shows the frame. There are two options:

- **The simple way:** create a surface with `.buffers = 2`. Then each frame call
  `papp_disp_lock(surf, &stride)`, draw, and call `papp_disp_present(surf)`.
  `hello_cube`, `shadows` and `mesh_viewer` do this.
- **The faster way:** allocate two buffers of your own (`PAPP_MEM_LARGE | PAPP_MEM_DMA`,
  64-byte aligned). Give each finished one to a presenter thread that calls
  `papp_disp_present_ext(surf, buf)`. The present scales with the PPA (about 8 ms on
  the 7" board), so the next frame is drawn meanwhile. The demo does this.

**In ESP-IDF**, you drive the panel yourself. `examples/idf/guition_p4_7/main/display.c`
does it for the Guition 7" board:

- It brings up the JD9165 MIPI-DSI panel with two DPI frame buffers.
- A 400x240 frame is scaled to 1000x600 with the PPA (`ppa_do_scale_rotate_mirror`) into
  the hidden panel buffer.
- It then flips at the next refresh.
- The scaling runs on a task, so `display_present(frame)` returns at once:

```c
display_init(400, 240);
uint16_t *fb[2] = { display_alloc_frame(), display_alloc_frame() };
for (int cur = 0;; cur ^= 1) {
    e3d_begin(e, fb[cur], 400);
    ...
    e3d_end(e);
    display_present(fb[cur]);    /* waits for the previous one to be scaled */
}
```

For another board, keep the PPA and task part, and replace the panel bring-up with your
board's. The frame buffers must be 64-byte aligned (the PPA's DMA and the cache line);
the PPA driver writes the cache back itself.

**Frame size.** Rasterising cost scales with the pixel count. 400x240, scaled 2.5x, suits
a 1024x600 panel. 320x200 is about 1.5x faster, and 640x400 about 2.5x slower.

---

## 13. Models from STL and OBJ files

`tools/stl2e3d.py` converts binary STL, ASCII STL and Wavefront OBJ files. It needs only
Python 3.

```bash
# the text format, for e3d_load_file (copy it to the SD card)
python tools/stl2e3d.py model.stl -o model.txt --size 200

# a C header with a model_create() function, for building into the firmware
python tools/stl2e3d.py model.stl --header model.h --size 200 --color FF8020

# several models, one object each, in one file; at most 3000 triangles each
python tools/stl2e3d.py a.stl b.obj -o scene.txt --max-tris 3000
```

Using the header:

```c
#include "model.h"
e3d_mesh *m = model_create();     /* e3d_mesh_create + e3d_mesh_finish */
...
e3d_mesh_free(m);
```

What it does to each model:

1. **Axes.** CAD and STL files are usually z-up, and OBJ files y-up.
   - `--axes zup` is the default for STL and `--axes yup` for OBJ. Both map the file
     to e3d's y-up, x-depth axes.
   - `--axes e3d` keeps the file's axes.
   - `--axes=+y,+z,-x` gives the source axis for each of e3d's x, y and z.
   - A mirroring mapping reverses the windings automatically.
2. **Welding.** STL stores every triangle's corners separately. Corners closer than
   `--weld` become one vertex. The default is 1e-5 of the model's size. Welding is what
   makes smooth normals and the closed-mesh check work.
3. **Size.**
   - `--scale k` multiplies the coordinates.
   - `--size n` makes the largest extent n units.
4. **Placement.**
   - By default the middle of the bounds goes to the origin.
   - `--base` also puts the lowest point at y = 0, for models that stand on a floor.
   - `--no-center` leaves the model where it is.
5. **Decimation.** `--max-tris n` clusters vertices on the finest grid that still gives
   at most n triangles.
   - Models over the engine's limit (21845 triangles, 65535 vertices) are always reduced
     to fit.
   - Clustering can leave a few open or shared edges. The engine handles those; a mesh
     reported as open just uses majority winding.
6. **Winding.** Corners are reordered into e3d's convention. `--flip` reverses them, for
   files wound the wrong way.
7. **Colour.**
   - `--color RRGGBB` colours every triangle; the default is light grey.
   - `--stl-colors` uses the per-facet colours that some binary STL writers store
     (VisCAM/SolidView style).
   - `--crease` sets the header's crease angle (default 40). The text loader always
     uses 45.

The tool prints each model's triangle and vertex counts, its bounds, and whether it is
closed.

`examples/papp/mesh_viewer/build.py` runs the tool at build time on
`examples/assets/knot.stl`, so the header is never stale. The viewer also opens any text
file given as its argument.

**Triangle budget.** For 60 fps at 400x240, keep a scene to about 3-4k triangles on
screen, shadows included. A single model of up to about 8k triangles still runs
smoothly. Decimate heavier scans.

---

## 14. Performance

Measured at 400x240 on the Guition 7" board (360 MHz), in a PAPP:

| Scene | fps |
|---|---|
| hello_cube (12 triangles) | 67 (the display's pace) |
| shadows (545 triangles + 325 shadow) | 59 |
| mesh_viewer, knot (3840 triangles) | 67 (about 12 ms of engine time a frame) |
| ESP-IDF example, 1.3k triangles + 800 shadow | 67 (about 11.5 ms of engine time a frame) |
| demo main scene (1.8-3.4k triangles + 830 shadow), shaded | 62.6 |
| the same, flat / fog / wireframe | 64.2 / 49.6 / 37.9 |

How the time splits:

- **Geometry**, which runs on both cores: transforming, lighting, culling, clipping and
  binning. It costs about 300 cycles per triangle written, plus about 220 for the corner
  colours.
- **Rasterising**, which runs on both cores, band by band: filling the triangles, and
  copying each finished band from SRAM to the PSRAM frame. The copy costs about as much
  as drawing a band.

The floor is drawn like any other triangles. A floor of n x n tiles is 2n² triangles, and
each tile is set up again for every band it touches. Keep n modest: 12 to 24.

Tips:

- **Fewer, bigger objects.** Each object has a fixed cost: the matrix, the light set-up
  and a scratch check. Merge static decorations into one mesh.
- **Share meshes.** Ten balls from one sphere mesh is one mesh in memory and ten objects.
- **Keep scales uniform and `shininess` whole**, so lighting stays on the fixed-point
  PIE path.
- **Hide what is off screen.** Off-screen triangles are cheap but not free; `E3D_HIDDEN`
  or not drawing at all is cheaper.
- **Avoid big background images.** Use a clear colour or a row function.
- **Wireframe costs more** than shaded for dense meshes. Fog costs about 20%.
- **Two threads.** `e3d_create(w, h, 2)` puts both cores to work, at nearly 2x the speed
  of one. Leave other heavy tasks off core 1 while `e3d_end` runs.
- **Present asynchronously** (section 12). A blocking 8 ms PPA scale caps you at about
  50 fps.
- `e3d_get_stats(e)` after `e3d_end` gives the objects, triangles in, triangles drawn,
  shadow triangles, and the geometry and raster times.

Section "Engine" of the README explains the design decisions behind these numbers: what
is slow on the P4, and why the work happens in SRAM.

---

## 15. Memory, threads and porting

**Memory.**

- **The renderer** (`e3d_create`) takes one band tile per thread in internal SRAM:
  colour, depth and stencil for 16 rows, about 45 KB at 400 wide. Its first primitive chunks go in SRAM too: one more each frame, up to 8 per
  core, while at least 40 KB of the internal heap stays free. The draw lists then grow
  in PSRAM as a scene needs.
- **Meshes and everything else** the engine allocates go to PSRAM. On a system without
  PSRAM, they go to the normal heap.
- **A mesh** costs about 12 bytes per vertex plus 16 per triangle, plus its normals.
- **The P4's LP SRAM** is uncached and slow. The engine never keeps a block there.

**Threads.**

- Call the e3d functions for one renderer from one task. They are not locked.
- `e3d_end` hands half of each phase to the worker on core 1 and waits for it.
- The row function is the one callback, and it runs on both cores.

**Porting.** Everything platform-specific is in `e3d/src/e3d_port.h`:

- A microsecond clock.
- Aligned SRAM and any-memory allocators.
- The free internal heap.
- The engine's own heap.
- A binary semaphore, and a thread with join.
- A log macro.
- `E3D_FAST`, the attribute that puts code in internal RAM.

To run elsewhere, add a backend there. A non-P4 target also needs `e3d_render.c` built
without the PIE `-march`; its C paths are used then.

---

## 16. Troubleshooting

| Symptom | Likely cause |
|---|---|
| Nothing shows | The camera faces away (it looks along +x), or the object is behind it, or `e3d_draw` is outside `e3d_begin` / `e3d_end`. |
| An object looks inside out or has holes | An open mesh with mixed windings: use `E3D_DOUBLE_SIDED`, or fix the model with `stl2e3d.py --flip`. |
| A converted STL is shaded in blocks | Its corners were not welded: raise `--weld` (try 1e-4). |
| No shadows | The floor was drawn after the objects, `shadows` is off, or the light is below `shadow_y`. |
| Shadows float above or sink into the floor | `shadow_y` does not match the floor's height: use the floor's y plus 1. |
| Lighting is much slower than expected | A non-uniform scale or a fractional `shininess`, which take the float path. |
| Tearing or half-old frames | A buffer was drawn into while it was being shown or scaled: use two buffers. |
| `e3d_create` returns NULL | No internal SRAM for the band tiles, or no memory at all. |
| `e3d_mesh_create` returns NULL | Over 65535 vertices or 21845 triangles: decimate. |
| The launcher reports a few leaked allocations at exit | newlib's `printf("%f")` keeps a few blocks. They are reclaimed with the app; the engine itself frees everything. |

---

## 17. API reference

The complete, commented API is in `e3d/include/e3d.h`. In brief:

```c
/* matrices */
void e3d_identity(e3d_mat *m);
void e3d_move(e3d_mat *m, float dx, float dy, float dz);
void e3d_scale(e3d_mat *m, float sx, float sy, float sz);
void e3d_rotate(e3d_mat *m, float ax, float ay, float az);
void e3d_mul(e3d_mat *a, const e3d_mat *b);                       /* a = a * b */
void e3d_apply(const e3d_mat *m, float v[3]);
void e3d_rotate_about(e3d_mat *m, const float b[3], float ax, float ay, float az, float deg);
void e3d_scale_about(e3d_mat *m, const float b[3], float sx, float sy, float sz);

/* meshes */
int       e3d_load_text(const char *text, e3d_mesh **out);
int       e3d_load_file(const char *path, e3d_mesh **out);
void      e3d_meshes_free(e3d_mesh *m, int count);
e3d_mesh *e3d_mesh_create(int nverts, int ntris);
void      e3d_mesh_finish(e3d_mesh *m, float crease_deg);
void      e3d_mesh_free(e3d_mesh *m);
e3d_mesh *e3d_mesh_sphere(float radius, int slices, int stacks, int r, int g, int b);
e3d_mesh *e3d_mesh_floor(float x0, float z0, float x1, float z1, float y, int n, uint32_t a, uint32_t b);

/* objects */
void e3d_object_init(e3d_object *o, const e3d_mesh *mesh);
void e3d_transform(e3d_object *o, const e3d_mat *mat);             /* o->m = mat * o->m */
void e3d_object_pos(const e3d_object *o, float out[3]);
void e3d_object_color(e3d_object *o, int r, int g, int b);

/* renderer */
e3d_t      *e3d_create(int width, int height, int threads);
void        e3d_destroy(e3d_t *e);
e3d_camera *e3d_cam(e3d_t *e);
e3d_light  *e3d_light_get(e3d_t *e);
e3d_params *e3d_params_get(e3d_t *e);
void        e3d_camera_move(e3d_t *e, float dx, float dy, float dz, float dh, float dp, float dt);
void        e3d_set_focal(e3d_t *e, float f);
void        e3d_set_background(e3d_t *e, const uint16_t *img);
void        e3d_set_background_rows(e3d_t *e, e3d_row_fn fn, void *ctx);

/* a frame */
void e3d_begin(e3d_t *e, uint16_t *pixels, int stride);
void e3d_draw(e3d_t *e, const e3d_object *o);
void e3d_draw_light(e3d_t *e);
void e3d_end(e3d_t *e);
const e3d_stats *e3d_get_stats(e3d_t *e);

/* overlay text */
void e3d_text(uint16_t *pixels, int stride, int w, int h, int x, int y, const char *s,
              uint16_t color, int scale);
```
