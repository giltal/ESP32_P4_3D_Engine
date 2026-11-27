/*
 * Cinderhorn on the desktop: runs the game's simulation and renderer (e3d's host build)
 * and writes screenshots as PPM files, for checking the worlds and the looks without the board.
 *
 *   gcc -O2 -DE3D_HOST -I e3d/include -I e3d/src -I <launcher>/papp-sdk/include \
 *       -I <launcher>/components/papp_abi/include -o cinderhorn_sim game/cinderhorn/tools/host.c \
 *       e3d/src/e3d.c e3d/src/e3d_render.c -lm
 *   ./cinderhorn_sim <level> <mode> <seconds> <shot every s> [res 0..3] [x z yaw]
 *   mode: 0 = the computer plays, 1 = title, 2 = a map of the height field (map_<level>.ppm),
 *         3 = stills from x z yaw (the dragon placed there)
 */
#define CH_HOST 1
#include <stdarg.h>
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
bool papp_input_gamepad(int p, papp_gamepad_t *o) { memset(o, 0, sizeof(*o)); return true; }
int papp_disp_touch(papp_surface_t *s, papp_touch_point_t *p, int m) { return 0; }

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

static void frame(e3d_t *e, uint16_t *px)
{
    scene_setup(e);
    e3d_begin(e, px, W);
    draw_scene(e);
    e3d_end(e);
    draw_hud(px, 30.f);
}

