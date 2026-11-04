/*
 * 3D Engine demo — the original demoMain.c show, ported to the e3d engine as a
 * PAPP. 400x240 RGB565 surface, scaled by the launcher.
 *
 * The show, in the original's world coordinates:
 *   ARE / YOU / READY   letters fly past the camera, spinning
 *   9 .. 0              countdown numbers
 *   approach            the camera rolls down towards the floor, light glow
 *   balls               five balls drop and bounce, with shadows
 *   logos               the ringed M rises and flies in, the striped logo assembles
 *   main                everything turns, balls roll and collide, a ring of text
 *                       circles the scene; then the camera flies away and it loops
 *
 * Speeds are per frame of the original (FPS0 frames a second), so the show
 * runs at the same pace whatever the frame rate. Motions are computed from the
 * start pose and the elapsed frames, not accumulated, so nothing drifts.
 *
 * Controls: A next part, START restart, Y style (shaded / flat / wireframe /
 * fog), X statistics, B exit. In the main part the D-pad turns and moves the
 * camera, L1 / R1 tilt it up and down.
 */
#include "papp/papp.h"
#include "e3d.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

PAPP_MANIFEST(.id = "demo.e3d", .name = "3D Engine", .version = PAPP_APP_VERSION, .main_stack = 24576);

#define W     400
#define H     240
#define FPS0  30.f

extern const char asset_letters3d[], asset_numbers3d[], asset_marvell3d[], asset_motorola3d[];

/* ── Scene ───────────────────────────────────────────────────────────────── */

typedef struct {
    e3d_object o;
    e3d_mat m0;                 /* pose at the start of its part */
    float c0[3];                /* its centre then */
} item;

typedef struct {
    e3d_object o;
    float p[3], v[3];           /* centre and velocity, per original frame */
    e3d_mat rot;
    float squash;
    bool rest;
    int renorm;
} ball_t;

static e3d_t *g_e;
static e3d_mesh *m_letters, *m_numbers, *m_marvell, *m_moto, *m_ball[5], *m_floor;
static int n_letters, n_numbers, n_marvell, n_moto;

static item ready[11];
static const int ready_first[4] = { 0, 3, 6, 11 };
static const int ready_mid[3] = { 1, 4, 8 };
static const float ready_end[3] = { -2500.f, -2500.f, -1500.f };
static item count[10];
static float count_start[10], count_end;
static item marv[10];
static e3d_mat marv_final[10];
static float marv_center[3];
static item moto[2];
static e3d_mat moto_final[2];
static float moto_c1[3], moto_p0[3];
static item ring[80];
static int n_ring;
static e3d_object floor_o;
static ball_t balls[5];

/* The launcher's present scales the frame with the PPA and waits for it. A
 * presenter thread does that while the next frame is drawn into the other
 * of two buffers. */
static papp_surface_t *g_surf;
static papp_sem_t *g_pres_go, *g_pres_done;
static const uint16_t *volatile g_pres_buf;

static int presenter_main(void *arg)
{
    for (;;) {
        if (!papp_sem_take(g_pres_go, 100))
            continue;                   /* main ends it with a NULL buffer */
        const uint16_t *buf = g_pres_buf;
        if (!buf)
            break;
        papp_disp_present_ext(g_surf, buf);
        papp_sem_give(g_pres_done);
    }
    return 0;
}

enum { PH_ARE, PH_YOU, PH_READY, PH_COUNT, PH_APPROACH, PH_BALLS, PH_MOTO, PH_MARVELL, PH_MAIN, PH_AWAY, PH_N };
static const char *const phase_name[PH_N] = { "ARE", "YOU", "READY", "COUNTDOWN", "APPROACH", "BALLS",
                                              "LOGO", "LOGO", "MAIN", "AWAY" };
static int g_phase;
static float g_pf;              /* original frames into the phase */
static float g_main_t;          /* main part clock, keeps running while flying away */
static uint32_t g_style_flags;
static int g_style;
static bool g_hud = true;

/* free camera in the main part */
static float g_user_heading, g_user_pitch, g_user_off[3];
static float g_cam_base[3];

static void item_init(item *it, const e3d_mesh *mesh, float dx, float dy, float dz)
{
    e3d_object_init(&it->o, mesh);
    e3d_mat m;
    e3d_move(&m, dx, dy, dz);
    e3d_transform(&it->o, &m);
    it->m0 = it->o.m;
    e3d_object_pos(&it->o, it->c0);
}

static void item_rebase(item *it)
{
    it->m0 = it->o.m;
    e3d_object_pos(&it->o, it->c0);
}

/* the original's per-frame "move(d) * rotate about the centre": after f frames
 * that is move(f*d) * rotate(f*deg) about the starting centre c */
