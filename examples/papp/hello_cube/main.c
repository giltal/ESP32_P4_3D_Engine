/*
 * hello_cube — the smallest e3d program: a cube built in code, spinning.
 *
 * What it shows:
 *   - making a mesh with e3d_mesh_create / e3d_mesh_finish
 *   - an object (a mesh plus a model matrix) and moving it each frame
 *   - the frame: e3d_begin, e3d_draw, e3d_end into a 400x240 RGB565 buffer
 *
 * Controls: B exits.
 */
#include "papp/papp.h"
#include "e3d.h"

PAPP_MANIFEST(.id = "e3d.hello_cube", .name = "e3d Hello Cube", .version = PAPP_APP_VERSION);

#define W 400
#define H 240

/* A cube of side 2*s around the origin, one colour per face. Each face has
 * its own 4 corners so its edges stay sharp (e3d_mesh_finish would keep them
 * sharp anyway: they are at 90 degrees, above the crease angle). */
static e3d_mesh *make_cube(float s)
{
    /* the 4 corners of each face, seen from outside, anticlockwise */
    static const signed char face[6][4][3] = {
        { { -1, -1, -1 }, { -1, 1, -1 }, { -1, 1, 1 }, { -1, -1, 1 } },   /* -x: faces the camera */
        { { 1, -1, 1 }, { 1, 1, 1 }, { 1, 1, -1 }, { 1, -1, -1 } },       /* +x */
        { { -1, 1, -1 }, { 1, 1, -1 }, { 1, 1, 1 }, { -1, 1, 1 } },       /* +y: top */
        { { -1, -1, 1 }, { 1, -1, 1 }, { 1, -1, -1 }, { -1, -1, -1 } },   /* -y */
        { { -1, -1, 1 }, { -1, 1, 1 }, { 1, 1, 1 }, { 1, -1, 1 } },       /* +z: right */
        { { 1, -1, -1 }, { 1, 1, -1 }, { -1, 1, -1 }, { -1, -1, -1 } },   /* -z */
    };
    static const uint8_t rgb[6][3] = { { 230, 60, 50 }, { 60, 200, 80 }, { 70, 110, 230 },
                                       { 240, 200, 50 }, { 220, 80, 200 }, { 60, 200, 210 } };
    e3d_mesh *m = e3d_mesh_create(24, 12);
    if (!m)
        return NULL;
    for (int f = 0; f < 6; f++) {
        for (int k = 0; k < 4; k++)
            for (int a = 0; a < 3; a++)
                m->pos[f * 4 + k][a] = face[f][k][a] * s;
        /* two triangles per face; e3d_mesh_finish turns a closed mesh outward
         * whichever way the triangles were wound */
        for (int t = 0; t < 2; t++) {
            e3d_tri *tr = &m->tris[f * 2 + t];
            tr->v[0] = (uint16_t)(f * 4);
            tr->v[1] = (uint16_t)(f * 4 + 1 + t);
            tr->v[2] = (uint16_t)(f * 4 + 2 + t);
            tr->r = rgb[f][0];
            tr->g = rgb[f][1];
            tr->b = rgb[f][2];
        }
    }
    e3d_mesh_finish(m, 40.f);   /* normals, bounds, winding; 40 degree crease */
    return m;
}

int main(int argc, char **argv)
{
    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 2 };
    papp_surface_t *surf = papp_disp_surface_create(&cfg);
    e3d_t *e = e3d_create(W, H, 2);          /* 2: geometry and rasterising on both cores */
    e3d_mesh *cube = make_cube(60.f);
    if (!surf || !e || !cube) {
        papp_log("hello_cube: out of memory\n");
        return 1;
    }

    /* The camera sits at the origin looking along +x (x is depth, y up, z right) */
    *e3d_cam(e) = (e3d_camera){ .pos = { 0, 0, 0 } };
    /* A light above, left of and behind the camera */
    *e3d_light_get(e) = (e3d_light){ .pos = { -200, 400, -300 }, .r = 255, .g = 255, .b = 240 };
    e3d_params *par = e3d_params_get(e);
    par->clear_rgb565 = 0x1082;              /* dark grey background */
    par->shadows = false;                    /* nothing to cast them on */

    e3d_object obj;
    e3d_object_init(&obj, cube);

    float angle = 0;
    int64_t last = papp_time_us();
    while (!papp_exit_requested()) {
        papp_gamepad_t pad;
        if (papp_input_gamepad(0, &pad) && (pad.buttons & PAPP_BTN_B))
            break;

        int64_t now = papp_time_us();
        angle += (float)(now - last) * 60e-6f;   /* 60 degrees a second */
        last = now;

        /* The model matrix from scratch each frame: spin about the cube's
         * centre, then place it 300 units ahead of the camera. The matrix
         * calls make a matrix (they do not add to one); e3d_mul combines. */
        e3d_mat spin, place;
        e3d_rotate(&spin, angle * 0.7f, angle, angle * 0.3f);
        e3d_move(&place, 300, 0, 0);
        e3d_mul(&place, &spin);              /* place * spin: spin acts first */
        obj.m = place;

        int stride;
        uint16_t *px = papp_disp_lock(surf, &stride);
        e3d_begin(e, px, stride);
        e3d_draw(e, &obj);
        e3d_end(e);
        e3d_text(px, stride, W, H, 4, 4, "HELLO CUBE", 0xFFFF, 2);
        papp_disp_present(surf);
    }

    e3d_destroy(e);
    e3d_mesh_free(cube);
    papp_disp_surface_destroy(surf);
    return 0;
}