int main(int argc, char **argv)
{
    int lv = argc > 1 ? atoi(argv[1]) : 0, mode = argc > 2 ? atoi(argv[2]) : 0;
    float dur = argc > 3 ? (float)atof(argv[3]) : 30.f, every = argc > 4 ? (float)atof(argv[4]) : 5.f;
    res_init(1024, 600);
    g_res = argc > 5 ? atoi(argv[5]) : 0;
    g_w = k_res[g_res][0];
    g_h = k_res[g_res][1];
    g_ts = H >= 560 ? 3 : H >= 400 ? 2 : 1;
    srand(1);
    static uint16_t px[WMAX * HMAX];
    e3d_t *e = e3d_create(W, H, 1);
    g_e = e;
    if (!(e && b_init() && make_models() && level_build(lv))) { fprintf(stderr, "build failed\n"); return 1; }
    e3d_set_background_rows(e, bg_row, NULL);
    apply_look(e);
    int tris = 0;
    for (int i = 0; i < NCH * NCH; i++)
        for (int l = 0; l < NLOD; l++)
            if (g_chunk[i].land[l]) tris += g_chunk[i].land[l]->ntris;
    fprintf(stderr, "%s: land tris (all lods) %d, trees %d\n", g_lv->name, tris, g_ntree);
    if (mode == 2) {
        /* a map: 4 px per cell, coloured by height; things marked */
        int S = 4, N = GN * S;
        unsigned char *img = calloc((size_t)N * N * 3, 1);
        for (int y = 0; y < N; y++)
            for (int x = 0; x < N; x++) {
                float wx = -HALF + (float)x / S * CELL, wz = -HALF + (float)y / S * CELL;
                float h = ground_h(wx, wz);
                int pl; float s = support_h(wx, wz, 100.f, &pl);
                unsigned char *p = img + ((size_t)y * N + x) * 3;
                if (pl >= 0) { p[0] = 230; p[1] = 120; p[2] = 230; continue; }
                if (h < 0) { p[0] = 20; p[1] = 60; p[2] = (unsigned char)(140 + h * 10); continue; }
                int v = (int)(60 + h * 12); if (v > 255) v = 255;
                int band = ((int)floorf(h)) & 1;
                p[0] = (unsigned char)(v * (band ? 9 : 10) / 10); p[1] = (unsigned char)v; p[2] = (unsigned char)(v / 2);
                (void)s;
            }
        #define MARK(wx, wz, r, g, b) do { int cx = (int)(((wx) + HALF) / CELL * S), cy = (int)(((wz) + HALF) / CELL * S); \
            for (int dy = -3; dy <= 3; dy++) for (int dx = -3; dx <= 3; dx++) { int X = cx + dx, Y = cy + dy; \
            if (X >= 0 && Y >= 0 && X < N && Y < N) { unsigned char *q = img + ((size_t)Y * N + X) * 3; q[0] = r; q[1] = g; q[2] = b; } } } while (0)
        level_reset();
        for (int i = 0; i < 4; i++) MARK(g_beacon[i].p[0], g_beacon[i].p[2], 255, 60, 0);
        for (int i = 0; i < g_nen; i++) MARK(g_en[i].p[0], g_en[i].p[2], 255, 0, 0);
        for (int i = 0; i < MAX_GEMS; i++) if (g_gem[i].state) MARK(g_gem[i].p[0], g_gem[i].p[2], 255, 255, 0);
        for (int i = 0; i < g_nbox; i++) MARK(g_box[i].p[0], g_box[i].p[2], 140, 70, 20);
        MARK(g_lv->spawn[0], g_lv->spawn[1], 255, 255, 255);
        MARK(g_lv->portal[0], g_lv->portal[1], 160, 0, 255);
        char path[64]; snprintf(path, sizeof path, "map_%d.ppm", lv);
        FILE *f = fopen(path, "wb"); fprintf(f, "P6 %d %d 255\n", N, N); fwrite(img, 1, (size_t)N * N * 3, f); fclose(f);
        fprintf(stderr, "gems total %d\n", g_gem_total);
        return 0;
    }
    if (mode == 3) {
        level_reset();
        g_state = ST_PLAY;
        float sp[3] = { (float)atof(argv[6]), 0, (float)atof(argv[7]) };
        sp[1] = ground_h(sp[0], sp[2]);
        respawn_at(sp, (float)atof(argv[8]));
        P.inv_t = 0;
        input_t none = { 0 };
        for (int i = 0; i < 60; i++) { g_fake_t += 33333; update(&none, 1.f / 30.f); }
        frame(e, px);
        save_ppm("still.ppm", px);
        const e3d_stats *st = e3d_get_stats(e);
        fprintf(stderr, "objs %d tris %d/%d\n", st->objects, st->tris_drawn, st->tris_in);
        return 0;
    }
    if (mode == 4 || mode == 5) {
        /* a close-up of the dragon from the front-left (mode 5: gliding, breathing fire) */
        level_reset();
        g_state = ST_PLAY;
        float sp[3] = { (float)atof(argv[6]), 0, (float)atof(argv[7]) };
        sp[1] = ground_h(sp[0], sp[2]);
        respawn_at(sp, (float)atof(argv[8]));
        P.inv_t = 0;
        input_t none = { 0 };
        for (int i = 0; i < 30; i++) { g_fake_t += 33333; update(&none, 1.f / 30.f); }
        if (mode == 5) {
            P.mode = M_GLIDE; P.wing = 1.f; P.ground = false; P.p[1] += 3.f;
            input_t f = { 0 }; f.fire = true;
            update(&f, 1.f / 30.f);
            for (int i = 0; i < 6; i++) { P.v[1] = 0; P.p[1] += 0.0f; update(&none, 1.f / 30.f); P.mode = M_GLIDE; P.wing = 1.f; }
        }
        float a = P.yaw + (float)atof(argv[9]) * RAD, d = argc > 10 ? (float)atof(argv[10]) : 3.2f;
        if (argc > 11) g_focal = (float)atof(argv[11]);
        float lift = argc > 12 ? (float)atof(argv[12]) : 1.1f;
        float c[3] = { P.p[0] + cosf(a) * d, P.p[1] + lift, P.p[2] + sinf(a) * d };
        memcpy(g_cam_p, c, sizeof c);
        float dx = P.p[0] + 0.05f - c[0], dy = P.p[1] + 0.55f - c[1], dz = P.p[2] - c[2];
        g_cam_h = atan2f(dz, dx) * DEG;
        g_cam_pt = atan2f(dy, sqrtf(dx * dx + dz * dz)) * DEG;
        scene_setup(e); e3d_begin(e, px, W); draw_scene(e); e3d_end(e);
        save_ppm("close.ppm", px);
        return 0;
    }
    if (mode == 6) {
        /* a scripted run from x z yaw: tokens "w<s>" hold forward, "j" press A, "i<s>" idle, "f" fire */
        level_reset();
        g_state = ST_PLAY;
        float sp[3] = { (float)atof(argv[6]), 0, (float)atof(argv[7]) };
        sp[1] = ground_h(sp[0], sp[2]);
        respawn_at(sp, (float)atof(argv[8]));
        P.inv_t = 0;
        const char *q = argv[9];
        float tt = 0; int fr = 0;
        while (*q) {
            char c = *q++;
            float secs = 0;
            if (c == 'w' || c == 'i' || c == 'c') { secs = strtof(q, (char **)&q); }
            int n = c == 'j' || c == 'f' ? 1 : (int)(secs * 30.f + 0.5f);
            for (int k = 0; k < n; k++) {
                input_t in = { 0 };
                in.my = (c == 'w' || c == 'c' || c == 'j' || c == 'f') ? 1.f : 0.f;
                in.jump = c == 'j';
                in.fire = c == 'f';
                in.charge = c == 'c';
                g_fake_t += 33333; update(&in, 1.f / 30.f); tt += 1.f / 30.f;
                if ((fr++ % 6) == 0)
                    fprintf(stderr, "t %.2f %c pos %6.1f %5.2f %6.1f v %5.1f %5.1f %5.1f mode %d gnd %d plat %d hp %d\n", tt, c, P.p[0], P.p[1], P.p[2], P.v[0], P.v[1], P.v[2], P.mode, P.ground, P.plat, P.hp);
            }
        }
        frame(e, px);
        save_ppm("script.ppm", px);
        return 0;
    }
    if (mode == 7) {
        /* light the beacons one by one (placed in front of each, fire), then walk into the portal */
        start_play();
        input_t none = { 0 }, fire = { 0 }, fwd = { 0 };
        fire.fire = true;
        fwd.my = 1.f;
        for (int b = 0; b < 4; b++) {
            float best = -1, bx = 0, bz = 0;
            for (int k = 0; k < 16; k++) {   /* a standing place 3.5 m away, at the beacon's height */
                float a = (float)k * 0.3927f, x = g_beacon[b].p[0] + cosf(a) * 3.5f, z = g_beacon[b].p[2] + sinf(a) * 3.5f;
                float h = support_h(x, z, g_beacon[b].p[1] + 1.f, NULL);
                if (fabsf(h - g_beacon[b].p[1]) < 0.6f && best < 0) { best = h; bx = x; bz = z; }
            }
            float sp[3] = { bx, best, bz };
            respawn_at(sp, atan2f(g_beacon[b].p[2] - bz, g_beacon[b].p[0] - bx));
            g_cam_yaw = P.yaw;
            for (int i = 0; i < 10; i++) { g_fake_t += 33333; update(&none, 1.f / 30.f); }
            update(&fire, 1.f / 30.f);
            for (int i = 0; i < 20; i++) { g_fake_t += 33333; update(&none, 1.f / 30.f); }
            fprintf(stderr, "beacon %d at %.0f %.0f: %s (stand %.1f vs %.1f)\n", b, g_beacon[b].p[0], g_beacon[b].p[2], g_beacon[b].lit ? "lit" : "NOT LIT", best, g_beacon[b].p[1]);
            if (b == 0) { frame(e, px); save_ppm("beacon.ppm", px); }
        }
        float a = g_portal_yaw;
        float sp[3] = { g_portal_p[0] + cosf(a) * 6.f, 0, g_portal_p[2] + sinf(a) * 6.f };
        sp[1] = ground_h(sp[0], sp[2]);
        respawn_at(sp, a + 3.14159f);
        g_cam_yaw = P.yaw;
        for (int i = 0; i < 20; i++) { g_fake_t += 33333; update(&none, 1.f / 30.f); }
        frame(e, px); save_ppm("portal.ppm", px);
        for (int i = 0; i < 60 && g_state == ST_PLAY; i++) { g_fake_t += 33333; update(&fwd, 1.f / 30.f); }
        for (int i = 0; i < 45; i++) { g_fake_t += 33333; update(&none, 1.f / 30.f); }
        fprintf(stderr, "state %d (2 = cleared)\n", g_state);
        frame(e, px); save_ppm("done.ppm", px);
        return 0;
    }
    if (mode == 8) {
        /* the creatures in a row, and the props, in front of the dragon */
        level_reset();
        g_state = ST_PLAY;
        float x0 = -20, z0 = 10;
        float sp[3] = { x0, ground_h(x0, z0), z0 };
        respawn_at(sp, 0);
        P.inv_t = 0;
        for (int i = 0; i < 3; i++) {
            enemy_t *en = &g_en[i];
            en->alive = 1; en->type = (uint8_t)i; en->state = 0; en->yaw = 3.14159f;
            v3_set(en->p, x0 + 7.f, 0, z0 - 3.f + 3.f * (float)i);
            en->p[1] = ground_h(en->p[0], en->p[2]);
        }
        g_nen = 3;
        g_nbox = 2;
        v3_set(g_box[0].p, x0 + 4.f, 0, z0 - 5.f); g_box[0].p[1] = ground_h(g_box[0].p[0], g_box[0].p[2]); g_box[0].kind = T_URN; g_box[0].alive = 1;
        v3_set(g_box[1].p, x0 + 4.f, 0, z0 + 5.f); g_box[1].p[1] = ground_h(g_box[1].p[0], g_box[1].p[2]); g_box[1].kind = T_CHEST; g_box[1].alive = 1;
        add_gem(x0 + 3.f, sp[1] + 0.9f, z0 - 2.f, GEM_EMERALD, false);
        add_gem(x0 + 3.f, sp[1] + 0.9f, z0, GEM_SAPPHIRE, false);
        add_gem(x0 + 3.f, sp[1] + 0.9f, z0 + 2.f, GEM_STAR, false);
        v3_set(g_cam_p, x0 - 2.5f, sp[1] + 2.2f, z0);
        g_cam_h = 0; g_cam_pt = -8.f;
        frame(e, px);
        save_ppm("lineup.ppm", px);
        return 0;
    }
    g_auto = mode == 0;
    if (mode == 0) start_play(); else start_attract();
    float t = 0, next = 0; int shot = 0, nf = 0, maxtri = 0; double tg = 0;
    while (t < dur) {
        float dt = 1.f / 30.f;
        g_fake_t += 33333;
        input_t in = mode == 0 ? ai_input(dt) : (input_t){ 0 };
        update(&in, dt);
        frame(e, px);
        const e3d_stats *st = e3d_get_stats(e);
        nf++; if (st->tris_in > maxtri) maxtri = st->tris_in;
        tg += st->tris_drawn;
        t += dt;
        if (t >= next) {
            char p[64];
            snprintf(p, sizeof p, "shot_%d_%d_%03d.ppm", lv, mode, shot++);
            save_ppm(p, px);
            fprintf(stderr, "t %.1f pos %.1f %.1f %.1f mode %d hp %d gems %d/%d lit %d objs %d tris %d/%d\n", t, P.p[0], P.p[1], P.p[2], P.mode, P.hp,
                    g_gem_got, g_gem_total, g_lit, st->objects, st->tris_drawn, st->tris_in);
            next += every;
        }
    }
    fprintf(stderr, "frames %d, avg tris drawn %.0f, max tris in %d\n", nf, tg / nf, maxtri);
    return 0;
}