static void spin_move(item *it, const float c[3], float f, float dx, float dy, float dz,
                      float ax, float ay, float az, float deg)
{
    e3d_mat r, t;
    e3d_rotate_about(&r, c, ax, ay, az, deg * f);
    e3d_mul(&r, &it->m0);
    e3d_move(&t, dx * f, dy * f, dz * f);
    e3d_mul(&t, &r);
    it->o.m = t;
}

static const e3d_mesh *glyph(char ch)
{
    if (ch >= 'A' && ch <= 'Z' && ch - 'A' < n_letters)
        return &m_letters[ch - 'A'];
    if (ch >= '0' && ch <= '9' && ch - '0' < n_numbers)
        return &m_numbers[ch - '0'];
    return NULL;
}

/* 3D text: letters 40 apart along z, starting at z, as the original laid them out */
static int place_text(item *dst, int max, const char *s, float x, float y, float z)
{
    int n = 0;
    for (int i = 0; s[i] && n < max; i++) {
        const e3d_mesh *g = glyph(s[i]);
        if (g)
            item_init(&dst[n++], g, x, y, z + 40.f * (float)i);
    }
    return n;
}

/* The sky, drawn a row at a time by the engine: a gradient and stars. */
#define NSTARS 160
static uint16_t sky_row_rgb[H];
static uint16_t sky_first[H + 1];          /* stars of row y: sky_first[y] .. sky_first[y + 1] */
static uint16_t star_x[NSTARS], star_rgb[NSTARS];

static void make_sky(void)
{
    for (int y = 0; y < H; y++) {
        float t = (float)y / (float)(H - 1);
        int r = (int)(10 + 70 * t * t), g = (int)(12 + 30 * t * t), b = (int)(40 + 90 * t);
        sky_row_rgb[y] = PAPP_RGB565(r, g, b);
    }
    int sy[NSTARS], sx[NSTARS], sv[NSTARS], count[H + 1];
    memset(count, 0, sizeof(count));
    uint32_t seed = 12345;
    for (int i = 0; i < NSTARS; i++) {
        seed = seed * 1664525u + 1013904223u;
        sx[i] = (int)((seed >> 8) % W);
        seed = seed * 1664525u + 1013904223u;
        sy[i] = (int)((seed >> 8) % (H * 2 / 3));
        sv[i] = 120 + (int)((seed >> 4) % 136);
        count[sy[i] + 1]++;
    }
    for (int y = 0; y < H; y++)
        count[y + 1] += count[y];
    for (int y = 0; y <= H; y++)
        sky_first[y] = (uint16_t)count[y];
    for (int i = 0; i < NSTARS; i++) {
        int k = count[sy[i]]++;
        star_x[k] = (uint16_t)sx[i];
        star_rgb[k] = PAPP_RGB565(sv[i], sv[i], sv[i]);
    }
}

static void sky_row(uint16_t *row, int y, int w, void *ctx)
{
    uint32_t c = sky_row_rgb[y] * 0x10001u, *p = (uint32_t *)row;
    for (int x = 0; x < w / 2; x++)
        p[x] = c;
    for (int k = sky_first[y]; k < sky_first[y + 1]; k++)
        row[star_x[k]] = star_rgb[k];
}

/* striped beach balls, so the rolling shows */
static e3d_mesh *make_ball(int slices, int stacks, uint32_t c1, uint32_t c2)
{
    e3d_mesh *m = e3d_mesh_sphere(30.f, slices, stacks, 255, 255, 255);
    if (!m)
        return NULL;
    for (int t = 0; t < m->ntris; t++) {
        int slice = t < 2 * slices ? t / 2 : ((t - 2 * slices) / 2) % slices;
        uint32_t c = (slice / 2) & 1 ? c2 : c1;
        m->tris[t].r = (uint8_t)(c >> 16);
        m->tris[t].g = (uint8_t)(c >> 8);
        m->tris[t].b = (uint8_t)c;
    }
    return m;
}

