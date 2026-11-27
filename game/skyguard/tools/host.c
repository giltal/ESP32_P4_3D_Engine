/*
 * Skyguard on the desktop: runs the game and renderer (e3d's host build) with the
 * computer playing, and writes screenshots as PPM files. For tuning and checking
 * the looks without the board.
 *
 *   gcc -O2 -DE3D_HOST -DSG_HOST -I e3d/include -I e3d/src -I <launcher>/papp-sdk/include \
 *       -I <launcher>/components/papp_abi/include -o skyguard_sim game/skyguard/tools/host.c \
 *       e3d/src/e3d.c e3d/src/e3d_render.c -lm
 *   ./skyguard_sim <mode> <start wave> <seconds> <shot every s>
 *   mode: 0 = the computer plays a real game, 1 = title attract, 2 = nobody defends (game over),
 *         3 = like 0 without the HUD, 4 = the icon scene (icon.ppm)
 */
#define SG_HOST 1
#include <stdarg.h>
#include "../main.c"

void papp_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); }
static int64_t g_fake_t;
int64_t papp_time_us(void) { return g_fake_t; }
bool papp_exit_requested(void) { return false; }
void *papp_alloc(size_t n, uint32_t caps, size_t al) { (void)caps; (void)al; return calloc(1, n); }
void papp_free(void *p) { free(p); }
void papp_mutex_lock(papp_mutex_t *m) { (void)m; }
void papp_mutex_unlock(papp_mutex_t *m) { (void)m; }
size_t papp_audio_write(papp_audio_stream_t *s, const void *d, size_t n, uint32_t t) { (void)s; (void)d; (void)t; return n; }
bool papp_input_gamepad(int p, papp_gamepad_t *o) { (void)p; memset(o, 0, sizeof(*o)); return true; }
int papp_disp_touch(papp_surface_t *s, papp_touch_point_t *p, int m) { (void)s; (void)p; (void)m; return 0; }
int papp_input_analog(int ch) { (void)ch; return -1; }

static void save_ppm(const char *path, const uint16_t *px)
{
    FILE *f = fopen(path, "wb");
    fprintf(f, "P6 %d %d 255\n", W, H);
    for (int i = 0; i < W * H; i++) {
        uint16_t c = px[i];
        unsigned char rgb[3] = { (unsigned char)((c >> 11) << 3), (unsigned char)(((c >> 5) & 63) << 2), (unsigned char)((c & 31) << 3) };
        fwrite(rgb, 1, 3, f);
    }
    fclose(f);
}

int main(int argc, char **argv)
{
    int mode = argc > 1 ? atoi(argv[1]) : 0, wave = argc > 2 ? atoi(argv[2]) : 1;
    float dur = argc > 3 ? (float)atof(argv[3]) : 120.f, every = argc > 4 ? (float)atof(argv[4]) : 5.f;
    static uint16_t px[W * H];
    e3d_t *e = e3d_create(W, H, 1);
    if (!(e && b_init() && make_meshes())) { fprintf(stderr, "build failed\n"); return 1; }
    b_free();
    e3d_set_background_rows(e, bg_row, NULL);
    srand(1);
    if (mode == 4) {
        /* the icon: a staged moment over the left cities, cropped from the frame */
        new_game(3, false);
        g_state = ST_PLAY;
        g_time = 0;
        g_budget = 0;
        static const float tr[][4] = { { 122, -95, 46, -62 }, { 122, -20, 30, -44 }, { 122, 10, 58, -30 }, { 122, -60, 20, -20 } };
        for (int i = 0; i < 4; i++)
            g_en[i] = (enemy_t){ .on = true, .sy = tr[i][0], .sz = tr[i][1], .y = tr[i][2], .z = tr[i][3], .target = 1,
                                 .vy = tr[i][2] - tr[i][0], .vz = tr[i][3] - tr[i][1], .t = (float)i };
        g_abm[0] = (abm_t){ .on = true, .sy = BASE_TOP + 3, .sz = 0, .y = 40, .z = -14, .ty = 44, .tz = -16,
                            .vy = 40 - (BASE_TOP + 3), .vz = -14 };
        g_ex[0] = (expl_t){ .on = true, .y = 52, .z = -36, .t = 0.7f, .rmax = 11 };
        g_ex[1] = (expl_t){ .on = true, .y = 36, .z = -52, .t = 0.4f, .rmax = 10 };
        g_frame = 3;
        scene_begin(e, px);
        draw_scene(e);
        e3d_end(e);
        save_ppm("icon.ppm", px);
        return 0;
    }
    if (mode == 1) start_demo();
    else { new_game(wave, false); }
    float t = 0, next = 0; int shot = 0, nf = 0, maxtri = 0; double tg = 0;
    int last_wave = g_wave;
    while (t < dur) {
        float dt = 1.f / 30.f;
        g_fake_t += 33333;
        input_t in = mode == 2 ? (input_t){ .fire = -2 } : ai_input(dt);
        update(&in, dt);
        scene_begin(e, px);
        draw_scene(e);
        e3d_end(e);
        const e3d_stats *st = e3d_get_stats(e);
        nf++; if (st->tris_in > maxtri) maxtri = st->tris_in;
        tg += st->tris_drawn;
        if (mode != 3)
            draw_hud(px, 60.f);
        t += dt;
        if (g_wave != last_wave) {
            fprintf(stderr, "t %.1f wave %d score %d cities %d kills %d\n", t, g_wave, g_score, cities_alive(), g_kills);
            last_wave = g_wave;
        }
        if (t >= next) {
            char p[64];
            snprintf(p, sizeof p, "shot_%d_%03d.ppm", mode, shot++);
            save_ppm(p, px);
            fprintf(stderr, "shot %d t %.1f state %d wave %d score %d en %d objs %d tris %d/%d\n", shot - 1, t, g_state,
                    g_wave, g_score, enemies_on(), st->objects, st->tris_drawn, st->tris_in);
            next += every;
        }
        if (mode != 1 && g_state == ST_OVER && g_state_t > 3.f) break;
    }
    fprintf(stderr, "end: wave %d score %d kills %d; frames %d, avg tris drawn %.0f, max tris in %d\n", g_wave, g_score,
            g_kills, nf, tg / nf, maxtri);
    return 0;
}
