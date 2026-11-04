/*
 * e3d on a bare ESP-IDF app (no launcher): the Guition JC1060P470C board.
 *
 * Renders a 400x240 scene on both cores (the main task on core 0 and the
 * engine's worker on core 1) and shows it scaled 2.5x on the 1024x600 panel
 * (display.c: JD9165 panel and PPA). Two frame buffers: the engine draws the
 * next frame while the PPA scales the last one.
 *
 * The scene: a tiled floor, five balls circling and bouncing around a
 * spinning gem, a moving light with its glow, shadows on the floor.
 */
#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include "e3d.h"
#include "display.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"

static const char *TAG = "e3d_demo";

#define W 400
#define H 240
#define NBALLS 5

void app_main(void)
{
    ESP_ERROR_CHECK(display_init(W, H));
    uint16_t *fb[2] = { display_alloc_frame(), display_alloc_frame() };
    e3d_t *e = e3d_create(W, H, 2);
    e3d_mesh *floor_m = e3d_mesh_floor(0, -450, 1000, 450, 0, 14, 0xE0D8C0, 0x405870);
    e3d_mesh *gem_m = e3d_mesh_sphere(70, 4, 2, 90, 210, 240);    /* 4 x 2: an octahedron */
    e3d_mesh *ball_m[NBALLS];
    static const uint32_t ball_rgb[NBALLS] = { 0xE03828, 0x30B048, 0x3060E0, 0xF0C020, 0xC040C0 };
    for (int i = 0; i < NBALLS; i++)
        ball_m[i] = e3d_mesh_sphere(32, 20, 12, (ball_rgb[i] >> 16) & 255, (ball_rgb[i] >> 8) & 255,
                                    ball_rgb[i] & 255);
    bool ok = fb[0] && fb[1] && e && floor_m && gem_m;
    for (int i = 0; i < NBALLS; i++)
        ok = ok && ball_m[i];
    if (!ok) {
        ESP_LOGE(TAG, "out of memory");
        return;
    }

    *e3d_cam(e) = (e3d_camera){ .pos = { -150, 300, 0 }, .pitch = -22 };
    e3d_params *par = e3d_params_get(e);
    par->clear_rgb565 = 0x10A6;
    par->shadow_dark = 0.4f;

    e3d_object floor_o, gem, ball[NBALLS];
    e3d_object_init(&floor_o, floor_m);
    floor_o.flags = E3D_FLOOR;
    e3d_object_init(&gem, gem_m);
    gem.flags = E3D_FLAT;                     /* faceted */
    for (int i = 0; i < NBALLS; i++)
        e3d_object_init(&ball[i], ball_m[i]);

    const float cx = 480;                     /* the middle of the scene, along the depth */
    int cur = 0, frames = 0;
    int64_t t0 = esp_timer_get_time(), log_t = t0;
    for (;;) {
        int64_t now = esp_timer_get_time();
        float t = (float)(now - t0) * 1e-6f;

        *e3d_light_get(e) = (e3d_light){ .pos = { cx + 300 * cosf(t * 0.5f), 500, 300 * sinf(t * 0.5f) },
                                         .r = 255, .g = 245, .b = 220 };
        e3d_mat spin, m;
        e3d_rotate(&spin, 0, t * 60, 0);
        e3d_move(&m, cx, 150 + 25 * sinf(t * 1.5f), 0);
        e3d_mul(&m, &spin);
        gem.m = m;
        for (int i = 0; i < NBALLS; i++) {
            float a = t * 0.8f + (float)i * 6.2831853f / NBALLS;
            e3d_move(&ball[i].m, cx + 260 * cosf(a), 32 + 110 * fabsf(sinf(t * 2.5f + (float)i)), 260 * sinf(a));
        }

        uint16_t *px = fb[cur];
        e3d_begin(e, px, W);
        e3d_draw(e, &floor_o);
        e3d_draw(e, &gem);
        for (int i = 0; i < NBALLS; i++)
            e3d_draw(e, &ball[i]);
        e3d_draw_light(e);
        e3d_end(e);
        e3d_text(px, W, W, H, 4, 4, "E3D ON ESP-IDF", 0xFFFF, 1);
        display_present(px);
        cur ^= 1;

        frames++;
        if (now - log_t >= 3000000) {
            const e3d_stats *st = e3d_get_stats(e);
            ESP_LOGI(TAG, "%.1f fps, %d tris + %d shadow, geometry %d us, raster %d us",
                     frames * 1e6 / (double)(now - log_t), st->tris_drawn, st->shadow_tris, st->t_geom_us,
                     st->t_raster_us);
            frames = 0;
            log_t = now;
        }
    }
}