static void scene_reset(void)
{
    e3d_camera *cam = e3d_cam(g_e);
    *cam = (e3d_camera){ .pos = { -5000, 700, 100 }, .pitch = -5 };
    e3d_light *l = e3d_light_get(g_e);
    *l = (e3d_light){ .pos = { -5850, 950, 600 }, .r = 240, .g = 240, .b = 200 };
    g_user_heading = g_user_pitch = 0;
    memset(g_user_off, 0, sizeof(g_user_off));

    /* ARE YOU READY */
    static const char *const words[3] = { "ARE", "YOU", "READY" };
    static const float z0[3] = { 50, 50, 20 };
    int n = 0;
    for (int w = 0; w < 3; w++)
        n += place_text(&ready[n], 11 - n, words[w], -5200, 670, z0[w]);

    /* countdown 9 .. 0: each starts when the one before is 40 frames out */
    for (int k = 0; k < 10 && k < n_numbers; k++) {
        item_init(&count[k], &m_numbers[k], -5200, 670, (k & 1) ? 100 : 50);
        static const uint32_t col[10] = { 0xF00000, 0x00F000, 0x0000F0, 0xF000F0, 0xF0F000,
                                          0x00F0F0, 0xF07878, 0x78F078, 0x7878F0, 0xF0F0F0 };
        e3d_object_color(&count[k].o, col[k] >> 16, (col[k] >> 8) & 255, col[k] & 255);
    }
    count_start[9] = 0;
    for (int k = 8; k >= 0; k--)
        count_start[k] = count_start[k + 1] + (-4400.f - count[k + 1].c0[0]) / 20.f;
    count_end = count_start[0] + (-2500.f - count[0].c0[0]) / 20.f;

    /* balls high above the floor */
    static const float bp[5][3] = { { 300, 750, 350 }, { 50, 650, -80 }, { 100, 700, 450 },
                                    { -100, 600, 200 }, { 400, 800, 0 } };
    static const float bv[5][2] = { { 6, 3 }, { -4, 6 }, { 5, -4 }, { 4, 6 }, { -6, -4 } };
    for (int k = 0; k < 5; k++) {
        ball_t *b = &balls[k];
        e3d_object_init(&b->o, m_ball[k]);
        memcpy(b->p, bp[k], sizeof(b->p));
        b->v[0] = bv[k][0];
        b->v[1] = 0;
        b->v[2] = bv[k][1];
        e3d_identity(&b->rot);
        b->squash = 0;
        b->rest = false;
        b->renorm = 0;
    }

    /* logos: scaled to 0.8 about their centres, the M below the floor, its ring behind */
    static const float mpos[2][3] = { { 80, -220, 100 }, { -880, 190, 100 } };
    for (int k = 0; k < 2 && k < n_moto; k++) {
        e3d_object_init(&moto[k].o, &m_moto[k]);
        e3d_mat s, m;
        e3d_scale_about(&s, m_moto[k].center, 0.8f, 0.8f, 0.8f);
        e3d_move(&m, mpos[k][0], mpos[k][1], mpos[k][2]);
        e3d_mul(&m, &s);
        e3d_transform(&moto[k].o, &m);
        item_rebase(&moto[k]);
    }

    /* striped logo: 10 pieces from 3 parts, 720 out along their paths */
    static const struct { int part; float pre[3], off[3]; } mv[10] = {
        { 0, { 0, 0, 0 },     { 720, 720, 720 } },  { 1, { 0, 0, 0 },     { 720, -720, -720 } },
        { 2, { 0, 0, 0 },     { 0, 0, 720 } },      { 2, { 0, -15, 0 },   { 0, -720, -720 } },
        { 2, { 0, -30, 0 },   { 0, -720, 720 } },   { 0, { 0, 0, 100 },   { 0, 720, -720 } },
        { 1, { 0, 0, 100 },   { 0, 720, 720 } },    { 2, { 0, 0, 100 },   { 720, -720, 720 } },
        { 2, { 0, -15, 100 }, { 720, 720, -720 } }, { 2, { 0, -30, 100 }, { -720, 0, 0 } },
    };
    for (int k = 0; k < 10; k++)
        item_init(&marv[k], &m_marvell[mv[k].part], mv[k].pre[0] + mv[k].off[0],
                  mv[k].pre[1] + mv[k].off[1] + 250, mv[k].pre[2] + mv[k].off[2]);

    /* the ring of text: lines laid out at x = -250, turned about (800, 1000, 200) */
    static const struct { const char *s; float angle; bool yellow; } lines[] = {
        { "ESP32 P4", 0, false },    { "DUAL RISC V", -20, true }, { "360 MHZ", -55, false },
        { "32MB PSRAM", -80, true }, { "MIPI DSI", -120, false },  { "3D ENGINE", 35, true },
    };
    n_ring = 0;
    for (size_t i = 0; i < sizeof(lines) / sizeof(lines[0]); i++) {
        int first = n_ring;
        n_ring += place_text(&ring[n_ring], 80 - n_ring, lines[i].s, -250, 1000, -50);
        const float axis_at[3] = { 800, 1000, 200 };
        for (int k = first; k < n_ring; k++) {
            e3d_mat r;
            e3d_rotate_about(&r, axis_at, 0, 1, 0, lines[i].angle);
            e3d_transform(&ring[k].o, &r);
            if (lines[i].yellow)
                e3d_object_color(&ring[k].o, 240, 240, 0);
            ring[k].o.flags |= E3D_NO_SHADOW;
            item_rebase(&ring[k]);
        }
    }

    g_phase = PH_ARE;
    g_pf = 0;
    g_main_t = 0;
}

