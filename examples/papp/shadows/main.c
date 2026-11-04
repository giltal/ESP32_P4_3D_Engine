/*
 * shadows — a floor, a bouncing ball and a spinning gem under a moving light.
 *
 * What it shows:
 *   - a floor object (E3D_FLOOR): drawn as the background, receives shadows
 *   - planar shadows (e3d_params.shadows, shadow_y, shadow_dark)
 *   - moving the point light, and the glow drawn at it (e3d_draw_light)
 *   - the render styles: shaded, flat (E3D_FLAT), wireframe (E3D_WIRE), fog
 *   - per-frame statistics (e3d_get_stats)
 *
 * Controls: Y next style, X shadows on/off, B exit.
 */
#include "papp/papp.h"
#include "e3d.h"

#include <math.h>
#include <stdio.h>

PAPP_MANIFEST(.id = "e3d.shadows", .name = "e3d Shadows", .version = PAPP_APP_VERSION);

#define W 400
#define H 240

int main(int argc, char **argv)
{
    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 2 };
    papp_surface_t *surf = papp_disp_surface_create(&cfg);
    e3d_t *e = e3d_create(W, H, 2);
    /* the floor: x (depth) 0..900, z -400..400 at y = 0, 12 x 12 tiles */
    e3d_mesh *floor_m = e3d_mesh_floor(0, -400, 900, 400, 0, 12, 0xD8D0B8, 0x506070);
    e3d_mesh *ball_m = e3d_mesh_sphere(50, 24, 16, 230, 70, 60);   /* smooth: 24 x 16 */
    e3d_mesh *gem_m = e3d_mesh_sphere(60, 6, 3, 80, 200, 240);     /* 6 x 3: a faceted gem */
    if (!surf || !e || !floor_m || !ball_m || !gem_m) {
        papp_log("shadows: out of memory\n");
        return 1;
    }

    /* Camera: behind and above the scene, looking down a little */
    *e3d_cam(e) = (e3d_camera){ .pos = { -120, 260, 0 }, .pitch = -20 };
    e3d_params *par = e3d_params_get(e);
    par->clear_rgb565 = 0x18E7;
    par->shadows = true;
    par->shadow_y = 1;          /* just above the floor at y = 0 */
    par->shadow_dark = 0.4f;    /* shadowed floor keeps 40% of its colour */
    par->fog_rgb565 = 0x18E7;   /* fog fades to the background */

    e3d_object floor_o, ball, gem;
    e3d_object_init(&floor_o, floor_m);
    floor_o.flags = E3D_FLOOR;
    e3d_object_init(&ball, ball_m);
    e3d_object_init(&gem, gem_m);

    static const char *const style_name[4] = { "SHADED", "FLAT", "WIREFRAME", "FOG" };
    int style = 0;
    uint32_t prev = 0;
    float t = 0, fps = 0;
    int frames = 0;
    int64_t last = papp_time_us(), stat_t = last;
    char line[48];

    while (!papp_exit_requested()) {
        papp_gamepad_t pad;
        uint32_t held = papp_input_gamepad(0, &pad) ? pad.buttons : 0, press = held & ~prev;
        prev = held;
        if (press & PAPP_BTN_B)
            break;
        if (press & PAPP_BTN_Y) {
            style = (style + 1) & 3;
            par->fog_density = style == 3 ? 0.0015f : 0.f;
        }
        if (press & PAPP_BTN_X)
            par->shadows = !par->shadows;

        int64_t now = papp_time_us();
        t += (float)(now - last) * 1e-6f;
        last = now;
        uint32_t look = style == 1 ? E3D_FLAT : style == 2 ? E3D_WIRE : 0;

        /* the light circles above the scene */
        e3d_light *l = e3d_light_get(e);
        *l = (e3d_light){ .pos = { 450 + 260 * cosf(t * 0.6f), 420, 260 * sinf(t * 0.6f) },
                          .r = 255, .g = 245, .b = 220 };

        /* ball: bounces (|sin|), centre 50 above the floor at the bottom */
        e3d_mat m;
        e3d_move(&m, 420, 50 + 160 * fabsf(sinf(t * 2.2f)), -130);
        ball.m = m;
        ball.flags = look;

        /* gem: spins and hovers; always flat, it is meant to look faceted */
        e3d_mat spin;
        e3d_rotate(&spin, 0, t * 70, 20);
        e3d_move(&m, 520, 120 + 20 * sinf(t * 1.3f), 140);
        e3d_mul(&m, &spin);
        gem.m = m;
        gem.flags = E3D_FLAT | (look & E3D_WIRE);

        int stride;
        uint16_t *px = papp_disp_lock(surf, &stride);
        e3d_begin(e, px, stride);
        e3d_draw(e, &floor_o);
        e3d_draw(e, &ball);
        e3d_draw(e, &gem);
        e3d_draw_light(e);
        e3d_end(e);

        if (++frames == 30) {
            fps = 30e6f / (float)(now - stat_t);
            stat_t = now;
            frames = 0;
        }
        const e3d_stats *st = e3d_get_stats(e);
        snprintf(line, sizeof(line), "%s  %.0f FPS", style_name[style], (double)fps);
        e3d_text(px, stride, W, H, 4, 4, line, 0xFFFF, 1);
        snprintf(line, sizeof(line), "%d TRIS  %d SHADOW", st->tris_drawn, st->shadow_tris);
        e3d_text(px, stride, W, H, 4, 14, line, 0x07FF, 1);
        papp_disp_present(surf);
    }

    e3d_destroy(e);
    e3d_mesh_free(floor_m);
    e3d_mesh_free(ball_m);
    e3d_mesh_free(gem_m);
    papp_disp_surface_destroy(surf);
    return 0;
}
