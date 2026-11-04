/*
 * mesh_viewer — orbit around a model: the built-in one, or objects from a file.
 *
 *   e3d_mesh_viewer                    the built-in knot (examples/assets/knot.stl,
 *                                      converted to a C header by tools/stl2e3d.py)
 *   e3d_mesh_viewer /sd/models/x.txt   every object in an e3d text file (A steps them)
 *
 * Make a text file from an STL with:
 *   python tools/stl2e3d.py model.stl -o model.txt --size 200
 * and copy it to the SD card.
 *
 * What it shows:
 *   - a mesh from a generated header (knot_create) and from a file (e3d_load_file)
 *   - an orbit camera aimed at the model, sized from the mesh's bounding sphere
 *   - a background drawn by a row function (e3d_set_background_rows)
 *
 * Controls: D-pad orbit, L1 / R1 zoom, A next object, Y style, X auto-spin, B exit.
 */
#include "papp/papp.h"
#include "e3d.h"
#include "knot.h"           /* generated at build time */

#include <math.h>
#include <stdio.h>

PAPP_MANIFEST(.id = "e3d.mesh_viewer", .name = "e3d Mesh Viewer", .version = PAPP_APP_VERSION);

#define W 400
#define H 240
#define DEG 0.0174533f

/* A vertical gradient. It runs on both cores at once and may only write row. */
static void gradient_row(uint16_t *row, int y, int w, void *ctx)
{
    int k = y * 32 / H;                              /* 0..31 */
    uint16_t c = (uint16_t)(((4 + k / 4) << 11) | ((8 + k / 2) << 5) | (12 + k / 3));
    for (int x = 0; x < w; x++)
        row[x] = c;
}

int main(int argc, char **argv)
{
    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 2 };
    papp_surface_t *surf = papp_disp_surface_create(&cfg);
    e3d_t *e = e3d_create(W, H, 2);
    if (!surf || !e) {
        papp_log("mesh_viewer: out of memory\n");
        return 1;
    }

    /* The meshes: a file's objects, or the built-in knot */
    e3d_mesh *file_meshes = NULL, *knot = NULL;
    const e3d_mesh *meshes;
    int count = 0;
    if (argc > 1)
        count = e3d_load_file(argv[1], &file_meshes);
    if (count > 0) {
        meshes = file_meshes;
        papp_log("mesh_viewer: %s: %d objects\n", argv[1], count);
    } else {
        if (argc > 1)
            papp_log("mesh_viewer: cannot load %s, showing the built-in model\n", argv[1]);
        knot = knot_create();
        if (!knot)
            return 1;
        meshes = knot;
        count = 1;
    }

    e3d_set_background_rows(e, gradient_row, NULL);
    e3d_params *par = e3d_params_get(e);
    par->shadows = false;
    static const char *const style_name[3] = { "SHADED", "FLAT", "WIREFRAME" };
    static const uint32_t style_flags[3] = { 0, E3D_FLAT, E3D_WIRE | E3D_DOUBLE_SIDED };

    int cur = 0, style = 0;
    bool spin = true;
    float az = 30, el = 20, zoom = 1;       /* orbit angles in degrees, distance factor */
    uint32_t prev = 0;
    int64_t last = papp_time_us();
    char line[64];

    while (!papp_exit_requested()) {
        papp_gamepad_t pad;
        uint32_t held = papp_input_gamepad(0, &pad) ? pad.buttons : 0, press = held & ~prev;
        prev = held;
        if (press & PAPP_BTN_B)
            break;
        if (press & PAPP_BTN_A)
            cur = (cur + 1) % count;
        if (press & PAPP_BTN_Y)
            style = (style + 1) % 3;
        if (press & PAPP_BTN_X)
            spin = !spin;

        int64_t now = papp_time_us();
        float dt = (float)(now - last) * 1e-6f;
        last = now;
        if (held & PAPP_BTN_LEFT) az -= 90 * dt;
        if (held & PAPP_BTN_RIGHT) az += 90 * dt;
        if (held & PAPP_BTN_UP) el += 60 * dt;
        if (held & PAPP_BTN_DOWN) el -= 60 * dt;
        if (held & PAPP_BTN_L1) zoom *= 1 + dt;
        if (held & PAPP_BTN_R1) zoom /= 1 + dt;
        if (spin && !(held & (PAPP_BTN_LEFT | PAPP_BTN_RIGHT)))
            az += 25 * dt;
        el = el > 85 ? 85 : el < -85 ? -85 : el;

        const e3d_mesh *mesh = &meshes[cur];
        /* The model sits with its centre at the world origin */
        e3d_object obj;
        e3d_object_init(&obj, mesh);
        e3d_move(&obj.m, -mesh->center[0], -mesh->center[1], -mesh->center[2]);
        obj.flags = style_flags[style];

        /* The camera looks along (cos h, 0, sin h) turned down by the pitch; put
         * it back from the origin along that line, far enough that the bounding
         * sphere fills about 80% of the height */
        float dist = mesh->radius * 3.6f * zoom + 1.f;
        e3d_camera *cam = e3d_cam(e);
        cam->heading = az;
        cam->pitch = -el;
        cam->tilt = 0;
        cam->pos[0] = -dist * cosf(el * DEG) * cosf(az * DEG);
        cam->pos[1] = dist * sinf(el * DEG);
        cam->pos[2] = -dist * cosf(el * DEG) * sinf(az * DEG);
        /* the light follows the camera, up and to the left of it */
        e3d_light *l = e3d_light_get(e);
        l->pos[0] = cam->pos[0] * 1.5f - cam->pos[2] * 0.6f;
        l->pos[1] = cam->pos[1] + dist;
        l->pos[2] = cam->pos[2] * 1.5f + cam->pos[0] * 0.6f;

        int stride;
        uint16_t *px = papp_disp_lock(surf, &stride);
        e3d_begin(e, px, stride);
        e3d_draw(e, &obj);
        e3d_end(e);

        const e3d_stats *st = e3d_get_stats(e);
        snprintf(line, sizeof(line), "OBJECT %d/%d  %d TRIS  %s", cur + 1, count, mesh->ntris, style_name[style]);
        e3d_text(px, stride, W, H, 4, 4, line, 0xFFFF, 1);
        snprintf(line, sizeof(line), "%.1f MS", (st->t_geom_us + st->t_raster_us) / 1000.0);
        e3d_text(px, stride, W, H, 4, 14, line, 0x07FF, 1);
        papp_disp_present(surf);
    }

    e3d_destroy(e);
    if (file_meshes)
        e3d_meshes_free(file_meshes, count);
    if (knot)
        e3d_mesh_free(knot);
    papp_disp_surface_destroy(surf);
    return 0;
}