static void draw(e3d_object *o)
{
    uint32_t keep = o->flags;
    o->flags |= g_style_flags;
    e3d_draw(g_e, o);
    o->flags = keep;
}

/* ── Parts of the show ───────────────────────────────────────────────────── */

static void ball_matrix(ball_t *b)
{
    float s = b->squash > 0 ? b->squash : 0;
    e3d_mat m, t;
    const float foot[3] = { 0, -30, 0 };
    e3d_scale_about(&m, foot, 1.f + 0.12f * s, 1.f - 0.2f * s, 1.f + 0.12f * s);
    e3d_mul(&m, &b->rot);
    e3d_move(&t, b->p[0], b->p[1], b->p[2]);
    e3d_mul(&t, &m);
    b->o.m = t;
}

/* drop with the original's gravity (10 per frame squared), bounce, squash */
static bool balls_fall(float dk)
{
    bool all = true;
    for (int k = 0; k < 5; k++) {
        ball_t *b = &balls[k];
        b->squash -= dk / 4.f;
        if (!b->rest) {
            for (float left = dk; left > 0.f; left -= 0.5f) {
                float h = left < 0.5f ? left : 0.5f;
                b->v[1] -= 10.f * h;
                b->p[1] += b->v[1] * h;
                if (b->p[1] <= 30.f && b->v[1] < 0.f) {
                    b->p[1] = 30.f;
                    b->v[1] = -b->v[1] * 0.72f;
                    b->squash = 1.f;
                    if (b->v[1] < 6.f) {
                        b->v[1] = 0;
                        b->rest = true;
                        break;
                    }
                }
            }
            all = false;
        }
        ball_matrix(b);
    }
    return all;
}

static void orthonormalise(e3d_mat *m)
{
    float *c0[3] = { &m->m[0][0], &m->m[1][0], &m->m[2][0] };
    float x[3] = { m->m[0][0], m->m[1][0], m->m[2][0] }, y[3] = { m->m[0][1], m->m[1][1], m->m[2][1] };
    float l = sqrtf(x[0] * x[0] + x[1] * x[1] + x[2] * x[2]);
    for (int i = 0; i < 3; i++) x[i] /= l;
    float d = x[0] * y[0] + x[1] * y[1] + x[2] * y[2];
    for (int i = 0; i < 3; i++) y[i] -= d * x[i];
    l = sqrtf(y[0] * y[0] + y[1] * y[1] + y[2] * y[2]);
    for (int i = 0; i < 3; i++) y[i] /= l;
    float z[3] = { x[1] * y[2] - x[2] * y[1], x[2] * y[0] - x[0] * y[2], x[0] * y[1] - x[1] * y[0] };
    for (int i = 0; i < 3; i++) {
        *c0[i] = x[i];
        m->m[i][1] = y[i];
        m->m[i][2] = z[i];
    }
}

/* roll on the floor inside the walls, bounce off the walls and each other */
static void balls_roll(float dk)
{
    for (int k = 0; k < 5; k++) {
        ball_t *b = &balls[k];
        b->p[0] += b->v[0] * dk;
        b->p[2] += b->v[2] * dk;
        if ((b->p[0] < -240 && b->v[0] < 0) || (b->p[0] > 850 && b->v[0] > 0))
            b->v[0] = -b->v[0];
        if ((b->p[2] < -450 && b->v[2] < 0) || (b->p[2] > 650 && b->v[2] > 0))
            b->v[2] = -b->v[2];
    }
    for (int i = 0; i < 5; i++)
        for (int j = i + 1; j < 5; j++) {
            ball_t *a = &balls[i], *b = &balls[j];
            float dx = b->p[0] - a->p[0], dz = b->p[2] - a->p[2], d2 = dx * dx + dz * dz;
            if (d2 >= 60.f * 60.f || d2 < 1e-6f)
                continue;
            float d = sqrtf(d2), nx = dx / d, nz = dz / d;
            float rel = (b->v[0] - a->v[0]) * nx + (b->v[2] - a->v[2]) * nz;
            if (rel < 0.f) {      /* closing: swap the velocity along the normal */
                a->v[0] += rel * nx; a->v[2] += rel * nz;
                b->v[0] -= rel * nx; b->v[2] -= rel * nz;
            }
            float push = (60.f - d) * 0.5f;
            a->p[0] -= nx * push; a->p[2] -= nz * push;
            b->p[0] += nx * push; b->p[2] += nz * push;
        }
    for (int k = 0; k < 5; k++) {
        ball_t *b = &balls[k];
        float sp = sqrtf(b->v[0] * b->v[0] + b->v[2] * b->v[2]) * dk;
        if (sp > 0.f) {
            /* rolling: axis up x v, angle = distance / radius */
            e3d_mat r;
            const float o[3] = { 0, 0, 0 };
            e3d_rotate_about(&r, o, b->v[2], 0, -b->v[0], -sp / 30.f * 57.29578f);
            e3d_mul(&r, &b->rot);
            b->rot = r;
            if (++b->renorm >= 32) {
                orthonormalise(&b->rot);
                b->renorm = 0;
            }
        }
        b->squash -= dk / 4.f;
        ball_matrix(b);
    }
}

