/*
 * Skyrift on the desktop: runs the game's simulation and renderer (e3d's host build) with
 * the computer flying every ship, and writes screenshots as PPM files. For tuning the AI
 * and checking the looks without the board.
 *
 *   gcc -O2 -DE3D_HOST -DSKY_HOST -I e3d/include -I e3d/src -I <launcher>/papp-sdk/include \
 *       -I <launcher>/components/papp_abi/include -o skyrift_sim game/skyrift/tools/host.c \
 *       e3d/src/e3d.c e3d/src/e3d_render.c -lm
 *   ./skyrift_sim <track> <class> <mode> <seconds> <shot every s> [res 0..3]
 *   mode: 0 = race (computer), 1 = title attract, 2 = icon close-up, 3 = race with player input
 */
#define SKY_HOST 1
#include <stdarg.h>
#include <time.h>
#include "../main.c"

void papp_log(const char *fmt, ...) { va_list a; va_start(a, fmt); vfprintf(stderr, fmt, a); va_end(a); }
static int64_t g_fake_t;
int64_t papp_time_us(void) { return g_fake_t; }
bool papp_exit_requested(void) { return false; }
void *papp_alloc(size_t n, uint32_t caps, size_t al) { (void)caps; (void)al; return calloc(1, n); }
void papp_free(void *p) { free(p); }
void papp_mutex_lock(papp_mutex_t *m) {}
void papp_mutex_unlock(papp_mutex_t *m) {}
size_t papp_audio_write(papp_audio_stream_t *s, const void *d, size_t n, uint32_t t) { return n; }
static uint32_t g_btn;
bool papp_input_gamepad(int p, papp_gamepad_t *o) { memset(o, 0, sizeof(*o)); o->buttons = g_btn; return true; }
int papp_disp_touch(papp_surface_t *s, papp_touch_point_t *p, int m) { return 0; }
int papp_input_analog(int ch) { return -1; }

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
    /* host_main track class mode shots... : mode 0 = auto race, 1 = title */
    int trk = argc > 1 ? atoi(argv[1]) : 0, cls = argc > 2 ? atoi(argv[2]) : 1, mode = argc > 3 ? atoi(argv[3]) : 0;
    float dur = argc > 4 ? (float)atof(argv[4]) : 60.f, every = argc > 5 ? (float)atof(argv[5]) : 5.f;
    g_track_i = trk; g_td = &k_track[trk];
    g_class = cls; g_cl = &k_class[cls];
    g_laps = 3;
    g_res = argc > 6 ? atoi(argv[6]) : 1;
    g_w = k_res[g_res][0];
    g_h = k_res[g_res][1];
    srand(1);
    static uint16_t px[WMAX * HMAX];
    e3d_t *e = e3d_create(W, H, 1);
    g_e = e;
    if (!(e && b_init() && make_meshes() && track_build())) { fprintf(stderr, "build failed\n"); return 1; }
    b_free();
    e3d_set_background_rows(e, bg_row, NULL);
    apply_look(e);
    g_show_fps = false;
    if (mode == 2) {
        g_auto = true; start_race();
        for (int i = 0; i < 200; i++) update(&(input_t){0}, 1.f / 30.f);   /* into the race */
        ship_t *s = PLAYER;
        float p[3];
        for (int q = 0; q < 3; q++) p[q] = s->pos[q] + s->fwd[q] * 5.2f + s->right[q] * 3.6f + s->up[q] * 1.6f;
        float d[3] = { s->pos[0] - p[0], s->pos[1] + 0.2f - p[1], s->pos[2] - p[2] };
        memcpy(g_cam_p, p, sizeof p);
        g_cam_h = atan2f(d[2], d[0]) * DEG;
        g_cam_pitch = atan2f(d[1], sqrtf(d[0] * d[0] + d[2] * d[2])) * DEG;
        g_focal = 300.f;
        s->flame = 1.2f; s->boost_t = 1.f;
        e3d_begin(e, px, W); draw_scene(e); e3d_end(e);
        e3d_begin(e, px, W); draw_scene(e); e3d_end(e);
        save_ppm("icon.ppm", px);
        return 0;
    }
    if (mode == 3) { start_race(); }
    else if (mode == 1) start_attract();
    else { g_auto = true; start_race(); }
    float t = 0, next = 0; int shot = 0;
    input_t in = { 0 };
    double tg = 0, tr = 0; int nf = 0, maxtri = 0;
    while (t < dur) {
        float dt = 1.f / 30.f;
        g_fake_t += 33333;
        if (mode == 3 && PLAYER->human) {
            ship_t tmp = *PLAYER;
            ai_drive(&tmp, dt);
            in.steer = tmp.steer; in.thrust = tmp.thrust; in.ab_l = tmp.ab_l; in.ab_r = tmp.ab_r;
            in.fire = PLAYER->weapon == WPN_ROCKET || PLAYER->weapon == WPN_TURBO || PLAYER->weapon == WPN_MINES;
            in.absorb = PLAYER->weapon == WPN_SHIELD && PLAYER->energy < 70.f;
        }
        update(&in, dt);
        e3d_begin(e, px, W);
        draw_scene(e);
        e3d_end(e);
        const e3d_stats *st = e3d_get_stats(e);
        nf++; if (st->tris_in > maxtri) maxtri = st->tris_in;
        tg += st->tris_drawn;
        draw_hud(px, 60.f);
        t += dt;
        if (t >= next) {
            char p[64];
            snprintf(p, sizeof p, "shot_%d_%d_%03d.ppm", trk, mode, shot++);
            save_ppm(p, px);
            fprintf(stderr, "t %.1f state %d lap %d place %d spd %.0f en %.0f u %.0f x %.1f objs %d tris %d/%d\n", t, g_state,
                    PLAYER->lap, PLAYER->place, PLAYER->va * SPEED_KMH, PLAYER->energy, PLAYER->u, PLAYER->x, st->objects, st->tris_drawn, st->tris_in);
            next += every;
        }
        if (g_state == ST_RESULTS && g_state_t > 3.f && mode == 0) break;
    }
    fprintf(stderr, "frames %d, avg tris drawn %.0f, max tris in %d\n", nf, tg / nf, maxtri);
    for (int i = 0; i < NSHIPS; i++) {
        const ship_t *s = &g_ship[g_order[i]];
        fprintf(stderr, "%d %s fin %d t %.2f best %.2f dead %d en %.0f wall %.0f rkt %.0f mine %.0f\n", i + 1, k_team[s->team].name, s->finished, s->finish_t, s->best_lap, s->dead, s->energy, g_dbg[g_order[i]][0], g_dbg[g_order[i]][1], g_dbg[g_order[i]][2]);
    }
    return 0;
}