static void moto_rise(float f)
{
    if (f > 120.f)
        f = 120.f;
    e3d_mat t;
    e3d_move(&t, 0, 5.f * f, 0);
    e3d_mul(&t, &moto[0].m0);
    moto[0].o.m = t;
    spin_move(&moto[1], moto[1].c0, f, 8, 1.5f, 0, 0, 0, 1, 12);
}

static void marv_assemble(float f)
{
    static const float axis[10][4] = {
        { 0, 0, 1, 20 },  { 0, 1, 0, 20 }, { 1, 0, 0, -20 }, { 1, 1, 0, 20 },  { 1, 0, 1, -20 },
        { 0, 1, 1, 20 },  { 1, 1, 1, -20 }, { 0, 0, 1, 20 }, { 0, 1, 0, -20 }, { 1, 0, 0, 20 },
    };
    if (f > 180.f)
        f = 180.f;
    /* each piece travels -off/180 a frame from its start: home after 180 */
    static const float off[10][3] = {
        { 720, 720, 720 }, { 720, -720, -720 }, { 0, 0, 720 }, { 0, -720, -720 }, { 0, -720, 720 },
        { 0, 720, -720 }, { 0, 720, 720 }, { 720, -720, 720 }, { 720, 720, -720 }, { -720, 0, 0 },
    };
    for (int k = 0; k < 10; k++)
        spin_move(&marv[k], marv[k].c0, f, -off[k][0] / 180.f, -off[k][1] / 180.f, -off[k][2] / 180.f,
                  axis[k][0], axis[k][1], axis[k][2], axis[k][3]);
}

static void main_setup(void)
{
    /* the assembled logo and the logos' end poses are where the main part starts */
    marv_assemble(180.f);
    float s[3] = { 0, 0, 0 };
    for (int k = 0; k < 10; k++) {
        float p[3];
        e3d_mat t;
        /* exact home pose: drop the 3600 degrees of float turn */
        e3d_object_pos(&marv[k].o, p);
        e3d_move(&t, p[0] - marv[k].c0[0], p[1] - marv[k].c0[1], p[2] - marv[k].c0[2]);
        e3d_mul(&t, &marv[k].m0);
        marv_final[k] = t;
        for (int i = 0; i < 3; i++)
            s[i] += p[i] / 10.f;
    }
    memcpy(marv_center, s, sizeof(s));
    moto_rise(120.f);
    for (int k = 0; k < 2; k++) {
        /* the ring is back to its start orientation after 4 turns */
        float p[3];
        e3d_object_pos(&moto[k].o, p);
        e3d_mat t;
        e3d_move(&t, p[0] - moto[k].c0[0], p[1] - moto[k].c0[1], p[2] - moto[k].c0[2]);
        e3d_mul(&t, &moto[k].m0);
        moto_final[k] = t;
        if (k == 0)
            memcpy(moto_p0, p, sizeof(p));
        else
            memcpy(moto_c1, p, sizeof(p));
    }
    memcpy(g_cam_base, e3d_cam(g_e)->pos, sizeof(g_cam_base));
}

static void main_update(float t, float dk)
{
    /* logo turns about its centre, 3 degrees a frame */
    for (int k = 0; k < 10; k++) {
        e3d_mat r;
        e3d_rotate_about(&r, marv_center, 0, 1, 0, 3.f * t);
        e3d_mul(&r, &marv_final[k]);
        marv[k].o.m = r;
    }
    /* the M circles the logo about a level axis, its ring spinning */
    float p[3];
    memcpy(p, moto_p0, sizeof(p));
    e3d_mat orbit, move, spin;
    e3d_rotate_about(&orbit, marv_center, 4, 0, 8, 2.f * t);
    e3d_apply(&orbit, p);
    e3d_move(&move, p[0] - moto_p0[0], p[1] - moto_p0[1], p[2] - moto_p0[2]);
    e3d_mat m0 = move;
    e3d_mul(&m0, &moto_final[0]);
    moto[0].o.m = m0;
    e3d_rotate_about(&spin, moto_c1, 0, 1, 0, -15.f * t);
    e3d_mul(&spin, &moto_final[1]);
    e3d_mat m1 = move;
    e3d_mul(&m1, &spin);
    moto[1].o.m = m1;
    /* ring of text: comes down to 420, turns a degree a frame */
    float drop = 10.f * t > 580.f ? 580.f : 10.f * t;
    const float axis_at[3] = { 800, 420, 200 };
    for (int k = 0; k < n_ring; k++) {
        e3d_mat r, d;
        e3d_rotate_about(&r, axis_at, 0, 1, 0, t);
        e3d_move(&d, 0, -drop, 0);
        e3d_mul(&r, &d);
        e3d_mul(&r, &ring[k].m0);
        ring[k].o.m = r;
    }
    balls_roll(dk);
}

static void draw_floor_scene(bool logos, bool text)
{
    draw(&floor_o);
    if (logos) {
        for (int k = 0; k < 10; k++)
            draw(&marv[k].o);
        for (int k = 0; k < 2; k++)
            draw(&moto[k].o);
    }
    for (int k = 0; k < 5; k++)
        draw(&balls[k].o);
    if (text)
        for (int k = 0; k < n_ring; k++)
            draw(&ring[k].o);
}

static void next_phase(void)
{
    if (g_phase == PH_APPROACH) {
        /* where the approach ends, in case it was skipped */
        e3d_camera *cam = e3d_cam(g_e);
        cam->pos[0] = -700;
        cam->pos[1] = 270;
        cam->tilt = 0;
    }
    if (g_phase == PH_BALLS)
        for (int k = 0; k < 5; k++) {
            balls[k].p[1] = 30;
            balls[k].v[1] = 0;
            balls[k].rest = true;
        }
    if (g_phase == PH_MOTO)
        moto_rise(120.f);
    if (g_phase == PH_MARVELL)
        main_setup();
    g_phase++;
    g_pf = 0;
    if (g_phase == PH_APPROACH)   /* the light moves over the floor */
        *e3d_light_get(g_e) = (e3d_light){ .pos = { -850, 750, 400 }, .r = 240, .g = 240, .b = 200 };
    if (g_phase >= PH_N)
        scene_reset();
}

static void update(float dk)
{
    g_pf += dk;
    float f = g_pf;
    e3d_camera *cam = e3d_cam(g_e);
    switch (g_phase) {
    case PH_ARE:
    case PH_YOU:
    case PH_READY: {
        int g = g_phase - PH_ARE;
        const float *c = ready[ready_mid[g]].c0;
        for (int k = ready_first[g]; k < ready_first[g + 1]; k++) {
            spin_move(&ready[k], c, f, 20, 0, 0, 1, 0, 0, -10);
            draw(&ready[k].o);
        }
        float p[3];
        e3d_object_pos(&ready[ready_first[g]].o, p);
        if (p[0] >= ready_end[g])
            next_phase();
        break;
    }
    case PH_COUNT:
        for (int k = 9; k >= 0; k--) {
            if (f < count_start[k])
                continue;
            spin_move(&count[k], count[k].c0, f - count_start[k], 20, 0, 0, 0, 1, 0, 10);
            draw(&count[k].o);
        }
        if (f >= count_end)
            next_phase();
        break;
    case PH_APPROACH: {
        float fm = f < 215.f ? f : 215.f;
        cam->pos[0] = -5000.f + 20.f * fm;
        cam->pos[1] = 700.f - 2.f * fm;
        cam->tilt = f < 240.f ? fmodf(6.f * f, 360.f) : 0.f;
        draw(&floor_o);
        e3d_draw_light(g_e);
        if (f >= 240.f)
            next_phase();
        break;
    }
    case PH_BALLS: {
        bool done = balls_fall(dk);
        draw_floor_scene(false, false);
        if (done && f > 30.f)
            next_phase();
        break;
    }
    case PH_MOTO:
        balls_fall(dk);
        moto_rise(f);
        draw(&floor_o);
        for (int k = 0; k < 2; k++)
            draw(&moto[k].o);
        for (int k = 0; k < 5; k++)
            draw(&balls[k].o);
        if (f >= 120.f)
            next_phase();
        break;
    case PH_MARVELL:
        balls_fall(dk);
        marv_assemble(f);
        draw_floor_scene(true, false);
        if (f >= 180.f)
            next_phase();
        break;
    case PH_MAIN:
    case PH_AWAY:
        g_main_t += dk;
        main_update(g_main_t, dk);
        if (g_phase == PH_MAIN) {
            for (int i = 0; i < 3; i++)
                cam->pos[i] = g_cam_base[i] + g_user_off[i];
            cam->heading = g_user_heading;
            cam->pitch = -5.f + g_user_pitch;
            if (f >= 1500.f) {
                memcpy(g_cam_base, cam->pos, sizeof(g_cam_base));
                next_phase();
            }
        } else {
            /* the original's exit: back off ever faster, rolling */
            cam->pos[0] = g_cam_base[0] - (10.f * f + 1.5f * f * f);
            cam->tilt = fmodf(f * f / 20.f, 360.f);
            if (f >= 120.f)
                next_phase();
        }
        draw_floor_scene(true, true);
        break;
    }
}

/* D-pad / shoulder camera control for the main part */
static void user_camera(uint32_t held, float dk)
{
    if (g_phase != PH_MAIN)
        return;
    if (held & PAPP_BTN_LEFT) g_user_heading -= 2.f * dk;
    if (held & PAPP_BTN_RIGHT) g_user_heading += 2.f * dk;
    if (held & PAPP_BTN_L1) g_user_pitch -= 1.f * dk;
    if (held & PAPP_BTN_R1) g_user_pitch += 1.f * dk;
    if (g_user_pitch > 60.f) g_user_pitch = 60.f;
    if (g_user_pitch < -60.f) g_user_pitch = -60.f;
    float step = ((held & PAPP_BTN_UP) ? 15.f : 0.f) - ((held & PAPP_BTN_DOWN) ? 15.f : 0.f);
    if (step != 0.f) {
        float h = g_user_heading * 0.0174533f, p = (-5.f + g_user_pitch) * 0.0174533f;
        g_user_off[0] += cosf(p) * cosf(h) * step * dk;
        g_user_off[1] += sinf(p) * step * dk;
        g_user_off[2] += cosf(p) * sinf(h) * step * dk;
    }
}

static void set_style(int s)
{
    static const char *const names[4] = { "SHADED", "FLAT", "WIREFRAME", "FOG" };
    g_style = s & 3;
    g_style_flags = g_style == 1 ? E3D_FLAT : g_style == 2 ? E3D_WIRE : 0;
    e3d_params_get(g_e)->fog_density = g_style == 3 ? 0.0005f : 0.f;
    papp_log("e3d: style %s\n", names[g_style]);
}

/* ── Main ────────────────────────────────────────────────────────────────── */

static bool load_assets(void)
{
    n_letters = e3d_load_text(asset_letters3d, &m_letters);
    n_numbers = e3d_load_text(asset_numbers3d, &m_numbers);
    n_marvell = e3d_load_text(asset_marvell3d, &m_marvell);
    n_moto = e3d_load_text(asset_motorola3d, &m_moto);
    papp_log("e3d: loaded letters %d, numbers %d, logo parts %d + %d\n", n_letters, n_numbers, n_marvell, n_moto);
    if (n_letters != 26 || n_numbers != 10 || n_marvell != 3 || n_moto != 2)
        return false;
    static const uint32_t ball_col[5][2] = { { 0xE02020, 0xF0F0F0 }, { 0x20B040, 0xF0E040 }, { 0x2050E0, 0xF0F0F0 },
                                             { 0xF0C020, 0xE03080 }, { 0x20C0D0, 0xF08020 } };
    for (int k = 0; k < 5; k++)
        if (!(m_ball[k] = make_ball(16, 10, ball_col[k][0], ball_col[k][1])))
            return false;
    /* the original floor: x -250..880, z -470..650 */
    m_floor = e3d_mesh_floor(-250, -470, 880, 650, 0, 24, 0xC8BEA0, 0x5A4A46);
    if (!m_floor)
        return false;
    e3d_object_init(&floor_o, m_floor);
    floor_o.flags = E3D_FLOOR;
    int open = 0;
    for (int i = 0; i < n_letters; i++)
        open += m_letters[i].open;
    papp_log("e3d: %d of 26 letters are open meshes (holes or shared edges)\n", open);
    return true;
}

int main(int argc, char **argv)
{
    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 1 };
    papp_surface_t *surf = papp_disp_surface_create(&cfg);
    if (!surf) {
        papp_log("e3d: no display surface\n");
        return 1;
    }
    g_surf = surf;
    uint16_t *fb[2];
    for (int i = 0; i < 2; i++)
        fb[i] = papp_alloc(sizeof(uint16_t) * W * H, PAPP_MEM_LARGE | PAPP_MEM_DMA, 64);
    g_pres_go = papp_sem_create(0, 1);
    g_pres_done = papp_sem_create(1, 1);
    papp_thread_cfg_t pcfg = { .name = "e3d_present", .stack_size = 4096, .prio = PAPP_PRIO_HIGH, .core = 0 };
    papp_thread_t *pres = (fb[0] && fb[1] && g_pres_go && g_pres_done)
                              ? papp_thread_create(presenter_main, NULL, &pcfg) : NULL;
    if (!pres) {
        papp_log("e3d: no frame buffers or presenter thread\n");
        return 1;
    }
    int cur = 0;
    g_e = e3d_create(W, H, 2);
    if (!g_e || !load_assets()) {
        papp_log("e3d: out of memory or bad assets\n");
        return 1;
    }
    make_sky();
    e3d_set_background_rows(g_e, sky_row, NULL);
    scene_reset();
    if (argc > 1 && strcmp(argv[1], "main") == 0)   /* start at the main scene */
        while (g_phase != PH_MAIN)
            next_phase();
    if (argc > 2)                                    /* and a style: 0..3, as Y steps them */
        set_style(atoi(argv[2]));
    papp_log("e3d: %s, %dx%d\n", "running", W, H);

    int64_t last = papp_time_us(), stat_t = last, log_t = last;
    int frames = 0;
    float fps = 0;
    uint32_t prev = 0;
    char hud[64];
    while (!papp_exit_requested()) {
        papp_gamepad_t pad;
        uint32_t held = papp_input_gamepad(0, &pad) ? pad.buttons : 0;
        uint32_t press = held & ~prev;
        prev = held;
        if (press & PAPP_BTN_B)
            break;
        if (press & PAPP_BTN_A)
            next_phase();
        if (press & PAPP_BTN_START)
            scene_reset();
        if (press & PAPP_BTN_X)
            g_hud = !g_hud;
        if (press & PAPP_BTN_Y)
            set_style(g_style + 1);

        int64_t now = papp_time_us();
        float dk = (float)(now - last) * 1e-6f * FPS0;
        last = now;
        if (dk > 3.f)
            dk = 3.f;
        user_camera(held, dk);

        int stride = W;
        uint16_t *px = fb[cur];
        e3d_begin(g_e, px, stride);
        update(dk);
        e3d_end(g_e);

        frames++;
        if (now - stat_t >= 500000) {
            fps = (float)frames * 1e6f / (float)(now - stat_t);
            frames = 0;
            stat_t = now;
        }
        const e3d_stats *st = e3d_get_stats(g_e);
        if (g_hud) {
            snprintf(hud, sizeof(hud), "%.1f FPS  %d TRIS", (double)fps, st->tris_drawn + st->shadow_tris);
            e3d_text(px, stride, W, H, 4, 4, hud, 0xFFE0, 1);
            snprintf(hud, sizeof(hud), "GEOM %.1f RAST %.1f MS", st->t_geom_us / 1000.0, st->t_raster_us / 1000.0);
            e3d_text(px, stride, W, H, 4, 14, hud, 0x07FF, 1);
            e3d_text(px, stride, W, H, 4, H - 11, phase_name[g_phase], 0xFFFF, 1);
        }
        papp_sem_take(g_pres_done, PAPP_WAIT_FOREVER);   /* the other buffer is on screen */
        g_pres_buf = px;
        papp_sem_give(g_pres_go);
        cur ^= 1;
        if (now - log_t >= 5000000) {
            papp_log("e3d: %.1f fps, %s, objects %d, tris %d + %d shadow, geom %d us, raster %d us\n",
                     (double)fps, phase_name[g_phase], st->objects, st->tris_drawn, st->shadow_tris,
                     st->t_geom_us, st->t_raster_us);
            log_t = now;
        }
    }

    papp_sem_take(g_pres_done, 1000);
    g_pres_buf = NULL;
    papp_sem_give(g_pres_go);
    papp_thread_join(pres, PAPP_WAIT_FOREVER, NULL);
    papp_sem_destroy(g_pres_go);
    papp_sem_destroy(g_pres_done);
    papp_free(fb[0]);
    papp_free(fb[1]);
    e3d_destroy(g_e);
    e3d_meshes_free(m_letters, n_letters);
    e3d_meshes_free(m_numbers, n_numbers);
    e3d_meshes_free(m_marvell, n_marvell);
    e3d_meshes_free(m_moto, n_moto);
    for (int k = 0; k < 5; k++)
        e3d_mesh_free(m_ball[k]);
    e3d_mesh_free(m_floor);
    papp_disp_surface_destroy(surf);
    papp_log("e3d: bye\n");
    return 0;
}
