/*
 * Dustline — a rally stage against the clock, on the e3d engine.
 *
 * One car on a closed course through the woods: tarmac, then dirt, then
 * loose gravel, three laps against the clock. A chase camera follows the car;
 * the car runs on a small tyre model (slip angles, a friction circle per axle,
 * four-wheel drive), so it grips on tarmac, slides on the loose stuff and can be
 * thrown into a corner with the handbrake.
 *
 * The road is the floor layer (the car's shadow falls on it); the grass, the
 * sky and the hills on the horizon are drawn by the background row function.
 * The course is cut into chunks of 32 m, each a road mesh and a scenery mesh in
 * two levels of detail.
 *
 * Records: best lap and best race in /sd/data/dustline/records.txt.
 * Save: the pause menu's "Save race" keeps the race (lap, times, the car) in
 * /sd/data/dustline/save.txt; "Continue" on the title brings it back, after a
 * countdown.
 *
 * Controls: the board's roller (paddle) steers, absolute: its middle is
 * straight ahead, its ends full lock. Pad LEFT/RIGHT or the stick steer too
 * (while pressed they win over the roller). Gas = the slider on the left of the
 * screen (a lever: it stays where the finger leaves it), R2 (analog) or UP;
 * A or B = brake (takes the gas off while held), DOWN = reverse, X = handbrake,
 * Y = back on the road, START or SELECT = pause. Touch: arrows and BRAKE at the
 * bottom right, REV above BRAKE, the top-right corner pauses.
 *
 * Arguments: `auto` = the computer drives (for testing; no records, `rec` = they count),
 *            `icon` = a still close-up for the icon, `laps N`, `fps` = show the frame rate.
 */
#include "papp/papp.h"
#include "e3d.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

PAPP_MANIFEST(.id = "giltal.dustline", .name = "Dustline", .version = PAPP_APP_VERSION, .main_stack = 16384);

#define W 400
#define H 240
#define DEG (180.f / (float)M_PI)
#define RAD ((float)M_PI / 180.f)

static float frand(void) { return (float)rand() / (float)RAND_MAX; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }

static uint32_t mix_rgb(uint32_t a, uint32_t b, float t)
{
    t = clampf(t, 0, 1);
    int r = (int)((float)(a >> 16) + ((float)(b >> 16) - (float)(a >> 16)) * t);
    int g = (int)((float)((a >> 8) & 255) + ((float)((b >> 8) & 255) - (float)((a >> 8) & 255)) * t);
    int bl = (int)((float)(a & 255) + ((float)(b & 255) - (float)(a & 255)) * t);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}
static uint32_t shade_rgb(uint32_t c, float k)
{
    int r = (int)fminf(255.f, (float)(c >> 16) * k), g = (int)fminf(255.f, (float)((c >> 8) & 255) * k),
        b = (int)fminf(255.f, (float)(c & 255) * k);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)b;
}
static uint16_t rgb565(uint32_t c) { return PAPP_RGB565((int)(c >> 16), (int)((c >> 8) & 255), (int)(c & 255)); }

/* ── Sound: an engine, tyre noise, gravel and a few one-shot effects ──────── */
#define SR 22050
enum { WAVE_SQUARE, WAVE_TRI, WAVE_NOISE };
typedef struct {
    int   left, delay, total;
    float ph, f, fmul, vol;
    int   wave;
} voice_t;
#define NVOICES 6
static voice_t g_voice[NVOICES];
static papp_mutex_t *g_snd_lock;
static papp_audio_stream_t *g_snd;
static volatile bool g_snd_quit;
static uint32_t g_noise = 0x1234567u;
/* continuous sounds, set by the game each frame */
static volatile float s_eng_f, s_eng_vol, s_skid_vol, s_grit_vol;

static void sfx_at(float f0, float f1, int ms, int wave, float vol, int delay_ms)
{
    if (!g_snd)
        return;
    papp_mutex_lock(g_snd_lock);
    int best = 0;
    for (int i = 0; i < NVOICES; i++) {
        if (g_voice[i].left == 0) {
            best = i;
            break;
        }
        if (g_voice[i].left < g_voice[best].left)
            best = i;
    }
    voice_t *v = &g_voice[best];
    int n = SR * ms / 1000;
    v->total = v->left = n > 0 ? n : 1;
    v->delay = SR * delay_ms / 1000;
    v->ph = 0;
    v->f = f0;
    v->fmul = powf(f1 / f0, 1.f / (float)v->total);
    v->vol = vol;
    v->wave = wave;
    papp_mutex_unlock(g_snd_lock);
}
static void sfx(float f0, float f1, int ms, int wave, float vol) { sfx_at(f0, f1, ms, wave, vol, 0); }

static int audio_main(void *arg)
{
    static int16_t buf[256];
    float eph = 0, eph2 = 0, ef = 40.f, ev = 0, sv = 0, gv = 0;
    float lp1 = 0, lp2 = 0, lp3 = 0;
    while (!g_snd_quit && !papp_exit_requested()) {
        papp_mutex_lock(g_snd_lock);
        float tf = s_eng_f, tv = s_eng_vol, ts = s_skid_vol, tg = s_grit_vol;
        for (int i = 0; i < 256; i++) {
            /* glide towards the targets (no clicks) */
            ef += (tf - ef) * 0.002f;
            ev += (tv - ev) * 0.002f;
            sv += (ts - sv) * 0.003f;
            gv += (tg - gv) * 0.002f;
            g_noise = g_noise * 1664525u + 1013904223u;
            float nz = (float)(int32_t)g_noise * (1.f / 2147483648.f);
            /* engine: two detuned pulses with a rasp of noise */
            eph += ef * (1.f / SR);
            if (eph >= 1.f) eph -= 1.f;
            eph2 += ef * 0.5f * (1.f / SR);
            if (eph2 >= 1.f) eph2 -= 1.f;
            float eng = (eph < 0.3f ? 1.f : -0.43f) * 0.6f + (eph2 < 0.5f ? 0.4f : -0.4f) + nz * 0.15f;
            lp1 += (eng - lp1) * 0.35f;
            float acc = lp1 * ev;
            /* tyres: a hiss (high-passed noise), gravel: a low rumble */
            lp2 += (nz - lp2) * 0.25f;
            acc += (nz - lp2) * sv;
            lp3 += (nz - lp3) * 0.05f;
            acc += lp3 * gv * 3.f;
            for (int k = 0; k < NVOICES; k++) {
                voice_t *v = &g_voice[k];
                if (!v->left)
                    continue;
                if (v->delay) {
                    v->delay--;
                    continue;
                }
                float env = (float)v->left / (float)v->total;
                env = env * (2.f - env);
                float s = v->wave == WAVE_SQUARE ? (v->ph < 0.5f ? 1.f : -1.f)
                        : v->wave == WAVE_TRI    ? (v->ph < 0.5f ? 4.f * v->ph - 1.f : 3.f - 4.f * v->ph)
                                                 : nz;
                acc += s * v->vol * env;
                v->ph += v->f * (1.f / SR);
                if (v->ph >= 1.f)
                    v->ph -= 1.f;
                v->f *= v->fmul;
                v->left--;
            }
            acc = clampf(acc, -1.f, 1.f);
            buf[i] = (int16_t)(acc * 24000.f);
        }
        papp_mutex_unlock(g_snd_lock);
        papp_audio_write(g_snd, buf, 256, 100);
    }
    return 0;
}

/* ── Mesh building ───────────────────────────────────────────────────────────
 * A builder collects vertices and triangles (each part with its own vertices),
 * then bakes them into an e3d mesh. Triangles are turned to face away from a
 * given inside point. */
#define BMAX_V 9000
#define BMAX_T 7000
typedef struct {
    int nv, nt;
    float (*pos)[3];
    struct { uint16_t v[3]; uint32_t rgb; } *tri;
} builder_t;
static builder_t B;

static bool b_init(void)
{
    B.pos = papp_alloc(sizeof(float) * 3 * BMAX_V, PAPP_MEM_LARGE, 4);
    B.tri = papp_alloc(sizeof(B.tri[0]) * BMAX_T, PAPP_MEM_LARGE, 4);
    return B.pos && B.tri;
}
static void b_free(void)
{
    papp_free(B.pos);
    papp_free(B.tri);
}
static void b_reset(void) { B.nv = B.nt = 0; }

static int b_v(float x, float y, float z)
{
    if (B.nv >= BMAX_V)
        return BMAX_V - 1;
    B.pos[B.nv][0] = x;
    B.pos[B.nv][1] = y;
    B.pos[B.nv][2] = z;
    return B.nv++;
}

/* outward from ctr: (v0 - v1) x (v2 - v1) points away from it */
static void b_tri(int a, int b, int c, uint32_t rgb, const float ctr[3])
{
    if (B.nt >= BMAX_T)
        return;
    float *p0 = B.pos[a], *p1 = B.pos[b], *p2 = B.pos[c];
    float u[3] = { p0[0] - p1[0], p0[1] - p1[1], p0[2] - p1[2] };
    float v[3] = { p2[0] - p1[0], p2[1] - p1[1], p2[2] - p1[2] };
    float n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
    float d[3] = { (p0[0] + p1[0] + p2[0]) / 3 - ctr[0], (p0[1] + p1[1] + p2[1]) / 3 - ctr[1],
                   (p0[2] + p1[2] + p2[2]) / 3 - ctr[2] };
    if (n[0] * d[0] + n[1] * d[1] + n[2] * d[2] < 0) {
        int s = a;
        a = c;
        c = s;
    }
    B.tri[B.nt].v[0] = (uint16_t)a;
    B.tri[B.nt].v[1] = (uint16_t)b;
    B.tri[B.nt].v[2] = (uint16_t)c;
    B.tri[B.nt].rgb = rgb;
    B.nt++;
}
static void b_quad(int a, int b, int c, int d, uint32_t rgb, const float ctr[3])
{
    b_tri(a, b, c, rgb, ctr);
    b_tri(a, c, d, rgb, ctr);
}
static e3d_mesh *b_bake(float crease)
{
    if (B.nt == 0)
        return NULL;
    e3d_mesh *m = e3d_mesh_create(B.nv, B.nt);
    if (!m)
        return NULL;
    memcpy(m->pos, B.pos, sizeof(float) * 3 * (size_t)B.nv);
    for (int i = 0; i < B.nt; i++) {
        e3d_tri *t = &m->tris[i];
        t->v[0] = B.tri[i].v[0];
        t->v[1] = B.tri[i].v[1];
        t->v[2] = B.tri[i].v[2];
        t->r = (uint8_t)(B.tri[i].rgb >> 16);
        t->g = (uint8_t)(B.tri[i].rgb >> 8);
        t->b = (uint8_t)B.tri[i].rgb;
    }
    e3d_mesh_finish(m, crease);
    return m;
}

/* a box (x0..x1, y0..y1, z0..z1) */
static void b_box(float x0, float y0, float z0, float x1, float y1, float z1, uint32_t rgb)
{
    int v[8];
    for (int i = 0; i < 8; i++)
        v[i] = b_v((i & 1) ? x1 : x0, (i & 2) ? y1 : y0, (i & 4) ? z1 : z0);
    float c[3] = { (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f };
    b_quad(v[0], v[1], v[3], v[2], rgb, c);
    b_quad(v[4], v[5], v[7], v[6], rgb, c);
    b_quad(v[0], v[1], v[5], v[4], rgb, c);
    b_quad(v[2], v[3], v[7], v[6], shade_rgb(rgb, 1.05f), c);
    b_quad(v[0], v[2], v[6], v[4], rgb, c);
    b_quad(v[1], v[3], v[7], v[5], rgb, c);
}

/* a cone standing at (x, y, z): base radius r, height h, seg sides, no base */
static void b_cone(float x, float y, float z, float r, float h, int seg, uint32_t rgb, float rot)
{
    int base = B.nv;
    for (int i = 0; i < seg; i++) {
        float a = rot + 6.2832f * (float)i / (float)seg;
        b_v(x + cosf(a) * r, y, z + sinf(a) * r);
    }
    int tip = b_v(x, y + h, z);
    float c[3] = { x, y + h * 0.3f, z };
    for (int i = 0; i < seg; i++)
        b_tri(base + i, base + (i + 1) % seg, tip, shade_rgb(rgb, 0.9f + 0.2f * (float)(i & 1)), c);
}

/* a prism: a convex side profile (x, y) pairs, extruded across z0..z1. side_rgb per profile edge. */
static void b_prism(const float (*p)[2], int n, float z0, float z1, const uint32_t *side_rgb, uint32_t cap_rgb)
{
    int base = B.nv;
    float cx = 0, cy = 0;
    for (int i = 0; i < n; i++) {
        b_v(p[i][0], p[i][1], z0);
        b_v(p[i][0], p[i][1], z1);
        cx += p[i][0];
        cy += p[i][1];
    }
    float c[3] = { cx / (float)n, cy / (float)n, (z0 + z1) * 0.5f };
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        b_quad(base + 2 * i, base + 2 * i + 1, base + 2 * j + 1, base + 2 * j, side_rgb[i], c);
    }
    for (int i = 1; i + 1 < n; i++) {
        b_tri(base, base + 2 * i, base + 2 * (i + 1), cap_rgb, c);
        b_tri(base + 1, base + 2 * i + 1, base + 2 * (i + 1) + 1, cap_rgb, c);
    }
}

static e3d_mat mat_at(float x, float y, float z, float rx, float ry, float rz, float s)
{
    e3d_mat m, r;
    e3d_move(&m, x, y, z);
    e3d_rotate(&r, rx, ry, rz);
    e3d_mul(&m, &r);
    if (s != 1.f) {
        e3d_scale(&r, s, s, s);
        e3d_mul(&m, &r);
    }
    return m;
}

static void draw_obj(e3d_t *e, const e3d_mesh *mesh, const e3d_mat *m, int32_t rgb, uint32_t flags)
{
    if (!mesh)
        return;
    e3d_object o;
    e3d_object_init(&o, mesh);
    o.m = *m;
    o.color = rgb;
    o.flags = flags;
    e3d_draw(e, &o);
}

/* ── The course ──────────────────────────────────────────────────────────────
 * Control points of a closed loop (metres; x, z), a surface for the stretch
 * that starts at each. Sampled with a Catmull-Rom spline, then every STEP m. */
enum { SURF_TARMAC, SURF_DIRT, SURF_GRAVEL, SURF_VERGE, SURF_GRASS, SURF_SNOW, SURF_ICE, SURF_SAND, SURF_HARD };
#define NSURF 9
static const char *const surf_name[] = { "TARMAC", "DIRT", "GRAVEL", "VERGE", "GRASS", "SNOW", "ICE", "SAND", "HARDPAN" };
/* arcade grip: the loose surfaces still turn at speed. GRASS is off the road (its name and colour
   come from the stage: deep snow, soft sand) */
static const float surf_mu[]   = { 1.15f, 1.10f, 1.05f, 0.62f, 0.50f, 1.00f, 0.86f, 1.00f, 1.10f };
static const float surf_drag[] = { 0.f, 30.f, 50.f, 220.f, 420.f, 45.f, 10.f, 70.f, 15.f };   /* rolling resistance, N per m/s */

typedef struct { float x, z; uint8_t surf; } ctrl_t;
static const ctrl_t k_ctrl[] = {
    { 0, 0, SURF_TARMAC },        /* start / finish, heading +x */
    { 120, 0, SURF_TARMAC },
    { 250, 4, SURF_TARMAC },
    { 350, 40, SURF_TARMAC },     /* fast right */
    { 410, 120, SURF_TARMAC },
    { 420, 210, SURF_DIRT },
    { 395, 280, SURF_DIRT },
    { 345, 300, SURF_DIRT },      /* hairpin */
    { 318, 262, SURF_DIRT },
    { 330, 210, SURF_DIRT },
    { 300, 160, SURF_DIRT },      /* esses */
    { 240, 175, SURF_DIRT },
    { 190, 140, SURF_DIRT },
    { 130, 165, SURF_DIRT },
    { 80, 230, SURF_GRAVEL },
    { 10, 270, SURF_GRAVEL },     /* long left */
    { -80, 262, SURF_GRAVEL },
    { -150, 215, SURF_GRAVEL },
    { -185, 140, SURF_GRAVEL },
    { -160, 80, SURF_GRAVEL },
    { -205, 30, SURF_TARMAC },    /* chicane */
    { -175, -30, SURF_TARMAC },
    { -110, -45, SURF_TARMAC },
    { -50, -18, SURF_TARMAC },
};

/* Frost Pass: snow, sweepers and an S, an icy stretch along a frozen lake */
static const ctrl_t k_ctrl_snow[] = {
    { 0, 0, SURF_SNOW }, { 110, 0, SURF_SNOW }, { 210, -25, SURF_SNOW }, { 290, -10, SURF_SNOW },
    { 330, 40, SURF_SNOW }, { 310, 100, SURF_ICE }, { 250, 120, SURF_ICE }, { 200, 170, SURF_ICE },
    { 220, 240, SURF_SNOW }, { 290, 270, SURF_SNOW }, { 350, 320, SURF_SNOW }, { 320, 390, SURF_SNOW },
    { 240, 400, SURF_SNOW }, { 170, 360, SURF_SNOW }, { 100, 380, SURF_SNOW }, { 30, 420, SURF_SNOW },
    { -60, 400, SURF_SNOW }, { -110, 330, SURF_SNOW }, { -90, 260, SURF_SNOW }, { -140, 200, SURF_SNOW },
    { -200, 160, SURF_SNOW }, { -190, 90, SURF_SNOW }, { -130, 60, SURF_SNOW }, { -70, 20, SURF_SNOW },
};
/* Sun Mesa: a tarmac straight, a fast dry lake bed, then a twisty sandy section and a hairpin */
static const ctrl_t k_ctrl_desert[] = {
    { 0, 0, SURF_TARMAC }, { 150, 0, SURF_TARMAC }, { 300, 10, SURF_HARD }, { 420, 60, SURF_HARD },
    { 470, 150, SURF_HARD }, { 430, 230, SURF_SAND }, { 350, 240, SURF_SAND }, { 300, 200, SURF_SAND },
    { 240, 230, SURF_SAND }, { 220, 300, SURF_SAND }, { 150, 340, SURF_SAND }, { 60, 320, SURF_SAND },
    { 20, 260, SURF_SAND }, { -50, 250, SURF_SAND }, { -130, 290, SURF_SAND }, { -210, 260, SURF_SAND },
    { -240, 180, SURF_SAND }, { -200, 110, SURF_TARMAC }, { -230, 50, SURF_TARMAC }, { -180, -10, SURF_TARMAC },
    { -90, -20, SURF_TARMAC },
};
#define NC(a) ((int)(sizeof(a) / sizeof(a[0])))
#define NCTRL_MAX 24

/* ── Stages ──────────────────────────────────────────────────────────────────
 * The course, its look and its plants. Colours in pairs are (at the horizon, higher up) for the
 * hills and (at the horizon, near the camera) for the ground. */
enum { HILLS_ROLLING, HILLS_MOUNTAINS, HILLS_MESAS };
enum { FLORA_FOREST, FLORA_SNOW, FLORA_DESERT };
typedef struct {
    const char *name, *file, *offroad;      /* file: "" = the first stage's file names */
    const ctrl_t *ctrl;
    int nctrl;
    float lake_x, lake_z, lake_rx, lake_rz;
    uint32_t water[2], shore;
    uint32_t sky[2], ground[2], far[2], near[2];
    uint32_t ev_ground[2], ev_far[2], ev_near[2];
    uint32_t tarmac_verge, offroad_dust;
    uint8_t hills, flora;
    float trees, rocks;                     /* tree density, the share of rocks */
} stage_t;
static const stage_t k_stage[] = {
    { "PINE VALLEY", "", "GRASS", k_ctrl, NC(k_ctrl), 280.f, 80.f, 45.f, 26.f, { 0x3C6C9C, 0x4474A4 }, 0x8C7E5A,
      { 0x4C86D0, 0xCFE2EE }, { 0x8EA068, 0x4E7A2C }, { 0x8AA0B8, 0x6E86A2 }, { 0x3E5E3A, 0x2C4A2A },
      { 0x7E8A4C, 0x40622A }, { 0x9A88A8, 0x7C6C90 }, { 0x4C5A3A, 0x3A4A30 },
      0x7C8450, 0x7A8A50, HILLS_ROLLING, FLORA_FOREST, 1.f, 0.12f },
    { "FROST PASS", "_snow", "DEEP SNOW", k_ctrl_snow, NC(k_ctrl_snow), 130.f, 150.f, 40.f, 26.f,
      { 0xB4D0E6, 0xC0DAEE }, 0xDCE4EC,
      { 0x5A8CD0, 0xDCE8F2 }, { 0xE0E8F0, 0xF2F5F9 }, { 0x8C9CB4, 0xF2F6FA }, { 0x34503E, 0x2A4434 },
      { 0xE6C8B4, 0xCCBCC4 }, { 0xB898B0, 0xF0D8D0 }, { 0x3A4A44, 0x2E3E38 },
      0xEEF2F6, 0xF0F4FA, HILLS_MOUNTAINS, FLORA_SNOW, 0.9f, 0.15f },
    { "SUN MESA", "_desert", "SOFT SAND", k_ctrl_desert, NC(k_ctrl_desert), 90.f, 62.f, 32.f, 20.f,
      { 0x3A7898, 0x4282A0 }, 0xB89C68,
      { 0x3E7CD0, 0xE6DCC8 }, { 0xD8BC88, 0xCFAE76 }, { 0xC08060, 0xB06E52 }, { 0xDCB880, 0xD2AC74 },
      { 0xD8A070, 0xB88A58 }, { 0xB86850, 0xA05A48 }, { 0xD89C68, 0xC88C5C },
      0xD8BC88, 0xD8B880, HILLS_MESAS, FLORA_DESERT, 0.45f, 0.35f },
};
#define NSTAGES NC(k_stage)
static int g_stage;
static const stage_t *g_st = &k_stage[0];

#define STEP       2.f
#define MAX_SAMP   1800
#define CHUNK      16                    /* samples per chunk (32 m) */
#define MAX_CHUNKS (MAX_SAMP / CHUNK + 1)
#define VERGE_W    1.4f

typedef struct {
    float x, z;          /* centre */
    float tx, tz;        /* unit tangent */
    float hw;            /* half width of the road */
    float curv;          /* 1/radius, + = turning right */
    uint8_t surf;
} samp_t;

static samp_t g_tr[MAX_SAMP];
static int    g_ns;                       /* samples on the loop */
static float  g_track_len;
static float  g_bx0, g_bz0, g_bx1, g_bz1; /* bounds */

static void catmull(const ctrl_t *p0, const ctrl_t *p1, const ctrl_t *p2, const ctrl_t *p3, float t, float *x, float *z)
{
    float t2 = t * t, t3 = t2 * t;
    *x = 0.5f * (2 * p1->x + (-p0->x + p2->x) * t + (2 * p0->x - 5 * p1->x + 4 * p2->x - p3->x) * t2 +
                 (-p0->x + 3 * p1->x - 3 * p2->x + p3->x) * t3);
    *z = 0.5f * (2 * p1->z + (-p0->z + p2->z) * t + (2 * p0->z - 5 * p1->z + 4 * p2->z - p3->z) * t2 +
                 (-p0->z + 3 * p1->z - 3 * p2->z + p3->z) * t3);
}

static void build_track(void)
{
    /* a dense polyline first, then resampled by arc length */
    enum { SUB = 64 };
    static float px[NCTRL_MAX * SUB + 1], pz[NCTRL_MAX * SUB + 1];
    static uint8_t ps[NCTRL_MAX * SUB + 1];
    const ctrl_t *kc = g_st->ctrl;
    const int nc = g_st->nctrl < NCTRL_MAX ? g_st->nctrl : NCTRL_MAX;
    int n = 0;
    for (int i = 0; i < nc; i++) {
        const ctrl_t *p0 = &kc[(i + nc - 1) % nc], *p1 = &kc[i], *p2 = &kc[(i + 1) % nc], *p3 = &kc[(i + 2) % nc];
        for (int s = 0; s < SUB; s++) {
            catmull(p0, p1, p2, p3, (float)s / SUB, &px[n], &pz[n]);
            ps[n] = p1->surf;
            n++;
        }
    }
    px[n] = px[0];
    pz[n] = pz[0];
    ps[n] = ps[0];
    float total = 0;
    for (int i = 0; i < n; i++)
        total += hypotf(px[i + 1] - px[i], pz[i + 1] - pz[i]);
    g_ns = (int)(total / STEP);
    if (g_ns > MAX_SAMP)
        g_ns = MAX_SAMP;
    float step = total / (float)g_ns;
    g_track_len = total;
    int seg = 0;
    float seg_start = 0, seg_len = hypotf(px[1] - px[0], pz[1] - pz[0]);
    for (int k = 0; k < g_ns; k++) {
        float d = (float)k * step;
        while (d > seg_start + seg_len && seg < n - 1) {
            seg_start += seg_len;
            seg++;
            seg_len = hypotf(px[seg + 1] - px[seg], pz[seg + 1] - pz[seg]);
        }
        float t = seg_len > 0 ? (d - seg_start) / seg_len : 0;
        samp_t *s = &g_tr[k];
        s->x = px[seg] + (px[seg + 1] - px[seg]) * t;
        s->z = pz[seg] + (pz[seg + 1] - pz[seg]) * t;
        s->surf = ps[seg];
        s->hw = s->surf == SURF_TARMAC ? 4.8f : 4.3f;
    }
    g_bx0 = g_bz0 = 1e9f;
    g_bx1 = g_bz1 = -1e9f;
    for (int k = 0; k < g_ns; k++) {
        samp_t *s = &g_tr[k], *a = &g_tr[(k + g_ns - 1) % g_ns], *b = &g_tr[(k + 1) % g_ns];
        float tx = b->x - a->x, tz = b->z - a->z, l = hypotf(tx, tz) + 1e-6f;
        s->tx = tx / l;
        s->tz = tz / l;
        g_bx0 = fminf(g_bx0, s->x);
        g_bx1 = fmaxf(g_bx1, s->x);
        g_bz0 = fminf(g_bz0, s->z);
        g_bz1 = fmaxf(g_bz1, s->z);
    }
    for (int k = 0; k < g_ns; k++) {
        samp_t *a = &g_tr[(k + g_ns - 2) % g_ns], *b = &g_tr[(k + 2) % g_ns];
        float ha = atan2f(a->tz, a->tx), hb = atan2f(b->tz, b->tx);
        float dh = fmodf(hb - ha + 3.f * (float)M_PI, 2.f * (float)M_PI) - (float)M_PI;
        g_tr[k].curv = dh / (4.f * STEP);
    }
    /* soften the surface changes: a sample of another surface stays where it is */
    papp_log("dustline: course %.0f m, %d samples\n", (double)total, g_ns);
}

/* the right-hand normal of a sample's tangent (z is right of x) */
static void samp_right(const samp_t *s, float *rx, float *rz)
{
    *rx = -s->tz;
    *rz = s->tx;
}

/* nearest sample to (x, z), searched around hint (or everywhere if hint < 0) */
static int nearest_sample(float x, float z, int hint, int span)
{
    int best = 0;
    float bd = 1e18f;
    if (hint < 0) {
        for (int k = 0; k < g_ns; k++) {
            float d = (g_tr[k].x - x) * (g_tr[k].x - x) + (g_tr[k].z - z) * (g_tr[k].z - z);
            if (d < bd) {
                bd = d;
                best = k;
            }
        }
        return best;
    }
    for (int o = -span; o <= span; o++) {
        int k = (hint + o + g_ns) % g_ns;
        float d = (g_tr[k].x - x) * (g_tr[k].x - x) + (g_tr[k].z - z) * (g_tr[k].z - z);
        if (d < bd) {
            bd = d;
            best = k;
        }
    }
    return best;
}

/* the lateral offset of (x, z) from sample k's centre line (+ = right) */
static float lateral(int k, float x, float z)
{
    float rx, rz;
    samp_right(&g_tr[k], &rx, &rz);
    return (x - g_tr[k].x) * rx + (z - g_tr[k].z) * rz;
}

/* ── Scenery: trees, rocks, marker posts, the start gantry ───────────────── */
enum { OBJ_PINE, OBJ_BIRCH, OBJ_ROCK, OBJ_POST, OBJ_PILLAR, OBJ_HAY, OBJ_FAN, OBJ_BUSH };
typedef struct { float x, z, r, s, rot; uint8_t kind, chunk; } prop_t;
#define MAX_PROPS 2200
static prop_t g_prop[MAX_PROPS];
static int    g_nprops;

/* collision grid over the props */
#define GCELL 24.f
static int     g_gw, g_gh;
static float   g_gx0, g_gz0;
static int16_t *g_gstart;                 /* g_gw * g_gh + 1 */
static int16_t *g_gidx;

static uint32_t s_rng = 0xC0FFEEu;
static float rnd(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return (float)(s_rng >> 8) * (1.f / 16777216.f);
}

/* how far (x, z) is from the road of any sample, minus its half width (coarse) */
static float road_gap(float x, float z)
{
    float best = 1e9f;
    for (int k = 0; k < g_ns; k += 2) {
        float d = hypotf(g_tr[k].x - x, g_tr[k].z - z) - g_tr[k].hw;
        if (d < best)
            best = d;
    }
    return best;
}

static void add_prop(float x, float z, int kind, float s, float r)
{
    if (g_nprops >= MAX_PROPS)
        return;
    int k = nearest_sample(x, z, -1, 0);
    g_prop[g_nprops++] = (prop_t){ x, z, r, s, rnd() * 6.2832f, (uint8_t)kind, (uint8_t)(k / CHUNK) };
}

/* the lake inside the loop: an ellipse with a wavy shore (radius scale by angle) */
#define LAKE_X  (g_st->lake_x)
#define LAKE_Z  (g_st->lake_z)
#define LAKE_RX (g_st->lake_rx)
#define LAKE_RZ (g_st->lake_rz)
static float lake_shore(float a)
{
    return 1.f + 0.10f * sinf(a * 3.f + 0.7f) + 0.06f * sinf(a * 7.f + 2.1f);
}
/* < 1 inside the lake (its shore scaled by margin) */
static float lake_dist(float x, float z, float margin)
{
    float u = (x - LAKE_X) / LAKE_RX, v = (z - LAKE_Z) / LAKE_RZ;
    return sqrtf(u * u + v * v) / (lake_shore(atan2f(v, u)) * margin);
}
#define LAKE_SEG 40
static void lake_point(int i, float scale, float *x, float *z)
{
    float a = 6.2832f * (float)i / (float)LAKE_SEG, r = lake_shore(a) * scale;
    *x = LAKE_X + cosf(a) * LAKE_RX * r;
    *z = LAKE_Z + sinf(a) * LAKE_RZ * r;
}
/* the least gap between the shore and the road (logged: keep it well clear) */
static float lake_gap(void)
{
    float g = 1e9f, x, z;
    for (int i = 0; i < LAKE_SEG; i++) {
        lake_point(i, 1.08f, &x, &z);
        g = fminf(g, road_gap(x, z));
    }
    return g;
}

static int g_fan_groups;
static void place_props(void)
{
    g_nprops = 0;
    s_rng = 0xC0FFEEu;
    /* start gantry pillars */
    {
        float rx, rz;
        samp_right(&g_tr[0], &rx, &rz);
        float d = g_tr[0].hw + 1.2f;
        add_prop(g_tr[0].x + rx * d, g_tr[0].z + rz * d, OBJ_PILLAR, 1.f, 0.6f);
        add_prop(g_tr[0].x - rx * d, g_tr[0].z - rz * d, OBJ_PILLAR, 1.f, 0.6f);
    }
    for (int k = 0; k < g_ns; k++) {
        const samp_t *t = &g_tr[k];
        float rx, rz;
        samp_right(t, &rx, &rz);
        /* marker posts every 24 m on both sides; hay bales on the outside of sharp corners */
        if (k % 12 == 6)
            for (int side = -1; side <= 1; side += 2) {
                float d = (t->hw + VERGE_W + 0.6f) * (float)side;
                float x = t->x + rx * d, z = t->z + rz * d;
                if (road_gap(x, z) > VERGE_W + 0.3f)
                    add_prop(x, z, OBJ_POST, 1.f, 0.15f);
            }
        if (fabsf(t->curv) > 1.f / 45.f && k % 3 == 0) {
            float side = t->curv > 0 ? -1.f : 1.f;   /* outside of the turn */
            float d = (t->hw + VERGE_W + 1.8f) * side;
            float x = t->x + rx * d, z = t->z + rz * d;
            if (road_gap(x, z) > VERGE_W + 1.f)
                add_prop(x, z, OBJ_HAY, 1.f, 0.9f);
        }
        /* trees in clumps, more of them further from the road */
        if (k % 3 == 0)
            for (int side = -1; side <= 1; side += 2) {
                float dens = (t->surf == SURF_TARMAC ? 0.45f : 0.75f) * g_st->trees;
                if (rnd() > dens)
                    continue;
                float off = t->hw + VERGE_W + 3.f + rnd() * rnd() * 34.f;
                float x = t->x + rx * off * (float)side + (rnd() - 0.5f) * 4.f;
                float z = t->z + rz * off * (float)side + (rnd() - 0.5f) * 4.f;
                if (road_gap(x, z) < VERGE_W + 2.5f || lake_dist(x, z, 1.5f) < 1.f)
                    continue;
                float u = rnd();
                if (u < g_st->rocks)
                    add_prop(x, z, OBJ_ROCK, 0.8f + rnd() * 1.4f, 1.4f);
                else
                    add_prop(x, z, u < 0.32f ? OBJ_BIRCH : OBJ_PINE, 0.8f + rnd() * 0.6f, 0.7f);
            }
    }
    /* spectators: a group behind the outside of each tight corner, and at the start */
    int last = -1000;
    for (int k = 2; k < g_ns - 2; k++) {
        const samp_t *t = &g_tr[k];
        float ac = fabsf(t->curv);
        if (ac < 1.f / 32.f || ac < fabsf(g_tr[k - 2].curv) || ac < fabsf(g_tr[k + 2].curv) || k - last < 50)
            continue;
        last = k;
        float rx, rz;
        samp_right(t, &rx, &rz);
        float side = t->curv > 0 ? -1.f : 1.f;
        int n = 0;
        for (int i = 0; i < 9 && n < 6; i++) {
            float along = (rnd() - 0.5f) * 16.f, off = t->hw + VERGE_W + 4.5f + rnd() * 3.f;
            float x = t->x + rx * off * side + t->tx * along, z = t->z + rz * off * side + t->tz * along;
            if (road_gap(x, z) < VERGE_W + 3.5f)
                continue;
            add_prop(x, z, OBJ_FAN, 0.9f + rnd() * 0.2f, 0.4f);
            n++;
        }
        g_fan_groups++;
    }
    for (int side = -1; side <= 1; side += 2)
        for (int i = 0; i < 5; i++) {
            float rx, rz;
            samp_right(&g_tr[0], &rx, &rz);
            float off = (g_tr[0].hw + VERGE_W + 5.f + rnd() * 3.f) * (float)side, along = -14.f + (float)i * 6.f + rnd() * 2.f;
            add_prop(g_tr[0].x + rx * off + g_tr[0].tx * along, g_tr[0].z + rz * off + g_tr[0].tz * along, OBJ_FAN,
                     0.9f + rnd() * 0.2f, 0.4f);
        }
    /* bushes along the verges: near detail only, you drive through them. Their own random numbers,
       so the rest of the scenery stays where it was */
    uint32_t br = 0xB5u;
#define BRND() ((br = br * 1664525u + 1013904223u), (float)(br >> 8) * (1.f / 16777216.f))
    for (int k = 1; k < g_ns; k += 2)
        for (int side = -1; side <= 1; side += 2) {
            if (BRND() > 0.4f)
                continue;
            const samp_t *t = &g_tr[k];
            float rx, rz;
            samp_right(t, &rx, &rz);
            float off = t->hw + VERGE_W + 1.2f + BRND() * BRND() * 9.f;
            float x = t->x + rx * off * (float)side + t->tx * (BRND() - 0.5f) * 3.f;
            float z = t->z + rz * off * (float)side + t->tz * (BRND() - 0.5f) * 3.f;
            float sc = 0.7f + BRND() * 0.6f;
            if (road_gap(x, z) < VERGE_W + 0.8f || lake_dist(x, z, 1.5f) < 1.f)
                continue;
            if (g_nprops < MAX_PROPS) {
                int kk = nearest_sample(x, z, -1, 0);
                g_prop[g_nprops++] = (prop_t){ x, z, 0, sc, BRND() * 6.2832f, OBJ_BUSH, (uint8_t)(kk / CHUNK) };
            }
        }
#undef BRND
    /* the grid */
    g_gx0 = g_bx0 - 60.f;
    g_gz0 = g_bz0 - 60.f;
    g_gw = (int)((g_bx1 - g_bx0 + 120.f) / GCELL) + 1;
    g_gh = (int)((g_bz1 - g_bz0 + 120.f) / GCELL) + 1;
    g_gstart = papp_alloc(sizeof(int16_t) * (size_t)(g_gw * g_gh + 1), PAPP_MEM_LARGE, 4);
    g_gidx = papp_alloc(sizeof(int16_t) * (size_t)(g_nprops + 1), PAPP_MEM_LARGE, 4);
    if (!g_gstart || !g_gidx)
        return;
    memset(g_gstart, 0, sizeof(int16_t) * (size_t)(g_gw * g_gh + 1));
    int *cnt = calloc((size_t)(g_gw * g_gh), sizeof(int));
    if (!cnt)
        return;
    for (int i = 0; i < g_nprops; i++) {
        int cx = (int)((g_prop[i].x - g_gx0) / GCELL), cz = (int)((g_prop[i].z - g_gz0) / GCELL);
        if (cx >= 0 && cz >= 0 && cx < g_gw && cz < g_gh)
            cnt[cz * g_gw + cx]++;
    }
    int acc = 0;
    for (int c = 0; c < g_gw * g_gh; c++) {
        g_gstart[c] = (int16_t)acc;
        acc += cnt[c];
        cnt[c] = 0;
    }
    g_gstart[g_gw * g_gh] = (int16_t)acc;
    for (int i = 0; i < g_nprops; i++) {
        int cx = (int)((g_prop[i].x - g_gx0) / GCELL), cz = (int)((g_prop[i].z - g_gz0) / GCELL);
        if (cx >= 0 && cz >= 0 && cx < g_gw && cz < g_gh) {
            int c = cz * g_gw + cx;
            g_gidx[g_gstart[c] + cnt[c]++] = (int16_t)i;
        }
    }
    free(cnt);
    papp_log("dustline: %d props, %d spectator groups, lake %.0f m from the road, grid %dx%d\n", g_nprops, g_fan_groups,
             (double)lake_gap(), g_gw, g_gh);
}

/* ── Chunk meshes ────────────────────────────────────────────────────────── */
typedef struct {
    e3d_mesh *road[2], *scen[2];     /* [0] near, [1] far */
    float cx, cz, r;
} chunk_t;
static chunk_t g_chunk[MAX_CHUNKS];
static int     g_nchunks;
static e3d_mesh *m_gantry, *m_lake[2];        /* the lake: [0] its muddy shore, [1] the water */

static const uint32_t road_rgb[NSURF][2] = { { 0x56565C, 0x5E5E63 }, { 0xA27A50, 0x9A744C }, { 0x9E968A, 0x958E82 },
                                             { 0, 0 }, { 0, 0 }, { 0xB4BCC8, 0xACB4C2 }, { 0x98BCD8, 0x90B4D2 },
                                             { 0xC49A62, 0xBC925C }, { 0xE2D6C0, 0xDACEB8 } };
static const uint32_t verge_rgb[NSURF]   = { 0x7C8450, 0x7E6A44, 0x857C68, 0, 0, 0xEEF2F6, 0xE6ECF2, 0xE0C28C, 0xD8C4A0 };
#define VERGE_RGB(s) ((s) == SURF_TARMAC ? g_st->tarmac_verge : verge_rgb[s])

/* The road from sample k0 to k1 as rows of shared vertices across it: the verges, the road
 * (split by a centre line on tarmac or two wheel ruts on the loose stuff). Faces up. */
#define ROW_MAX 8
static int road_row(const samp_t *t, int stride, float *off)
{
    float hw = t->hw;
    int n = 0;
    off[n++] = -hw - VERGE_W;
    off[n++] = -hw;
    if (stride == 1 && t->surf == SURF_TARMAC) {
        off[n++] = -0.12f;
        off[n++] = 0.12f;
    } else if (stride == 1) {
        off[n++] = -1.5f;
        off[n++] = -0.6f;
        off[n++] = 0.6f;
        off[n++] = 1.5f;
    }
    off[n++] = hw;
    off[n++] = hw + VERGE_W;
    return n;
}

static void road_strip(int k0, int k1, int stride)
{
    float off[ROW_MAX];
    int prev = -1, pn = 0, kprev = k0;
    /* every stride samples, and always the last one (a short last chunk still gets a strip) */
    for (int k = k0, end = 0; !end; kprev = k, k += stride) {
        if (k >= k1) {
            k = k1;
            end = 1;
        }
        int ka = k % g_ns;
        const samp_t *a = &g_tr[ka];
        float rx, rz;
        samp_right(a, &rx, &rz);
        int n = road_row(a, stride, off);
        int row = B.nv;
        for (int i = 0; i < n; i++)
            b_v(a->x + rx * off[i], 0, a->z + rz * off[i]);
        if (prev >= 0 && n != pn) {
            /* the surface changes here: a row of this layout at the last sample's place */
            const samp_t *p = &g_tr[kprev % g_ns];
            float px, pz;
            samp_right(p, &px, &pz);
            prev = B.nv;
            for (int i = 0; i < n; i++)
                b_v(p->x + px * off[i], 0, p->z + pz * off[i]);
        }
        if (prev >= 0) {
            const samp_t *p = &g_tr[kprev % g_ns];
            int s = p->surf, kp = kprev % g_ns;
            uint32_t rc = road_rgb[s][(kp / 4) & 1];
            bool kerb = s == SURF_TARMAC && fabsf(p->curv) > 1.f / 70.f;
            for (int i = 0; i + 1 < n; i++) {
                uint32_t c = rc;
                if (i == 0 || i == n - 2)
                    c = kerb ? ((kp & 1) ? 0xD02818 : 0xE8E8E0) : stride == 1 ? shade_rgb(VERGE_RGB(s), ((kp / 2) & 1) ? 1.f : 0.93f) : VERGE_RGB(s);
                else if (stride == 1 && s == SURF_TARMAC && i == 2)
                    c = ((kp / 3) & 1) == 0 ? 0xD8D8D0 : rc;
                else if (stride == 1 && s != SURF_TARMAC && (i == 2 || i == 4))
                    c = shade_rgb(rc, 0.86f);
                float ctr[3] = { a->x, -50.f, a->z };
                b_quad(prev + i, prev + i + 1, row + i + 1, row + i, c, ctr);
            }
        }
        prev = row;
        pn = n;
    }
}

/* a pine: a trunk and two cones; a birch: a white trunk and a round crown */
static void b_tree(const prop_t *p, bool far)
{
    float s = p->s;
    int fl = g_st->flora;
    if (p->kind == OBJ_PINE && fl == FLORA_DESERT) {
        /* a cactus: a column and two arms */
        float x = p->x, z = p->z, w = 0.32f * s, ca = cosf(p->rot), sa = sinf(p->rot);
        b_cone(x, 0, z, w * 1.1f, 4.4f * s, far ? 4 : 6, 0x4E7A3A, p->rot);
        if (!far) {
            float ax = x + ca * 0.75f * s, az = z + sa * 0.75f * s, bx = x - ca * 0.65f * s, bz = z - sa * 0.65f * s;
            b_box(fminf(x, ax) - 0.2f * s, 1.5f * s, fminf(z, az) - 0.2f * s, fmaxf(x, ax) + 0.2f * s, 1.85f * s,
                  fmaxf(z, az) + 0.2f * s, 0x48723A);
            b_box(ax - 0.22f * s, 1.5f * s, az - 0.22f * s, ax + 0.22f * s, 3.0f * s, az + 0.22f * s, 0x48723A);
            b_box(fminf(x, bx) - 0.18f * s, 2.2f * s, fminf(z, bz) - 0.18f * s, fmaxf(x, bx) + 0.18f * s, 2.5f * s,
                  fmaxf(z, bz) + 0.18f * s, 0x527E40);
            b_box(bx - 0.2f * s, 2.2f * s, bz - 0.2f * s, bx + 0.2f * s, 3.4f * s, bz + 0.2f * s, 0x527E40);
        }
    } else if (p->kind == OBJ_PINE) {
        bool sn = fl == FLORA_SNOW;
        if (!far)
            b_cone(p->x, 0, p->z, 0.35f * s, 2.0f * s, 4, 0x5A4030, p->rot);
        b_cone(p->x, 1.2f * s, p->z, 2.4f * s, 5.0f * s, far ? 4 : 6, sn ? 0x2C4E38 : 0x2E5A2A, p->rot);
        if (!far)
            b_cone(p->x, 3.6f * s, p->z, 1.6f * s, 3.6f * s, 5, sn ? 0xEEF2F6 : 0x3A6E32, p->rot + 0.4f);
    } else if (p->kind == OBJ_BIRCH) {
        if (!far)
            b_cone(p->x, 0, p->z, 0.25f * s, 4.0f * s, 4, fl == FLORA_FOREST ? 0xE0DCD0 : 0x6A5038, p->rot);
        /* the crown: an octahedron (snow: frosted; desert: a flat acacia crown) */
        float cy = 4.6f * s, r = 2.0f * s;
        static const uint32_t crown[3][2] = { { 0x7AA040, 0x6A9238 }, { 0xDCE4EC, 0xC4D0DC }, { 0x7E8444, 0x6C7238 } };
        float up = fl == FLORA_DESERT ? 0.45f : 1.3f, down = fl == FLORA_DESERT ? 0.3f : 0.9f;
        int v0 = B.nv;
        b_v(p->x + r, cy, p->z);
        b_v(p->x - r, cy, p->z);
        b_v(p->x, cy + r * up, p->z);
        b_v(p->x, cy - r * down, p->z);
        b_v(p->x, cy, p->z + r);
        b_v(p->x, cy, p->z - r);
        float c[3] = { p->x, cy, p->z };
        for (int a = 0; a < 2; a++)
            for (int b = 2; b < 4; b++)
                for (int d = 4; d < 6; d++)
                    b_tri(v0 + a, v0 + b, v0 + d, crown[fl][(a + b + d) & 1], c);
    } else if (p->kind == OBJ_ROCK) {
        float r = 1.1f * s;
        int v0 = B.nv;
        b_v(p->x + r, 0.2f, p->z + r * 0.3f);
        b_v(p->x - r * 0.9f, 0.1f, p->z - r * 0.2f);
        b_v(p->x + r * 0.1f, r * 1.1f, p->z);
        b_v(p->x, -0.2f, p->z);
        b_v(p->x - r * 0.2f, 0.3f, p->z + r);
        b_v(p->x + r * 0.3f, 0.1f, p->z - r * 0.9f);
        float c[3] = { p->x, 0.3f, p->z };
        for (int a = 0; a < 2; a++)
            for (int b = 2; b < 4; b++)
                for (int d = 4; d < 6; d++)
                    b_tri(v0 + a, v0 + b, v0 + d, fl == FLORA_DESERT ? ((a + d) & 1 ? 0xB4744C : 0x9C6240)
                                                  : fl == FLORA_SNOW ? ((a + d) & 1 ? 0x9AA0A8 : 0xE4EAF0)
                                                  : ((a + d) & 1 ? 0x8C8A84 : 0x7A7872), c);
    } else if (p->kind == OBJ_BUSH && !far) {
        /* a low mound on the ground: four corners and a top, two greens */
        float r = 1.0f * p->s, cy = 0.02f, a = p->rot;
        float ca = cosf(a), sa = sinf(a);
        int v0 = B.nv;
        b_v(p->x + r * ca, cy, p->z + r * sa);
        b_v(p->x - r * ca, cy, p->z - r * sa);
        b_v(p->x, 0.85f * p->s, p->z);
        b_v(p->x, -0.05f, p->z);
        b_v(p->x - r * 0.8f * sa, cy, p->z + r * 0.8f * ca);
        b_v(p->x + r * 0.8f * sa, cy, p->z - r * 0.8f * ca);
        float c[3] = { p->x, 0.2f, p->z };
        for (int a2 = 0; a2 < 2; a2++)
            for (int b = 2; b < 3; b++)            /* the top half only: the bottom is under the ground */
                for (int d = 4; d < 6; d++)
                    b_tri(v0 + a2, v0 + b, v0 + d, fl == FLORA_SNOW ? ((a2 + d) & 1 ? 0xF2F5F8 : 0xDCE4EE)
                                                   : fl == FLORA_DESERT ? ((a2 + d) & 1 ? 0x8C8048 : 0x7A6E3C)
                                                   : ((a2 + d) & 1 ? 0x46702E : 0x3C6428), c);
    } else if (p->kind == OBJ_POST && !far) {
        b_box(p->x - 0.1f, 0, p->z - 0.1f, p->x + 0.1f, 1.0f, p->z + 0.1f, 0xF0F0F0);
        b_box(p->x - 0.11f, 0.7f, p->z - 0.11f, p->x + 0.11f, 0.9f, p->z + 0.11f, 0xE03020);
    } else if (p->kind == OBJ_FAN && !far) {
        static const uint32_t shirt[] = { 0xD03030, 0x2E5CC8, 0xE8C020, 0xF0F0F0, 0x30A050, 0xE07018 };
        float s = p->s, x = p->x, z = p->z;
        b_box(x - 0.16f * s, 0, z - 0.2f * s, x + 0.16f * s, 0.85f * s, z + 0.2f * s, 0x3A3E50);
        b_box(x - 0.2f * s, 0.85f * s, z - 0.26f * s, x + 0.2f * s, 1.5f * s, z + 0.26f * s,
              shirt[(int)(p->rot * 10.f) % 6]);
        b_box(x - 0.13f * s, 1.52f * s, z - 0.13f * s, x + 0.13f * s, 1.8f * s, z + 0.13f * s, 0xE0B090);
    } else if (p->kind == OBJ_HAY && !far) {
        float c = cosf(p->rot), s2 = sinf(p->rot);
        (void)s2;
        float hx = 0.9f, hz = 0.5f;
        int v0 = B.nv;
        for (int i = 0; i < 8; i++) {
            float lx = (i & 1) ? hx : -hx, lz = (i & 4) ? hz : -hz;
            float a = p->rot;
            b_v(p->x + lx * cosf(a) - lz * sinf(a), (i & 2) ? 0.9f : 0.f, p->z + lx * sinf(a) + lz * cosf(a));
        }
        (void)c;
        float ct[3] = { p->x, 0.45f, p->z };
        uint32_t col = 0xD8B860;
        b_quad(v0 + 0, v0 + 1, v0 + 3, v0 + 2, col, ct);
        b_quad(v0 + 4, v0 + 5, v0 + 7, v0 + 6, col, ct);
        b_quad(v0 + 2, v0 + 3, v0 + 7, v0 + 6, 0xE8CC78, ct);
        b_quad(v0 + 0, v0 + 2, v0 + 6, v0 + 4, col, ct);
        b_quad(v0 + 1, v0 + 3, v0 + 7, v0 + 5, col, ct);
    }
}

static bool build_chunks(void)
{
    g_nchunks = (g_ns + CHUNK - 1) / CHUNK;
    for (int c = 0; c < g_nchunks; c++) {
        int k0 = c * CHUNK, k1 = k0 + CHUNK;
        if (k1 > g_ns)
            k1 = g_ns;
        chunk_t *ch = &g_chunk[c];
        float cx = 0, cz = 0;
        for (int k = k0; k <= k1; k++) {
            cx += g_tr[k % g_ns].x;
            cz += g_tr[k % g_ns].z;
        }
        cx /= (float)(k1 - k0 + 1);
        cz /= (float)(k1 - k0 + 1);
        float r = 0;
        for (int k = k0; k <= k1; k++)
            r = fmaxf(r, hypotf(g_tr[k % g_ns].x - cx, g_tr[k % g_ns].z - cz));
        ch->cx = cx;
        ch->cz = cz;
        ch->r = r + 45.f;
        for (int lod = 0; lod < 2; lod++) {
            b_reset();
            road_strip(k0, k1, lod ? 4 : 1);
            ch->road[lod] = b_bake(1.f);
            b_reset();
            for (int i = 0; i < g_nprops; i++)
                if (g_prop[i].chunk == c && g_prop[i].kind != OBJ_PILLAR)
                    b_tree(&g_prop[i], lod == 1);
            ch->scen[lod] = b_bake(35.f);
            if (!ch->road[lod])
                return false;
        }
    }
    /* the start gantry: two pillars and a chequered banner across the road */
    b_reset();
    {
        float hw = g_tr[0].hw + 1.2f;
        b_box(-0.4f, 0, -hw - 0.4f, 0.4f, 5.6f, -hw + 0.4f, 0x303848);
        b_box(-0.4f, 0, hw - 0.4f, 0.4f, 5.6f, hw + 0.4f, 0x303848);
        int n = 12;
        for (int i = 0; i < n; i++) {
            float z0 = -hw + 2.f * hw * (float)i / (float)n, z1 = -hw + 2.f * hw * (float)(i + 1) / (float)n;
            b_box(-0.15f, 4.6f, z0, 0.15f, 5.2f, z1, (i & 1) ? 0x101010 : 0xF0F0F0);
            b_box(-0.15f, 5.2f, z0, 0.15f, 5.8f, z1, (i & 1) ? 0xF0F0F0 : 0x101010);
        }
    }
    m_gantry = b_bake(1.f);
    for (int l = 0; l < 2; l++) {
        b_reset();
        int c0 = b_v(LAKE_X, 0, LAKE_Z);
        for (int i = 0; i < LAKE_SEG; i++) {
            float x, z;
            lake_point(i, l ? 1.f : 1.08f, &x, &z);
            b_v(x, 0, z);
        }
        float ctr[3] = { LAKE_X, -5.f, LAKE_Z };   /* below: the faces point up */
        for (int i = 0; i < LAKE_SEG; i++)
            b_tri(c0, c0 + 1 + i, c0 + 1 + (i + 1) % LAKE_SEG, l ? g_st->water[i & 1] : g_st->shore, ctr);
        m_lake[l] = b_bake(1.f);
    }
    return m_gantry && m_lake[0] && m_lake[1];
}

/* ── The car ─────────────────────────────────────────────────────────────────
 * Body frame: x forward, z right, y up. A bicycle model with slip angles. */
#define CAR_M      1150.f
#define CAR_I      1700.f
#define CAR_A      1.15f        /* CG to the front axle */
#define CAR_B      1.35f        /* CG to the rear axle */
#define CAR_CF     52000.f      /* cornering stiffness, N/rad */
#define CAR_CR     58000.f
#define CAR_POWER  125000.f     /* W */
#define CAR_FMAX   10500.f      /* the most the engine pushes, N */
#define CAR_DRAG   0.42f        /* air: N per (m/s)^2 */
#define CAR_BRAKE  13000.f
#define CAR_STEER  0.55f        /* full lock, rad */
#define STEER_SPEED 0.065f      /* lock lost with speed: lock = CAR_STEER / (1 + speed * this) */
#define STEER_RATE  3.5f        /* the wheels turn at most this many full locks a second */
#define CAR_R      1.25f        /* collision radius */
#define GRAV       9.81f

typedef struct {
    float x, z, h;              /* position, heading (rad) */
    float vx, vz, r;            /* world velocity, yaw rate */
    float steer;                /* front wheel angle */
    float vf, vl;               /* body-frame velocity (for effects) */
    float slip;                 /* how much it slides, 0.. */
    float pf, pr;               /* last step's side forces (traction control, ABS) */
    float spin;                 /* wheel turn, degrees */
    float roll, pitch;          /* body lean, for looks */
    float ax_s, ay_s;           /* smoothed accelerations */
    int   gear;
    float rpm;
    int   idx;                  /* nearest track sample */
    uint8_t surf;
} car_t;

static car_t g_car;

typedef struct {
    float steer;                /* -1..1, + = right */
    float gas, brake, rev;      /* 0..1; rev = reverse (brakes while still rolling forward) */
    bool  hand, reset;
} drive_t;

static uint8_t car_surface(const car_t *c)
{
    const samp_t *s = &g_tr[c->idx];
    float d = fabsf(lateral(c->idx, c->x, c->z));
    if (d <= s->hw)
        return s->surf;
    if (d <= s->hw + VERGE_W)
        return SURF_VERGE;
    return SURF_GRASS;
}

static float long_room(float fx, float cap, float side, bool locked)
{
    if (locked)
        return clampf(fx, -cap, cap);
    float room = sqrtf(fmaxf(0, cap * cap - side * side));
    if (fx > 0)
        return fminf(fx, fmaxf(room, 0.3f * cap));
    return fmaxf(fx, -fmaxf(room, 0.5f * cap));
}

static const float gear_kmh[] = { 0, 52, 88, 122, 156, 192 };   /* top speed of each gear */

static void car_step(car_t *c, const drive_t *in, float dt)
{
    float ch = cosf(c->h), sh = sinf(c->h);
    float vf = c->vx * ch + c->vz * sh;        /* forward */
    float vl = -c->vx * sh + c->vz * ch;       /* right */
    float mu = surf_mu[c->surf];
    float speed = hypotf(vf, vl);

    /* steering: less lock at speed, wheels turn at a finite rate */
    float lock = CAR_STEER / (1.f + speed * STEER_SPEED);
    float want = in->steer * lock;
    float rate = STEER_RATE * lock * dt;
    c->steer += clampf(want - c->steer, -rate, rate);
    float d = c->steer;

    /* loads: weight shifts with acceleration */
    float nf = CAR_M * GRAV * CAR_B / (CAR_A + CAR_B) - c->ax_s * CAR_M * 0.5f / (CAR_A + CAR_B) * 0.55f;
    float nr = CAR_M * GRAV - nf;
    nf = fmaxf(nf, 1500.f);
    nr = fmaxf(nr, 1500.f);

    /* longitudinal: four-wheel drive, 40/60 front/rear */
    float drive = 0;
    if (in->gas > 0) {
        float v = fmaxf(fabsf(vf), 3.f);
        drive = in->gas * fminf(CAR_FMAX, CAR_POWER / v);
    }
    float brake = 0;
    if (in->brake > 0 && fabsf(vf) > 0.5f)
        brake = in->brake * CAR_BRAKE;           /* at a stop it holds the car (below) */
    if (in->rev > 0) {
        if (vf > 1.f)
            brake = fmaxf(brake, in->rev * CAR_BRAKE);
        else
            drive -= in->rev * 5200.f;           /* reverse */
    }
    if (vf < -7.f && drive < 0)
        drive = 0;                               /* reverse is slow */
    float fx_f = drive * 0.4f, fx_r = drive * 0.6f;
    float bsign = vf > 0 ? 1.f : -1.f;
    fx_f -= brake * 0.6f * bsign;
    fx_r -= brake * 0.4f * bsign;
    if (in->hand)
        fx_r -= 4500.f * bsign * fminf(1.f, fabsf(vf));

    /* lateral: slip angles in each axle's frame */
    float vfa = vf, vla = vl + CAR_A * c->r;        /* front axle */
    float wl = vla * cosf(d) - vfa * sinf(d);       /* across the front wheels */
    float wf = vfa * cosf(d) + vla * sinf(d);       /* along them */
    float af = atan2f(wl, fmaxf(fabsf(wf), 1.5f));
    float ar = atan2f(vl - CAR_B * c->r, fmaxf(fabsf(vf), 1.5f));

    /* friction circle per axle (the handbrake locks the rear: little side grip) */
    /* the rear grips a little more than the front: at the limit the car runs wide rather than
       spins (the handbrake or a lift is what swings the tail) */
    /* the rear grips more than the front (it runs wide at the limit), a little less so on the
     * loose stuff, where the tail comes round; lower than 1.1 and it spins */
    float cap_f = mu * nf, cap_r = mu * nr * (c->surf == SURF_TARMAC ? 1.15f : 1.10f) * (in->hand ? 0.45f : 1.f);
    /* traction control and ABS: the side force keeps what it needs, drive and brakes get the rest
       (at least 30% / 50% of the grip) */
    fx_f = long_room(fx_f, cap_f, c->pf, false);
    fx_r = long_room(fx_r, cap_r, c->pr, in->hand);
    float lim_f = sqrtf(fmaxf(0, cap_f * cap_f - fx_f * fx_f));
    float lim_r = sqrtf(fmaxf(0, cap_r * cap_r - fx_r * fx_r));
    /* linear up to what the friction circle leaves, flat past it */
    float fy_f = clampf(-CAR_CF * af, -lim_f, lim_f), fy_r = clampf(-CAR_CR * ar, -lim_r, lim_r);

    /* forces on the body */
    float fx = fx_f * cosf(d) - fy_f * sinf(d) + fx_r;
    float fy = fx_f * sinf(d) + fy_f * cosf(d) + fy_r;
    float drag = CAR_DRAG * speed + surf_drag[c->surf];
    fx -= drag * vf;
    fy -= drag * vl;
    c->pf = fy_f;
    c->pr = fy_r;
    float tq = CAR_A * (fx_f * sinf(d) + fy_f * cosf(d)) - CAR_B * fy_r;
    /* stability aid: pulls the yaw rate towards what the front wheels ask, within the grip.
     * Without it a slide on the loose stuff at speed went on for 2-3 s after the wheel was
     * straight (no grip left at the rear to stop it). Off with the handbrake, for its slides. */
    if (!in->hand) {
        float rk = vf * tanf(d) / (CAR_A + CAR_B), rg = 1.1f * mu * GRAV / fmaxf(speed, 1.f);
        tq += clampf(-5.f * CAR_I * (c->r - clampf(rk, -rg, rg)), -4000.f, 4000.f);
    }

    float ax = fx / CAR_M, ay = fy / CAR_M;
    vf += (ax + vl * c->r) * dt;
    vl += (ay - vf * c->r) * dt;
    c->r += tq / CAR_I * dt;
    /* at a crawl the tyres just stop it sliding */
    if (speed < 2.f) {
        float k = expf(-dt * 6.f);
        vl *= k;
        if (fabsf(vf) < 1.f && in->gas <= 0 && in->rev <= 0) {
            vf *= k;
            c->r *= k;
        }
    }
    c->h += c->r * dt;
    if (c->h > (float)M_PI) c->h -= 2.f * (float)M_PI;
    if (c->h < -(float)M_PI) c->h += 2.f * (float)M_PI;
    ch = cosf(c->h);
    sh = sinf(c->h);
    c->vx = vf * ch - vl * sh;
    c->vz = vf * sh + vl * ch;
    c->x += c->vx * dt;
    c->z += c->vz * dt;
    c->vf = vf;
    c->vl = vl;
    c->ax_s += (ax - c->ax_s) * fminf(1.f, dt * 5.f);
    c->ay_s += (ay - c->ay_s) * fminf(1.f, dt * 5.f);
    c->slip = fabsf(ar) * 3.f + (fabsf(fy_r) >= lim_r * 0.99f && speed > 4.f ? 0.5f : 0.f);
    c->spin += vf * dt / 0.33f * DEG;
    if (c->spin > 3600.f || c->spin < -3600.f)
        c->spin = fmodf(c->spin, 360.f);
}

/* the props: push the car out, bounce, lose speed. Returns the impact speed. */
static float car_collide(car_t *c)
{
    if (!g_gstart)
        return 0;
    int cx = (int)((c->x - g_gx0) / GCELL), cz = (int)((c->z - g_gz0) / GCELL);
    float hit = 0;
    for (int j = cz - 1; j <= cz + 1; j++)
        for (int i = cx - 1; i <= cx + 1; i++) {
            if (i < 0 || j < 0 || i >= g_gw || j >= g_gh)
                continue;
            int cell = j * g_gw + i;
            for (int q = g_gstart[cell]; q < g_gstart[cell + 1]; q++) {
                const prop_t *p = &g_prop[g_gidx[q]];
                if (p->kind == OBJ_POST || p->kind == OBJ_BUSH)
                    continue;            /* knocked flat / driven through, no matter */
                float dx = c->x - p->x, dz = c->z - p->z, rr = p->r * p->s + CAR_R;
                if (p->kind == OBJ_PILLAR || p->kind == OBJ_HAY)
                    rr = p->r + CAR_R;
                float d2 = dx * dx + dz * dz;
                if (d2 >= rr * rr || d2 < 1e-6f)
                    continue;
                float dd = sqrtf(d2), nx = dx / dd, nz = dz / dd;
                c->x = p->x + nx * rr;
                c->z = p->z + nz * rr;
                float vn = c->vx * nx + c->vz * nz;
                if (vn < 0) {
                    hit = fmaxf(hit, -vn);
                    float soft = p->kind == OBJ_HAY ? 0.2f : 0.35f;
                    c->vx -= (1.f + soft) * vn * nx;
                    c->vz -= (1.f + soft) * vn * nz;
                    c->vx *= 0.8f;
                    c->vz *= 0.8f;
                    c->r = c->r * 0.5f + (frand() - 0.5f) * 0.02f * (-vn);
                }
            }
        }
    return hit;
}

static void car_place(car_t *c, int k, float back)
{
    const samp_t *s = &g_tr[k];
    memset(c, 0, sizeof(*c));
    c->x = s->x - s->tx * back;
    c->z = s->z - s->tz * back;
    c->h = atan2f(s->tz, s->tx);
    c->idx = k;
    c->gear = 1;
    c->surf = s->surf;
}

/* ── Game state ──────────────────────────────────────────────────────────── */
enum { ST_TITLE, ST_COUNT, ST_RACE, ST_FINISH };
static int   g_state;
static float g_state_t, g_t;
#define RACE_LAPS 3                        /* race records count only for this length (laps N = testing) */
static int   g_laps = RACE_LAPS;
static int   g_lap;                       /* 0-based, the lap being driven */
static uint8_t g_cp;                      /* checkpoints passed this lap: bits 0..2 */
static float g_race_t, g_lap_t;           /* timers */
static float g_lap_times[9];
static float g_best_lap = 0, g_best_race = 0;   /* records, 0 = none */
static bool  g_new_lap_rec, g_new_race_rec;
static float g_wrong_t, g_off_t, g_stuck_t;
static bool  g_auto, g_icon, g_auto_rec, g_show_fps, g_evening;
/* how the player drives: the board's roller + the touch screen; a gamepad (d-pad / left stick
   steer, R2 / UP gas); or twin sticks (right stick steers, left stick up = gas, down = brake).
   -1 until chosen: the roller if the board has one, else the gamepad. */
enum { CTRL_ROLLER, CTRL_PAD, CTRL_TWIN, NCTRL_MODES };
static const char *const ctrl_name[NCTRL_MODES] = { "Touch + roller", "Gamepad", "Twin stick" };
static int g_ctrl = -1;   /* g_auto_rec: the computer's times count (testing) */
static float g_msg_t;
static const char *g_msg;
static float g_shake;
static float g_cam_h, g_cam_x, g_cam_z;   /* the chase camera */
static float g_last_lap_t, g_split_t;     /* "LAP 1:02.3" pops up for a moment */

static void note(const char *msg)
{
    g_msg = msg;
    g_msg_t = 2.f;
}

#define DATA_DIR    "/sd/data/dustline"
/* each stage its own records, ghost and saved race; the first stage keeps the old names */
static const char *stage_path(const char *base, const char *ext)
{
    static char buf[64];
    snprintf(buf, sizeof(buf), DATA_DIR "/%s%s.%s", base, g_st->file, ext);
    return buf;
}
#define REC_PATH    stage_path("records", "txt")
#define SAVE_PATH   stage_path("save", "txt")
#define SET_PATH    DATA_DIR "/settings.txt"
static bool g_look_dirty;                 /* the evening switch changed: apply_look in the main loop */
static bool g_nofog;                      /* test switch: evening without the haze */
static bool g_notes_on = true;

static void settings_io(bool save)
{
    if (save) {
        mkdir("/sd/data", 0777);
        mkdir(DATA_DIR, 0777);
        FILE *f = fopen(SET_PATH, "w");
        if (f) {
            fprintf(f, "evening=%d\nstage=%d\ncontrols=%d\n", g_evening ? 1 : 0, g_stage, g_ctrl);
            fclose(f);
        }
    } else {
        FILE *f = fopen(SET_PATH, "r");
        if (f) {
            int v;
            if (fscanf(f, "evening=%d ", &v) == 1)
                g_evening = v != 0;
            if (fscanf(f, "stage=%d ", &v) == 1 && v >= 0 && v < NSTAGES) {
                g_stage = v;
                g_st = &k_stage[v];
            }
            if (fscanf(f, "controls=%d", &v) == 1 && v >= 0 && v < NCTRL_MODES)
                g_ctrl = v;
            fclose(f);
        }
    }
}

static void records_io(bool save)
{
    if (save && g_auto && !g_auto_rec)
        return;
    if (save) {
        mkdir("/sd/data", 0777);
        mkdir(DATA_DIR, 0777);
        FILE *f = fopen(REC_PATH, "w");
        if (f) {
            fprintf(f, "DUSTLINE 1 lap %d race %d\n", (int)(g_best_lap * 1000.f), (int)(g_best_race * 1000.f));
            fclose(f);
        }
    } else {
        FILE *f = fopen(REC_PATH, "r");
        if (f) {
            int l, r;
            if (fscanf(f, "DUSTLINE 1 lap %d race %d", &l, &r) == 2 && l >= 0 && r >= 0) {
                g_best_lap = (float)l / 1000.f;
                g_best_race = (float)r / 1000.f;
            }
            fclose(f);
        }
    }
}

/* ── The ghost: the best lap, driven again in wireframe ──────────────────────
 * The car's place is recorded every GHOST_DT of the lap; a lap that beats the
 * ghost becomes the ghost (on the card when it is a record). Also the times at
 * the three checkpoints, for the splits. */
#define GHOST_DT   0.1f
#define GHOST_MAX  3000                   /* 300 s */
#define GHOST_PATH stage_path("ghost", "bin")
typedef struct { int16_t x, z, h; } gpose_t;
static gpose_t g_rec[GHOST_MAX], g_ghost[GHOST_MAX];
static int     g_nrec, g_nghost;
static bool    g_rec_ok;                  /* this lap was recorded from its start */
static float   g_ghost_t;                 /* the ghost's lap time, 0 = none */
static float   g_cp_now[3], g_cp_ghost[3];
static float   g_delta, g_delta_t;        /* split against the ghost, shown while g_delta_t > 0 */
static bool    g_ghost_on = true;

static void ghost_io(bool save)
{
    if (save) {
        mkdir("/sd/data", 0777);
        mkdir(DATA_DIR, 0777);
        FILE *f = fopen(GHOST_PATH, "wb");
        if (!f)
            return;
        int32_t hd[6] = { 0x31474C44, (int32_t)(g_ghost_t * 1000.f), (int32_t)(g_cp_ghost[0] * 1000.f),
                          (int32_t)(g_cp_ghost[1] * 1000.f), (int32_t)(g_cp_ghost[2] * 1000.f), g_nghost };
        fwrite(hd, sizeof(hd), 1, f);
        fwrite(g_ghost, sizeof(gpose_t), (size_t)g_nghost, f);
        fclose(f);
    } else {
        FILE *f = fopen(GHOST_PATH, "rb");
        if (!f)
            return;
        int32_t hd[6];
        if (fread(hd, sizeof(hd), 1, f) == 1 && hd[0] == 0x31474C44 && hd[1] > 0 && hd[5] > 0 && hd[5] <= GHOST_MAX &&
            fread(g_ghost, sizeof(gpose_t), (size_t)hd[5], f) == (size_t)hd[5]) {
            g_ghost_t = (float)hd[1] / 1000.f;
            for (int i = 0; i < 3; i++)
                g_cp_ghost[i] = (float)hd[2 + i] / 1000.f;
            g_nghost = hd[5];
        }
        fclose(f);
    }
}

static void ghost_record(float lap_t)
{
    int n = (int)(lap_t / GHOST_DT) + 1;
    while (g_nrec < n && g_nrec < GHOST_MAX) {
        g_rec[g_nrec].x = (int16_t)clampf(g_car.x * 16.f, -32767, 32767);
        g_rec[g_nrec].z = (int16_t)clampf(g_car.z * 16.f, -32767, 32767);
        g_rec[g_nrec].h = (int16_t)(g_car.h * 10000.f);
        g_nrec++;
    }
}

/* the ghost's place at lap time t; false when it has none */
static bool ghost_pose(float t, float *x, float *z, float *h)
{
    if (g_nghost < 2 || t < 0)
        return false;
    float f = t / GHOST_DT;
    int i = (int)f;
    if (i >= g_nghost - 1)
        return false;
    f -= (float)i;
    const gpose_t *a = &g_ghost[i], *b = &g_ghost[i + 1];
    *x = ((float)a->x + ((float)b->x - (float)a->x) * f) / 16.f;
    *z = ((float)a->z + ((float)b->z - (float)a->z) * f) / 16.f;
    float ha = (float)a->h / 10000.f, hb = (float)b->h / 10000.f;
    float dh = fmodf(hb - ha + 3.f * (float)M_PI, 2.f * (float)M_PI) - (float)M_PI;
    *h = ha + dh * f;
    return true;
}

static void fmt_time(char *out, size_t n, float t)
{
    if (t <= 0) {
        snprintf(out, n, "-:--.--");
        return;
    }
    int cs = (int)(t * 100.f + 0.5f);
    snprintf(out, n, "%d:%02d.%02d", cs / 6000, (cs / 100) % 60, cs % 100);
}

static void race_reset(void)
{
    car_place(&g_car, 0, 9.f);
    g_lap = 0;
    g_cp = 0;
    g_race_t = g_lap_t = 0;
    memset(g_lap_times, 0, sizeof(g_lap_times));
    g_new_lap_rec = g_new_race_rec = false;
    g_wrong_t = g_off_t = g_stuck_t = 0;
    g_split_t = 0;
    g_nrec = 0;
    g_rec_ok = true;
    g_delta_t = 0;
    g_cam_h = g_car.h;
    g_cam_x = g_car.x;
    g_cam_z = g_car.z;
}

static float s_gas_lever;                 /* the touch gas slider, 0..1 */

static void start_count(void)
{
    g_state = ST_COUNT;
    g_state_t = 0;
    s_gas_lever = 0;              /* set it during the count for the launch */
}

static void new_race(void)
{
    race_reset();
    start_count();
}

/* ── Saved race ──────────────────────────────────────────────────────────── */
static int g_save_lap = -1;               /* lap of the saved race, -1 = none */

static void save_peek(void)
{
    g_save_lap = -1;
    FILE *f = fopen(SAVE_PATH, "r");
    if (!f)
        return;
    int lap;
    if (fscanf(f, "DUSTLINE 1 lap %d", &lap) == 1 && lap >= 0)
        g_save_lap = lap;
    fclose(f);
}

static bool save_race(void)
{
    if (g_state != ST_RACE && g_state != ST_COUNT)
        return false;
    mkdir("/sd/data", 0777);
    mkdir(DATA_DIR, 0777);
    FILE *f = fopen(SAVE_PATH, "w");
    if (!f) {
        papp_log("dustline: cannot write %s\n", SAVE_PATH);
        return false;
    }
    const car_t *c = &g_car;
    fprintf(f, "DUSTLINE 1 lap %d laps %d cp %d race %d laptime %d idx %d\n", g_lap, g_laps, g_cp,
            (int)(g_race_t * 1000.f), (int)(g_lap_t * 1000.f), c->idx);
    fprintf(f, "car %d %d %d %d %d %d\n", (int)(c->x * 100.f), (int)(c->z * 100.f), (int)(c->h * 10000.f),
            (int)(c->vx * 100.f), (int)(c->vz * 100.f), (int)(c->r * 10000.f));
    for (int i = 0; i < g_lap && i < 9; i++)
        fprintf(f, "%d\n", (int)(g_lap_times[i] * 1000.f));
    bool ok = fclose(f) == 0;
    if (ok)
        g_save_lap = g_lap;
    papp_log("dustline: %s lap %d, race %.2f s\n", ok ? "saved" : "could not save", g_lap + 1, (double)g_race_t);
    return ok;
}

static bool load_race(void)
{
    FILE *f = fopen(SAVE_PATH, "r");
    if (!f)
        return false;
    int lap, laps, cp, rt, lt, idx, x, z, h, vx, vz, r;
    bool ok = fscanf(f, "DUSTLINE 1 lap %d laps %d cp %d race %d laptime %d idx %d", &lap, &laps, &cp, &rt, &lt, &idx) == 6 &&
              fscanf(f, " car %d %d %d %d %d %d", &x, &z, &h, &vx, &vz, &r) == 6 && lap >= 0 && laps >= 1 &&
              laps <= 9 && lap < laps && idx >= 0 && idx < g_ns && rt >= 0 && lt >= 0;
    float times[9] = { 0 };
    for (int i = 0; ok && i < lap; i++) {
        int t;
        ok = fscanf(f, "%d", &t) == 1 && t > 0;
        times[i] = (float)t / 1000.f;
    }
    fclose(f);
    if (!ok) {
        papp_log("dustline: the saved race is damaged\n");
        return false;
    }
    race_reset();
    g_laps = laps;
    g_lap = lap;
    g_cp = (uint8_t)(cp & 7);
    g_race_t = (float)rt / 1000.f;
    g_lap_t = (float)lt / 1000.f;
    memcpy(g_lap_times, times, sizeof(times));
    g_rec_ok = false;                    /* this lap's start was not recorded: no ghost from it */
    car_t *c = &g_car;
    c->x = (float)x / 100.f;
    c->z = (float)z / 100.f;
    c->h = (float)h / 10000.f;
    /* it starts again from a standstill after the countdown */
    c->vx = c->vz = c->r = 0;
    (void)vx;
    (void)vz;
    (void)r;
    c->idx = idx;
    c->surf = car_surface(c);
    if (fabsf(lateral(idx, c->x, c->z)) > g_tr[idx].hw)
        car_place(c, idx, 0);    /* it was saved off the road: back on it */
    g_cam_h = c->h;
    g_cam_x = c->x;
    g_cam_z = c->z;
    start_count();
    papp_log("dustline: continued at lap %d, race %.2f s\n", lap + 1, (double)g_race_t);
    return true;
}

/* back on the road at the nearest sample, facing the right way */
static void car_recover(void)
{
    int k = g_car.idx;
    float rt = g_race_t;
    (void)rt;
    car_t c = g_car;
    car_place(&g_car, k, 0);
    g_car.gear = 1;
    (void)c;
    g_cam_h = g_car.h;
    sfx(300, 600, 160, WAVE_TRI, 0.12f);
}

/* ── Rules ───────────────────────────────────────────────────────────────── */
static void finish_lap(void)
{
    float lt = g_lap_t;
    if (g_lap < 9)
        g_lap_times[g_lap] = lt;
    g_last_lap_t = lt;
    g_split_t = 3.f;
    bool rec = (!g_auto || g_auto_rec) && (g_best_lap <= 0 || lt < g_best_lap);
    if (rec) {
        g_best_lap = lt;
        g_new_lap_rec = true;
    }
    papp_log("dustline: lap %d %.2f s%s\n", g_lap + 1, (double)lt, rec ? " (best)" : "");
    if (g_ghost_t > 0) {
        g_delta = lt - g_ghost_t;
        g_delta_t = 3.f;
    }
    /* a lap faster than the ghost becomes the ghost; on the card when it is a record */
    if (g_rec_ok && g_nrec > 10 && (g_ghost_t <= 0 || lt < g_ghost_t)) {
        memcpy(g_ghost, g_rec, sizeof(gpose_t) * (size_t)g_nrec);
        g_nghost = g_nrec;
        g_ghost_t = lt;
        memcpy(g_cp_ghost, g_cp_now, sizeof(g_cp_ghost));
        if (rec)
            ghost_io(true);
    }
    g_nrec = 0;
    g_rec_ok = true;
    g_lap++;
    g_lap_t = 0;
    g_cp = 0;
    if (g_lap >= g_laps) {
        g_state = ST_FINISH;
        g_state_t = 0;
        bool rrec = (!g_auto || g_auto_rec) && g_laps == RACE_LAPS && (g_best_race <= 0 || g_race_t < g_best_race);
        if (rrec) {
            g_best_race = g_race_t;
            g_new_race_rec = true;
        }
        papp_log("dustline: finished in %.2f s%s\n", (double)g_race_t, rrec ? " (best)" : "");
        if (g_new_lap_rec || g_new_race_rec)
            records_io(true);
        for (int i = 0; i < 5; i++)
            sfx_at(523.f * (1 + 0.25f * (float)(i % 3)), 523.f * (1 + 0.25f * (float)(i % 3)), 120, WAVE_SQUARE, 0.14f,
                   i * 120);
        /* a race over: the save is spent */
        remove(SAVE_PATH);
        g_save_lap = -1;
    } else {
        sfx_at(880, 880, 90, WAVE_SQUARE, 0.14f, 0);
        sfx_at(1175, 1175, 140, WAVE_SQUARE, 0.14f, 100);
    }
}

/* progress round the course: checkpoints at a quarter, a half, three quarters */
static void track_progress(int prev, int now)
{
    int q = g_ns / 4;
    for (int i = 1; i <= 3; i++) {
        int m = q * i;
        if (prev < m && now >= m && now - prev < 40 && !(g_cp & (1u << (i - 1)))) {
            g_cp |= (uint8_t)(1u << (i - 1));
            g_cp_now[i - 1] = g_lap_t;
            if (g_ghost_t > 0 && g_cp_ghost[i - 1] > 0 && g_state == ST_RACE) {
                g_delta = g_lap_t - g_cp_ghost[i - 1];
                g_delta_t = 2.5f;
            }
        }
    }
    /* over the line: from the last stretch to the first */
    if (prev > g_ns - 40 && now < 40) {
        if (g_cp == 7 && g_state == ST_RACE)
            finish_lap();
    }
}

/* ── Input ───────────────────────────────────────────────────────────────── */
#define TB_Y    186
#define TB_H    50
static uint32_t s_prev_btn;
static int s_touch_n;
static bool s_touch_used = true;        /* the touch controls show: from the start with the roller, else after a touch */
static int s_pot_raw = -1;                /* filtered reading */
static float s_pot_f = -1.f;
/* the roller is absolute: 0..65535 maps straight to full left..full right */
#define POT_MID   32768.f
#define POT_RANGE 32768.f                 /* counts from the middle to full lock */
static float s_pad_steer;                 /* pad left/right and the touch arrows, ramped */
static float s_touch_dig;                 /* touch arrows held in the last frame: -1, 0, 1 */
/* the gas slider, left edge: a lever that stays where the finger leaves it */
#define GS_X    4
#define GS_W    44
#define GS_Y0   (H / 2 - 35)              /* top = full gas; 70 px tall, centred */
#define GS_Y1   (H / 2 + 35)              /* bottom = no gas */
#define TBR_X   (W - 166)                 /* touch buttons, bottom right: < > BRAKE */
#define REV_Y   (TB_Y - 46)               /* REV, above BRAKE */
static float g_gas_show;                  /* the gas the car gets, for the slider */

static float slider_gas(int y)
{
    /* 8 px at each end snap to none / full */
    return clampf((float)(GS_Y1 - 8 - y) / (float)(GS_Y1 - GS_Y0 - 16), 0.f, 1.f);
}

/* gentle round the centre, full at the ends: small corrections stay small */
static float steer_curve(float v)
{
    return v * (0.35f + 0.65f * fabsf(v));
}
static float g_steer_show;                /* what the steering does, for the HUD */
static bool g_hand_held;

typedef struct {
    bool start, pause, tap;
    drive_t d;
} actions_t;

static actions_t read_input(papp_surface_t *surf, float dt)
{
    actions_t a = { 0 };
    papp_gamepad_t pad = { 0 };
    uint32_t held = papp_input_gamepad(0, &pad) ? pad.buttons : 0;
    uint32_t press = held & ~s_prev_btn;
    s_prev_btn = held;
    if (press & (PAPP_BTN_A | PAPP_BTN_B | PAPP_BTN_R1))
        a.start = true;
    if (press & (PAPP_BTN_START | PAPP_BTN_SELECT))
        a.pause = true;
    /* gas: the slider on the left of the screen (stays where it is left), the right trigger, or UP;
       twin sticks: the left stick up. Brake: A / B / L2 (analog when it is), twin: the left stick down */
    float gas = s_gas_lever;
    if (pad.rt > 2000)
        gas = fmaxf(gas, (float)pad.rt / 65535.f);
    else if (held & (PAPP_BTN_R2 | PAPP_BTN_UP))
        gas = 1.f;
    if (held & (PAPP_BTN_A | PAPP_BTN_B))
        a.d.brake = 1.f;
    if (pad.lt > 2000)
        a.d.brake = fmaxf(a.d.brake, (float)pad.lt / 65535.f);
    else if (held & PAPP_BTN_L2)
        a.d.brake = 1.f;
    if (g_ctrl == CTRL_TWIN) {
        const float dz = 0.15f;
        float v = clampf(-(float)pad.ly / 30000.f, -1.f, 1.f);   /* +y is down */
        if (v > dz)
            gas = fmaxf(gas, (v - dz) / (1.f - dz));
        else if (v < -dz)
            a.d.brake = fmaxf(a.d.brake, (-v - dz) / (1.f - dz));
    }
    if (held & PAPP_BTN_DOWN)
        a.d.rev = 1.f;
    if (held & (PAPP_BTN_X | PAPP_BTN_R1 | PAPP_BTN_L1))
        a.d.hand = true;
    if (press & PAPP_BTN_Y)
        a.d.reset = true;
    /* pad left/right ramp in over 0.3 s (back to the centre twice as fast); the stick is analog */
    float dig = 0;
    if (held & PAPP_BTN_LEFT) dig -= 1.f;
    if (held & PAPP_BTN_RIGHT) dig += 1.f;
    if (dig == 0)
        dig = s_touch_dig;
    float ramp = (dig == 0 || dig * s_pad_steer < 0 ? 7.f : 3.5f) * dt;
    s_pad_steer += clampf(dig - s_pad_steer, -ramp, ramp);
    float pad_steer = s_pad_steer;
    int sx = g_ctrl == CTRL_TWIN ? pad.rx : pad.lx;          /* twin sticks: the right one steers */
    if (dig == 0 && (sx > 5000 || sx < -5000))
        pad_steer = steer_curve(clampf((float)sx / 28000.f, -1.f, 1.f));

    /* the roller: absolute (its position is the steering), lightly filtered, a small dead zone */
    float pot = 0;
    int pv = g_ctrl == CTRL_ROLLER ? papp_input_analog(PAPP_ANALOG_PADDLE) : -1;
    if (pv >= 0) {
        if (s_pot_f < 0)
            s_pot_f = (float)pv;
        s_pot_f += ((float)pv - s_pot_f) * fminf(1.f, dt * 30.f);
        s_pot_raw = pv;
        float v = clampf((s_pot_f - POT_MID) / POT_RANGE, -1.f, 1.f);
        const float dz = 0.03f;
        pot = fabsf(v) < dz ? 0 : (v - copysignf(dz, v)) / (1.f - dz);
    }
    a.d.steer = pad_steer != 0 ? pad_steer : pot;

    /* touch: the gas slider on the left, steering arrows + BRAKE bottom right, REV above BRAKE,
       the top-right corner pauses */
    papp_touch_point_t tp[3];
    int n = papp_disp_touch(surf, tp, 3);
    s_touch_dig = 0;
    if (n > 0) {
        s_touch_used = true;
        for (int i = 0; i < n && i < 3; i++) {
            int x = tp[i].x, y = tp[i].y;
            if (x > W - 48 && y < 32) {
                if (s_touch_n == 0)
                    a.pause = true;
            } else if (g_state == ST_TITLE) {
                if (s_touch_n == 0)
                    a.tap = true;
            } else if (x < GS_X + GS_W + 14 && y >= GS_Y0 - 14 && y <= GS_Y1 + 14) {
                s_gas_lever = gas = slider_gas(y);
            } else if (x >= TBR_X && y >= TB_Y - 10) {
                if (x >= W - 62)
                    a.d.brake = 1.f;
                else if (x >= W - 114)
                    s_touch_dig = 1.f;
                else
                    s_touch_dig = -1.f;
            } else if (x >= W - 62 && y >= REV_Y - 6 && y < TB_Y - 10) {
                a.d.rev = 1.f;
            } else if (s_touch_n == 0)
                a.tap = true;
        }
    }
    s_touch_n = n > 0 ? n : 0;
    /* braking or reversing takes the gas off (the slider stays where it is) */
    a.d.gas = (a.d.brake > 0 || a.d.rev > 0) ? 0.f : gas;
    return a;
}

/* the computer drives: pure pursuit of a point ahead on the centre line, with the yaw rate that
   needs; slows for the tightest bend in the next 90 m */
static void auto_drive(drive_t *d)
{
    const car_t *c = &g_car;
    float speed = hypotf(c->vx, c->vz);
    int look = 3 + (int)(speed * 0.4f);
    int k = (c->idx + look) % g_ns;
    float dx = g_tr[k].x - c->x, dz = g_tr[k].z - c->z;
    float ld = fmaxf(hypotf(dx, dz), 4.f);
    float err = fmodf(atan2f(dz, dx) - c->h + 3.f * (float)M_PI, 2.f * (float)M_PI) - (float)M_PI;
    float kap = 2.f * sinf(err) / ld;
    float lock = CAR_STEER / (1.f + speed * STEER_SPEED);
    float delta = atanf(kap * (CAR_A + CAR_B)) * (1.f + speed * 0.01f) + 0.08f * (kap * fmaxf(speed, 1.f) - c->r);
    d->steer = clampf(delta / lock, -1.f, 1.f);
    float vmax = 60.f;
    for (int i = 0; i < 45; i += 2) {
        int q = (c->idx + i) % g_ns;
        float cv = fabsf(g_tr[q].curv) + 1e-4f;
        float mu = surf_mu[g_tr[q].surf] * 0.6f;
        float v = sqrtf(mu * GRAV / cv);
        float vb = sqrtf(v * v + 2.f * mu * GRAV * 0.6f * (float)i * STEP);
        if (vb < vmax)
            vmax = vb;
    }
    d->gas = speed < vmax - 1.f ? 1.f : speed < vmax ? 0.4f : 0.f;
    d->brake = speed > vmax + 2.f ? 1.f : 0.f;
    d->rev = 0;
    d->hand = false;
    d->reset = false;
}

/* ── Update ──────────────────────────────────────────────────────────────── */
/* dust: puffs thrown up behind the wheels */
#define MAX_DUST 70
typedef struct { float p[3], v[3], life, life0, size; uint32_t rgb; } dust_t;
static dust_t g_dust[MAX_DUST];
static float  g_dust_acc;

static void spawn_dust(float x, float y, float z, float vx, float vz, uint32_t rgb, float size)
{
    for (int i = 0; i < MAX_DUST; i++) {
        dust_t *p = &g_dust[i];
        if (p->life > 0)
            continue;
        p->p[0] = x;
        p->p[1] = y;
        p->p[2] = z;
        p->v[0] = vx + (frand() - 0.5f) * 2.f;
        p->v[1] = 0.5f + frand() * 1.0f;
        p->v[2] = vz + (frand() - 0.5f) * 2.f;
        p->life0 = p->life = 0.35f + frand() * 0.4f;
        p->size = size * (0.7f + frand() * 0.6f);
        p->rgb = rgb;
        return;
    }
}

static float s_phys_acc;
static float g_hit_t;

static void update(const actions_t *act, float dt)
{
    g_t += dt;
    g_state_t += dt;
    if (g_shake > 0) g_shake = fmaxf(0, g_shake - dt * 10.f);
    if (g_split_t > 0) g_split_t -= dt;
    if (g_hit_t > 0) g_hit_t -= dt;

    drive_t in = act->d;
    if (g_state == ST_TITLE || g_auto)
        auto_drive(&in);
    if (g_auto && g_state == ST_RACE && act->d.brake > 0) {   /* testing: A/B slows the computer down */
        in.gas = 0;
        in.brake = act->d.brake;
    }
    if (g_state == ST_COUNT) {
        in.gas = in.brake = in.rev = 0;
        in.hand = true;
    }
    if (g_state == ST_FINISH) {
        /* roll to a stop */
        in.gas = 0;
        in.brake = 0.3f;
        in.rev = 0;
        in.hand = false;
    }
    g_hand_held = in.hand;
    g_gas_show = in.gas;
    if (g_state == ST_COUNT && g_state_t >= 3.f) {
        g_state = ST_RACE;
        g_state_t = 0;
        sfx(1046, 1046, 400, WAVE_SQUARE, 0.18f);
    } else if (g_state == ST_COUNT) {
        int s0 = (int)(g_state_t - dt), s1 = (int)g_state_t;
        if (s1 != s0 || g_state_t - dt <= 0)
            sfx(523, 523, 180, WAVE_SQUARE, 0.16f);
    }
    if (g_state == ST_RACE) {
        g_race_t += dt;
        g_lap_t += dt;
    }

    car_t *c = &g_car;
    if (in.reset && g_state == ST_RACE)
        car_recover();
    g_steer_show = in.steer;

    s_phys_acc += dt;
    const float h = 1.f / 240.f;
    int prev = c->idx;
    float hit = 0;
    while (s_phys_acc >= h) {
        s_phys_acc -= h;
        c->idx = nearest_sample(c->x, c->z, c->idx, 6);
        c->surf = car_surface(c);
        car_step(c, &in, h);
        hit = fmaxf(hit, car_collide(c));
    }
    c->idx = nearest_sample(c->x, c->z, c->idx, 6);
    if (g_state != ST_COUNT)
        track_progress(prev, c->idx);
    if (g_state == ST_RACE)
        ghost_record(g_lap_t);
    if (g_delta_t > 0)
        g_delta_t -= dt;
    if (hit > 3.f && g_hit_t <= 0) {
        g_hit_t = 0.25f;
        g_shake = fminf(6.f, hit * 0.3f);
        sfx(140, 40, 260, WAVE_NOISE, fminf(0.4f, 0.06f + hit * 0.02f));
        sfx(90, 50, 200, WAVE_TRI, 0.2f);
    }

    /* looks: body lean, gears and revs */
    float speed = hypotf(c->vx, c->vz);
    c->roll += (clampf(-c->ay_s * 0.55f, -5.f, 5.f) - c->roll) * fminf(1.f, dt * 6.f);
    c->pitch += (clampf(c->ax_s * 0.35f, -3.f, 3.f) - c->pitch) * fminf(1.f, dt * 6.f);
    float kmh = fabsf(c->vf) * 3.6f;
    if (c->gear < 5 && kmh > gear_kmh[c->gear] * 0.97f) {
        c->gear++;
    } else if (c->gear > 1 && kmh < gear_kmh[c->gear - 1] * 0.62f)
        c->gear--;
    float lo = c->gear > 1 ? gear_kmh[c->gear - 1] * 0.55f : 0;
    float rpm = 1100.f + 6400.f * clampf((kmh - lo) / (gear_kmh[c->gear] - lo + 1e-3f), 0, 1.f);
    if (in.gas > 0 && (c->slip > 0.6f || kmh < 8.f))
        rpm = fmaxf(rpm, 3500.f + 2500.f * fminf(1.f, c->slip));     /* wheelspin: revs */
    c->rpm += (rpm - c->rpm) * fminf(1.f, dt * 8.f);
    s_eng_f = c->rpm / 60.f * 2.f;
    s_eng_vol = g_state == ST_TITLE ? 0.05f : 0.10f + 0.14f * in.gas;
    float slide = fabsf(c->vl) > 2.f ? fminf(1.f, (fabsf(c->vl) - 2.f) * 0.12f) : 0;
    bool loose = c->surf != SURF_TARMAC;
    s_skid_vol = g_state == ST_TITLE ? 0 : (loose ? 0.04f : 0.18f) * slide;
    s_grit_vol = g_state == ST_TITLE ? 0 : loose ? fminf(0.12f, speed * 0.004f) * (1.f + slide) : 0;

    /* dust behind the rear wheels on the loose surfaces (and smoke on tarmac when sliding) */
    float rate = 0;
    uint32_t dc = 0;
    if (c->surf == SURF_DIRT || c->surf == SURF_VERGE) {
        rate = speed * 0.9f + slide * 30.f;
        dc = 0xB89870;
    } else if (c->surf == SURF_GRAVEL) {
        rate = speed * 0.8f + slide * 30.f;
        dc = 0xB0A898;
    } else if (c->surf == SURF_SNOW || c->surf == SURF_ICE) {
        rate = speed * (c->surf == SURF_SNOW ? 1.0f : 0.4f) + slide * 30.f;
        dc = 0xF0F4FA;
    } else if (c->surf == SURF_SAND || c->surf == SURF_HARD) {
        rate = speed * (c->surf == SURF_SAND ? 1.0f : 0.7f) + slide * 30.f;
        dc = c->surf == SURF_SAND ? 0xD8B880 : 0xC8B494;
    } else if (c->surf == SURF_GRASS) {
        rate = speed * (g_st->flora == FLORA_FOREST ? 0.3f : 0.9f);
        dc = g_st->offroad_dust;
    } else if (slide > 0.3f) {
        rate = slide * 25.f;
        dc = 0xD0D0D0;
    }
    if (speed < 3.f)
        rate = 0;
    g_dust_acc += rate * dt;
    float chh = cosf(c->h), shh = sinf(c->h);
    while (g_dust_acc >= 1.f) {
        g_dust_acc -= 1.f;
        float side = frand() < 0.5f ? -0.8f : 0.8f;
        float x = c->x - chh * 1.9f - shh * side, z = c->z - shh * 1.9f + chh * side;
        spawn_dust(x, 0.15f, z, c->vx * 0.3f, c->vz * 0.3f, dc, 0.16f + fminf(0.16f, speed * 0.005f) + slide * 0.1f);
    }
    for (int i = 0; i < MAX_DUST; i++) {
        dust_t *p = &g_dust[i];
        if (p->life <= 0)
            continue;
        p->life -= dt;
        for (int k = 0; k < 3; k++)
            p->p[k] += p->v[k] * dt;
        p->v[0] *= 1.f - dt * 1.5f;
        p->v[2] *= 1.f - dt * 1.5f;
        p->v[1] *= 1.f - dt * 0.8f;
    }

    /* off the course or the wrong way for long: back on the road */
    float off = fabsf(lateral(c->idx, c->x, c->z)) - g_tr[c->idx].hw;
    g_off_t = off > 18.f ? g_off_t + dt : 0;
    {
        static bool was_off;
        if (g_auto && (off > 0.5f) != was_off) {
            was_off = off > 0.5f;
            if (was_off)
                papp_log("dustline: off the road at %d, %.0f km/h, curv %.3f\n", c->idx, (double)(speed * 3.6f),
                         (double)g_tr[c->idx].curv);
        }
    }
    float along = c->vx * g_tr[c->idx].tx + c->vz * g_tr[c->idx].tz;
    g_wrong_t = (along < -4.f && g_state == ST_RACE) ? g_wrong_t + dt : fmaxf(0, g_wrong_t - dt * 2.f);
    g_stuck_t = (speed < 1.f && (in.gas > 0 || in.rev > 0) && off > 0.f) ? g_stuck_t + dt : 0;
    if ((g_off_t > 3.f || g_stuck_t > 4.f) && g_state != ST_FINISH) {
        papp_log("dustline: back on the road at %d (%s)\n", c->idx, g_off_t > 3.f ? "off" : "stuck");
        car_recover();
        g_off_t = g_stuck_t = 0;
        note("BACK ON THE ROAD");
    }
    if (g_state == ST_TITLE && g_state_t > 0 && c->idx == 0 && prev > g_ns - 40)
        g_state_t = 0;

    /* the chase camera: follows the heading, leans towards where the car is going in a slide */
    float vh = atan2f(c->vz, c->vx);
    float target = c->h;
    if (speed > 4.f && c->vf > 0) {
        float dv = fmodf(vh - c->h + 3.f * (float)M_PI, 2.f * (float)M_PI) - (float)M_PI;
        target = c->h + dv * 0.55f;
    }
    float dh = fmodf(target - g_cam_h + 3.f * (float)M_PI, 2.f * (float)M_PI) - (float)M_PI;
    g_cam_h += dh * fminf(1.f, dt * 5.f);
    g_cam_x = c->x;
    g_cam_z = c->z;
}

/* ── Drawing ─────────────────────────────────────────────────────────────── */
#define FOCAL     235.f
#define CAM_BACK  7.6f
#define CAM_UP    2.9f
#define CAM_PITCH (-8.f)

static e3d_mesh *m_body, *m_wheel, *m_dust, *m_ghost;

/* the background: sky, hills on the horizon (by heading), grass below */
#define HILLS 1024
static uint8_t  s_far[HILLS], s_near[HILLS];
static uint16_t s_col[W];                 /* hill index of each column, set before each frame */
static uint16_t s_sky[H], s_grass[H], s_far_c[H], s_near_c[H];
static uint8_t  s_cl_lo[HILLS], s_cl_hi[HILLS];   /* clouds: rows above the horizon each column covers (lo > hi = none) */
static uint16_t s_cl_lit[H], s_cl_dark[H];
static int      s_cl_min = 255, s_cl_max;
static int      s_horizon;

/* Evening: a low golden sun and haze (fog) towards the horizon. The sky rows are not fogged by
   the engine (fog_keep_bg), so their colours fade to the fog colour here, by the depth of the
   ground each row shows. */
#define FOG_RGB      0xE8B888
#define FOG_DENSITY  0.005f
static void make_palette(void)
{
    bool ev = g_evening;
    for (int y = 0; y < H; y++) {
        float t = clampf((float)y / (float)(s_horizon > 0 ? s_horizon : 1), 0, 1);
        const stage_t *st = g_st;
        s_sky[y] = rgb565(ev ? mix_rgb(0x34488C, FOG_RGB, powf(t, 1.7f)) : mix_rgb(st->sky[0], st->sky[1], t * t));
        float g = clampf((float)(y - s_horizon) / (float)(H - s_horizon), 0, 1);
        uint32_t grass = ev ? mix_rgb(st->ev_ground[0], st->ev_ground[1], sqrtf(g))
                            : mix_rgb(st->ground[0], st->ground[1], sqrtf(g));
        if (ev && y >= s_horizon) {
            /* the ground this row shows, and the fog the engine would give it */
            float a = atanf(((float)y + 0.5f - H * 0.5f) / FOCAL) - CAM_PITCH * RAD;
            float d = a > 0.002f ? CAM_UP / tanf(a) : 2000.f;
            grass = mix_rgb(FOG_RGB, grass, expf(-d * FOG_DENSITY));
        }
        s_grass[y] = rgb565(grass);
        float hz = clampf((float)(s_horizon - y) / 40.f, 0, 1);
        if (ev) {
            s_far_c[y] = rgb565(mix_rgb(mix_rgb(st->ev_far[0], st->ev_far[1], hz), FOG_RGB, 0.5f));
            s_near_c[y] = rgb565(mix_rgb(mix_rgb(st->ev_near[0], st->ev_near[1], hz), FOG_RGB, 0.32f));
            s_cl_lit[y] = rgb565(0xFFDCAE);
            s_cl_dark[y] = rgb565(0xC88E8C);
        } else {
            s_far_c[y] = rgb565(mix_rgb(st->far[0], st->far[1], hz));
            s_near_c[y] = rgb565(mix_rgb(st->near[0], st->near[1], hz));
            s_cl_lit[y] = rgb565(0xF6F8FA);
            s_cl_dark[y] = rgb565(0xC4CEDA);
        }
    }
}

static void apply_look(e3d_t *e)
{
    e3d_params *par = e3d_params_get(e);
    par->ambient = g_evening ? 0.36f : 0.42f;
    par->diffuse = g_evening ? 0.86f : 0.80f;
    par->specular = g_evening ? 0.50f : 0.35f;
    par->shadow_dark = g_evening ? 0.50f : 0.55f;
    par->fog_density = g_evening && !g_nofog ? FOG_DENSITY : 0.f;
    par->fog_rgb565 = rgb565(FOG_RGB);
    par->fog_keep_bg = true;
    make_palette();
}

static void make_background(void)
{
    s_horizon = (int)(H * 0.5f - FOCAL * tanf(-CAM_PITCH * RAD) + 0.5f);
    make_palette();
    /* hills: sums of sines, wrapping round */
    for (int i = 0; i < HILLS; i++) {
        float a = (float)i / HILLS * 6.2832f;
        float f = 16.f + 9.f * sinf(a * 3.f + 1.f) + 6.f * sinf(a * 7.f + 2.f) + 3.f * sinf(a * 17.f) + 4.f * sinf(a * 2.f);
        float n = 5.f + 3.f * sinf(a * 23.f + 0.5f) + 2.f * sinf(a * 41.f) + 1.5f * sinf(a * 97.f);
        if (g_st->hills == HILLS_MOUNTAINS) {
            /* peaks: sharp tops, a ridge of pines in front */
            float pk = fabsf(sinf(a * 4.f + 1.f)), pk2 = fabsf(sinf(a * 9.f + 0.4f));
            f = 8.f + 24.f * pk * pk * pk + 9.f * pk2 * pk2 + 2.f * sinf(a * 31.f);
            n = 7.f + 3.f * sinf(a * 23.f + 0.5f) + 2.f * sinf(a * 41.f) + 2.f * fabsf(sinf(a * 97.f));
        } else if (g_st->hills == HILLS_MESAS) {
            /* flat-topped mesas far off, low dunes in front */
            float m = sinf(a * 5.f + 0.3f) + 0.5f * sinf(a * 11.f + 1.f);
            f = m > 0.75f ? 22.f + 2.f * sinf(a * 40.f) : m > 0.6f ? 4.f + (m - 0.6f) * 120.f : 3.f + 1.5f * sinf(a * 13.f);
            float d = sinf(a * 6.f + 1.f);
            n = 3.f + 4.f * d * d + 1.5f * sinf(a * 15.f);
        }
        s_far[i] = (uint8_t)clampf(f, 2.f, 40.f);
        s_near[i] = (uint8_t)clampf(n, 1.f, 12.f);
    }
    /* clouds: flat-bottomed heaps round the sky, by heading like the hills */
    for (int i = 0; i < HILLS; i++) {
        s_cl_lo[i] = 255;
        s_cl_hi[i] = 0;
    }
    uint32_t r = 0x5EEDu;
#define CRND() ((r = r * 1664525u + 1013904223u), (float)(r >> 8) * (1.f / 16777216.f))
    int pos = 10;
    while (pos < HILLS - 40) {
        int w = 14 + (int)(CRND() * 34.f);           /* half width, in columns of HILLS */
        if (pos + 2 * w >= HILLS)
            break;
        int base = 44 + (int)(CRND() * 40.f), th = 5 + w / 4 + (int)(CRND() * 5.f);
        float ph = CRND() * 6.f;
        for (int d = -w; d <= w; d++) {
            float u = (float)d / (float)w, p = sqrtf(fmaxf(0, 1.f - u * u));
            float bump = 0.65f + 0.35f * fabsf(sinf((float)d * 0.22f + ph)) * (0.6f + 0.4f * sinf((float)d * 0.07f + ph * 2.f));
            int i = pos + w + d, hi = base + (int)((float)th * powf(p, 0.6f) * bump);
            int lo = base + (int)((1.f - p) * 3.f);
            if (hi > lo) {
                s_cl_lo[i] = (uint8_t)lo;
                s_cl_hi[i] = (uint8_t)hi;
                if (lo < s_cl_min) s_cl_min = lo;
                if (hi > s_cl_max) s_cl_max = hi;
            }
        }
        pos += 2 * w + 10 + (int)(CRND() * 90.f);
    }
#undef CRND
}

static void bg_columns(float heading_deg)
{
    for (int x = 0; x < W; x++) {
        float a = heading_deg + atanf(((float)x + 0.5f - W * 0.5f) / FOCAL) * DEG;
        int i = (int)(a * (HILLS / 360.f)) % HILLS;
        if (i < 0)
            i += HILLS;
        s_col[x] = (uint16_t)i;
    }
}

static void sky_row(uint16_t *row, int y, int w, void *ctx)
{
    if (y >= s_horizon) {
        uint16_t c = s_grass[y];
        for (int x = 0; x < w; x++)
            row[x] = c;
        return;
    }
    int up = s_horizon - y;
    uint16_t sky = s_sky[y];
    if (up > 40) {
        if (up < s_cl_min || up > s_cl_max) {
            for (int x = 0; x < w; x++)
                row[x] = sky;
            return;
        }
        uint16_t lit = s_cl_lit[y], dark = s_cl_dark[y];
        for (int x = 0; x < w; x++) {
            int i = s_col[x];
            row[x] = up < s_cl_lo[i] || up > s_cl_hi[i] ? sky : up - s_cl_lo[i] < 3 ? dark : lit;
        }
        return;
    }
    uint16_t fc = s_far_c[y], nc = s_near_c[y];
    for (int x = 0; x < w; x++) {
        int i = s_col[x];
        row[x] = s_near[i] >= up ? nc : s_far[i] >= up ? fc : sky;
    }
}

static void draw_scene(e3d_t *e)
{
    const car_t *c = &g_car;
    e3d_camera *cam = e3d_cam(e);
    float ch = cosf(g_cam_h), sh = sinf(g_cam_h);
    float sk = g_shake;
    *cam = (e3d_camera){ .pos = { g_cam_x - ch * CAM_BACK + (frand() - 0.5f) * sk * 0.05f, CAM_UP + (frand() - 0.5f) * sk * 0.05f,
                                  g_cam_z - sh * CAM_BACK },
                         .heading = g_cam_h * DEG, .pitch = CAM_PITCH };
    if (g_icon)
        *cam = (e3d_camera){ .pos = { c->x + 4.2f, 1.9f, c->z + 4.9f }, .heading = -128.f, .pitch = -13.f };
    bg_columns(cam->heading);
    /* the sun: high and to the left (evening: low and golden), moving with the car (the shadow stays the same) */
    if (g_evening)
        *e3d_light_get(e) = (e3d_light){ .pos = { c->x - 230.f, 85.f, c->z - 120.f }, .r = 255, .g = 194, .b = 128 };
    else
        *e3d_light_get(e) = (e3d_light){ .pos = { c->x - 60.f, 300.f, c->z - 170.f }, .r = 255, .g = 246, .b = 232 };

    /* the course: chunks in front of the camera, near ones in detail */
    float cx = cam->pos[0], cz = cam->pos[2];
    e3d_mat id;
    e3d_identity(&id);
    draw_obj(e, m_lake[0], &id, -1, E3D_FLOOR);
    draw_obj(e, m_lake[1], &id, -1, E3D_FLOOR);
    for (int i = 0; i < g_nchunks; i++) {
        const chunk_t *k = &g_chunk[i];
        float dx = k->cx - cx, dz = k->cz - cz;
        float d = hypotf(dx, dz);
        if (d > 450.f + k->r)
            continue;
        if (dx * ch + dz * sh < -k->r)
            continue;            /* behind */
        int lod = d > 110.f + k->r ? 1 : 0;
        draw_obj(e, k->road[lod], &id, -1, E3D_FLOOR);
    }
    /* the car and its shadow */
    e3d_mat m, r;
    e3d_move(&m, c->x, 0.f, c->z);
    e3d_rotate(&r, 0, c->h * DEG, 0);
    e3d_mul(&m, &r);
    e3d_rotate(&r, c->roll, 0, c->pitch);
    e3d_mul(&m, &r);
    draw_obj(e, m_body, &m, -1, 0);
    static const float wp[4][2] = { { CAR_A + 0.1f, -0.78f }, { CAR_A + 0.1f, 0.78f }, { -CAR_B, -0.78f }, { -CAR_B, 0.78f } };
    for (int i = 0; i < 4; i++) {
        e3d_mat w = m;
        e3d_move(&r, wp[i][0], 0.33f, wp[i][1]);
        e3d_mul(&w, &r);
        if (i < 2) {
            e3d_rotate(&r, 0, c->steer * DEG, 0);
            e3d_mul(&w, &r);
        }
        e3d_rotate(&r, 0, 0, -c->spin);
        e3d_mul(&w, &r);
        draw_obj(e, m_wheel, &w, -1, E3D_NO_SHADOW);
    }
    /* the ghost: the best lap, as a wire outline */
    float gx, gz, gh;
    if (g_ghost_on && !g_icon && (g_state == ST_RACE || g_state == ST_COUNT) &&
        ghost_pose(g_state == ST_RACE ? g_lap_t : 0, &gx, &gz, &gh)) {
        float dx = gx - cx, dz = gz - cz;
        if (dx * dx + dz * dz < 200.f * 200.f && dx * ch + dz * sh > 0) {
            e3d_mat g = mat_at(gx, 0.f, gz, 0, gh * DEG, 0, 1.f);
            draw_obj(e, m_ghost, &g, 0x60E8FF, E3D_WIRE | E3D_DOUBLE_SIDED | E3D_UNLIT | E3D_NO_SHADOW);
        }
    }
    /* scenery (no shadows: the car's are enough) */
    for (int i = 0; i < g_nchunks; i++) {
        const chunk_t *k = &g_chunk[i];
        float dx = k->cx - cx, dz = k->cz - cz;
        float d = hypotf(dx, dz);
        if (d > 330.f + k->r || dx * ch + dz * sh < -k->r)
            continue;
        int lod = d > 70.f + k->r ? 1 : 0;
        if (k->scen[lod])
            draw_obj(e, k->scen[lod], &id, -1, E3D_NO_SHADOW);
    }
    {
        float dx = g_tr[0].x - cx, dz = g_tr[0].z - cz;
        if (hypotf(dx, dz) < 400.f && dx * ch + dz * sh > -10.f) {
            e3d_mat g = mat_at(g_tr[0].x, 0, g_tr[0].z, 0, atan2f(g_tr[0].tz, g_tr[0].tx) * DEG, 0, 1.f);
            draw_obj(e, m_gantry, &g, -1, E3D_NO_SHADOW);
        }
    }
    /* dust */
    for (int i = 0; i < MAX_DUST; i++) {
        const dust_t *p = &g_dust[i];
        if (p->life <= 0)
            continue;
        float t = p->life / p->life0;
        float s = p->size * (2.2f - 1.2f * t);
        m = mat_at(p->p[0], p->p[1], p->p[2], t * 90.f, t * 140.f, 0, s);
        draw_obj(e, m_dust, &m, (int32_t)mix_rgb(shade_rgb(p->rgb, 1.25f), p->rgb, t), E3D_FLAT | E3D_UNLIT | E3D_NO_SHADOW);
    }
}

static void text_at(uint16_t *px, int x, int y, const char *s, uint16_t col, int scale)
{
    e3d_text(px, W, W, H, x + 1, y + 1, s, 0x0000, scale);
    e3d_text(px, W, W, H, x, y, s, col, scale);
}
static int text_w(const char *s, int scale) { return (int)strlen(s) * 6 * scale - scale; }
static void text_c(uint16_t *px, int y, const char *s, uint16_t col, int scale)
{
    text_at(px, (W - text_w(s, scale)) / 2, y, s, col, scale);
}
static void text_r(uint16_t *px, int xr, int y, const char *s, uint16_t col, int scale)
{
    text_at(px, xr - text_w(s, scale), y, s, col, scale);
}

static void fill(uint16_t *px, int x, int y, int w, int h, uint16_t col)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if (i >= 0 && i < W && j >= 0 && j < H)
                px[j * W + i] = col;
}

static void shade_box(uint16_t *px, int x0, int y0, int x1, int y1, int keep)
{
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < H; y++) {
        uint16_t *r = px + y * W;
        for (int x = x0 < 0 ? 0 : x0; x < x1 && x < W; x++) {
            uint16_t c = r[x];
            int rr = (c >> 11) * keep / 8, gg = ((c >> 5) & 63) * keep / 8, bb = (c & 31) * keep / 8;
            r[x] = (uint16_t)((rr << 11) | (gg << 5) | bb);
        }
    }
}

static void frame_box(uint16_t *px, int x, int y, int w, int h, uint16_t col)
{
    fill(px, x, y, w, 1, col);
    fill(px, x, y + h - 1, w, 1, col);
    fill(px, x, y, 1, h, col);
    fill(px, x + w - 1, y, 1, h, col);
}

/* ── Pace notes ─────────────────────────────────────────────────────────────
 * The corners, found once from the course's curvature, graded like a co-driver's
 * notes: 1 (tightest) .. 6 (fast) by the smallest radius, HAIRPIN for a tight
 * turn of more than 140 degrees, LONG for one that goes on. A sign at the top
 * shows the next corner from a few seconds before it until the car is into it,
 * with the one straight after it ("into") when it follows within 40 m. */
typedef struct {
    int start, len;             /* samples */
    int8_t dir;                 /* +1 right, -1 left */
    uint8_t grade;              /* 1..6, 0 = hairpin */
    bool lng;
} note_t;
#define MAX_NOTES 48
static note_t g_note[MAX_NOTES];
static int    g_nnotes;

static void make_notes(void)
{
    static float cs[2048];
    if (g_ns > 2048)
        return;
    for (int k = 0; k < g_ns; k++) {           /* curvature smoothed over +-8 m */
        float a = 0;
        for (int d = -4; d <= 4; d++)
            a += g_tr[(k + d + g_ns) % g_ns].curv;
        cs[k] = a / 9.f;
    }
    const float on = 1.f / 200.f;              /* a radius under 200 m is a corner */
    int k0 = 0;                                /* start the scan on a straight bit */
    while (k0 < g_ns && fabsf(cs[k0]) >= on)
        k0++;
    g_nnotes = 0;
    int i = 0;
    while (i < g_ns && g_nnotes < MAX_NOTES) {
        int k = (k0 + i) % g_ns;
        if (fabsf(cs[k]) < on) {
            i++;
            continue;
        }
        int dir = cs[k] > 0 ? 1 : -1, n = 0, gap = 0, last = 0;
        float ang = 0, cmax = 0;
        /* the corner goes on while it turns the same way; short straighter bits (< 16 m) don't end it */
        while (i + n < g_ns) {
            float c = cs[(k + n) % g_ns] * (float)dir;
            if (c >= on) {
                gap = 0;
                last = n;
                ang += c * STEP;
                cmax = fmaxf(cmax, c);
            } else if (++gap > 8)
                break;
            n++;
        }
        n = last + 1;                          /* up to its last turning sample */
        i += n;
        float deg = ang * DEG, rmin = 1.f / cmax;
        if (deg < 18.f)
            continue;                          /* a kink: no note */
        note_t *t = &g_note[g_nnotes++];
        t->start = k;
        t->len = n;
        t->dir = (int8_t)dir;
        t->grade = rmin < 25.f ? 1 : rmin < 40.f ? 2 : rmin < 60.f ? 3 : rmin < 90.f ? 4 : rmin < 130.f ? 5 : 6;
        if (deg > 140.f && rmin < 35.f && (float)n * STEP / (deg * RAD) < 30.f)
            t->grade = 0;                      /* a hairpin: tight all the way round, not a long sweeper */
        t->lng = n * STEP > 70.f && deg > 60.f;
    }
    char line[200];
    int o = 0;
    for (int j = 0; j < g_nnotes && o < (int)sizeof(line) - 12; j++)
        o += snprintf(line + o, sizeof(line) - (size_t)o, " %c%d%s", g_note[j].dir > 0 ? 'R' : 'L', g_note[j].grade,
                      g_note[j].lng ? "+" : "");
    papp_log("dustline: %d pace notes:%s\n", g_nnotes, line);
}

/* the note to show now (-1 none) and the one it runs into (-1 none) */
static int note_now(int *into)
{
    const car_t *c = &g_car;
    float speed = hypotf(c->vx, c->vz);
    float look = clampf(speed * 4.5f, 100.f, 260.f) / STEP;   /* samples: about 4.5 s ahead */
    int best = -1, bd = 1 << 30;
    for (int j = 0; j < g_nnotes; j++) {
        int d = (g_note[j].start - c->idx + g_ns) % g_ns;
        if (d > g_ns / 2)
            d -= g_ns;                         /* behind: negative */
        if (d > look || d < -(int)(g_note[j].len * 0.4f) - 2)
            continue;
        if (d < bd) {
            bd = d;
            best = j;
        }
    }
    *into = -1;
    if (best >= 0) {
        const note_t *a = &g_note[best], *b = &g_note[(best + 1) % g_nnotes];
        int gap = (b->start - (a->start + a->len) + 2 * g_ns) % g_ns;
        if (g_nnotes > 1 && gap * STEP < 40.f)
            *into = (best + 1) % g_nnotes;
    }
    return best;
}

/* a thick dot, and a filled triangle (small: bounding box and edge tests) */
static void dot(uint16_t *px, float x, float y, int t, uint16_t col)
{
    fill(px, (int)(x - t * 0.5f + 0.5f), (int)(y - t * 0.5f + 0.5f), t, t, col);
}
static void tri_fill(uint16_t *px, float ax, float ay, float bx, float by, float cx, float cy, uint16_t col)
{
    int x0 = (int)fminf(ax, fminf(bx, cx)), x1 = (int)fmaxf(ax, fmaxf(bx, cx)) + 1;
    int y0 = (int)fminf(ay, fminf(by, cy)), y1 = (int)fmaxf(ay, fmaxf(by, cy)) + 1;
    float area = (bx - ax) * (cy - ay) - (by - ay) * (cx - ax);
    if (fabsf(area) < 0.5f)
        return;
    for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < H; y++)
        for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < W; x++) {
            float px_ = (float)x + 0.5f, py = (float)y + 0.5f;
            float w0 = (bx - px_) * (cy - py) - (by - py) * (cx - px_);
            float w1 = (cx - px_) * (ay - py) - (cy - py) * (ax - px_);
            float w2 = (ax - px_) * (by - py) - (ay - py) * (bx - px_);
            if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0))
                px[y * W + x] = col;
        }
}

/* the arrow: up from the bottom, then bending by the corner's angle, an arrow head at the end */
static void note_arrow(uint16_t *px, int x0, int y0, int sz, const note_t *n, uint16_t col)
{
    static const float bend[7] = { 180.f, 110.f, 90.f, 70.f, 55.f, 40.f, 25.f };   /* by grade, 0 = hairpin */
    float th = bend[n->grade] * RAD, rad = n->grade == 0 ? sz * 0.22f : sz * 0.34f;
    float x = x0 + sz * 0.5f - (n->grade == 0 ? n->dir * rad : 0), y = (float)(y0 + sz - 2);
    float stem = n->grade == 0 ? sz * 0.42f : sz * 0.3f;
    int t = sz / 7 > 3 ? sz / 7 : 3;
    for (float u = 0; u < stem; u += 1.f)
        dot(px, x, y - u, t, col);
    y -= stem;
    /* the arc: heading up (-y), turning towards dir */
    float h = 0, steps = th * rad;
    for (float u = 0; u < steps; u += 1.f) {
        h = u / rad;
        dot(px, x + n->dir * (rad - rad * cosf(h)), y - rad * sinf(h), t, col);
    }
    h = th;
    float ex = x + n->dir * (rad - rad * cosf(h)), ey = y - rad * sinf(h);
    float dx = n->dir * sinf(h), dy = -cosf(h);           /* the direction at the end */
    float hl = sz * 0.3f, hw = sz * 0.22f;
    tri_fill(px, ex + dx * hl, ey + dy * hl, ex - dy * hw, ey + dx * hw, ex + dy * hw, ey - dx * hw, col);
}

/* the co-driver's call when a sign shows: a glide up for right, down for left, then the grade
   (lower = tighter; tight corners and hairpins twice) */
static void note_beep(const note_t *n)
{
    float a = n->dir > 0 ? 700.f : 950.f, b = n->dir > 0 ? 950.f : 700.f;
    sfx(a, b, 110, WAVE_TRI, 0.16f);
    float g = n->grade == 0 ? 400.f : 480.f + 90.f * (float)n->grade;
    sfx_at(g, g, 90, WAVE_TRI, 0.16f, 150);
    if (n->grade <= 1)
        sfx_at(g, g, 90, WAVE_TRI, 0.16f, 270);
}

static int s_note_shown = -1;
static void draw_note(uint16_t *px)
{
    int into, j = note_now(&into);
    if (j != s_note_shown && j >= 0)
        note_beep(&g_note[j]);
    s_note_shown = j;
    if (j < 0)
        return;
    const note_t *n = &g_note[j];
    uint16_t col = n->grade <= 1 ? 0xF904 : n->grade <= 3 ? 0xFD20 : 0xFFE0;
    int bw = 96, bh = 46, bx = W / 2 - bw / 2, by = 4;
    if (into >= 0)
        bx -= 26;
    shade_box(px, bx, by, bx + bw, by + bh, 3);
    frame_box(px, bx, by, bw, bh, col);
    note_arrow(px, bx + 4, by + 5, 36, n, col);
    char num[4];
    snprintf(num, sizeof(num), "%d", n->grade);
    const char *word = n->grade == 0 ? "HAIRPIN" : n->dir > 0 ? "RIGHT" : "LEFT";
    if (n->grade == 0)
        text_at(px, bx + 46, by + 8, n->dir > 0 ? "R" : "L", col, 3);
    else
        text_at(px, bx + 50, by + 6, num, col, 3);
    text_at(px, bx + 44, by + 30, word, 0xFFFF, 1);
    if (n->lng)
        text_at(px, bx + 70, by + 10, "LONG", 0xFFFF, 1);
    if (into >= 0) {
        const note_t *m = &g_note[into];
        uint16_t c2 = m->grade <= 1 ? 0xF904 : m->grade <= 3 ? 0xFD20 : 0xFFE0;
        int x2 = bx + bw + 4;
        shade_box(px, x2, by + 8, x2 + 48, by + bh - 4, 3);
        frame_box(px, x2, by + 8, 48, bh - 12, c2);
        text_at(px, x2 + 3, by + 11, "INTO", 0xC618, 1);
        note_arrow(px, x2 + 2, by + 18, 22, m, c2);
        if (m->grade == 0)
            text_at(px, x2 + 27, by + 22, "HP", c2, 1);
        else {
            snprintf(num, sizeof(num), "%d", m->grade);
            text_at(px, x2 + 28, by + 19, num, c2, 2);
        }
    }
}

/* the course from above, top right */
#define MAP_W 74
#define MAP_H 62
static uint8_t s_map[MAP_H][MAP_W];
static float s_map_k, s_map_x0, s_map_z0;

static void map_xy(float x, float z, int *mx, int *my)
{
    /* x (into the screen at the start) points up on the map, z to the right */
    *mx = (int)((z - s_map_z0) * s_map_k);
    *my = MAP_H - 1 - (int)((x - s_map_x0) * s_map_k);
}

static void make_map(void)
{
    float sx = g_bx1 - g_bx0, sz = g_bz1 - g_bz0;
    s_map_k = fminf((MAP_W - 6) / sz, (MAP_H - 6) / sx);
    s_map_x0 = g_bx0 - ((MAP_H - 1) / s_map_k - sx) * 0.5f;
    s_map_z0 = g_bz0 - ((MAP_W - 1) / s_map_k - sz) * 0.5f;
    memset(s_map, 0, sizeof(s_map));
    for (int k = 0; k < g_ns; k++) {
        int mx, my;
        map_xy(g_tr[k].x, g_tr[k].z, &mx, &my);
        if (mx >= 0 && my >= 0 && mx < MAP_W && my < MAP_H)
            s_map[my][mx] = (uint8_t)(1 + g_tr[k].surf);
    }
}

static void draw_map(uint16_t *px, int x0, int y0)
{
    static const uint16_t col[NSURF] = { 0xDEFB, 0xD5AE, 0xBDD7, 0, 0, 0xFFFF, 0x9E9F, 0xF6AE, 0xE5F2 };
    shade_box(px, x0 - 2, y0 - 2, x0 + MAP_W + 2, y0 + MAP_H + 2, 4);
    for (int y = 0; y < MAP_H; y++)
        for (int x = 0; x < MAP_W; x++)
            if (s_map[y][x])
                px[(y0 + y) * W + x0 + x] = col[s_map[y][x] - 1];
    int mx, my;
    map_xy(g_tr[0].x, g_tr[0].z, &mx, &my);
    fill(px, x0 + mx - 1, y0 + my, 3, 1, 0xFFFF);
    map_xy(g_car.x, g_car.z, &mx, &my);
    fill(px, x0 + mx - 1, y0 + my - 1, 3, 3, 0xF800);
}

static void draw_touch(uint16_t *px, const actions_t *a)
{
    struct { int x, y, w, h; const char *s; bool on; } b[4] = {
        { TBR_X, TB_Y, 48, TB_H, "<", s_touch_dig < 0 }, { W - 114, TB_Y, 48, TB_H, ">", s_touch_dig > 0 },
        { W - 62, TB_Y, 58, TB_H, "BRAKE", a->d.brake > 0 }, { W - 62, REV_Y, 58, 36, "REV", a->d.rev > 0 } };
    for (int i = 0; i < 4; i++) {
        shade_box(px, b[i].x, b[i].y, b[i].x + b[i].w, b[i].y + b[i].h, b[i].on ? 8 : 5);
        frame_box(px, b[i].x, b[i].y, b[i].w, b[i].h, b[i].on ? 0xFE60 : 0x8410);
        uint16_t col = b[i].on ? 0xFE60 : 0xE71C;
        if (i < 2) {                      /* the font has no < >: a triangle */
            int mx = b[i].x + b[i].w / 2, my = b[i].y + b[i].h / 2;
            for (int k = 0; k < 13; k++) {
                int hh = i == 0 ? k : 12 - k;
                fill(px, mx - 6 + k, my - hh, 1, 2 * hh + 1, col);
            }
        } else
            text_at(px, b[i].x + (b[i].w - text_w(b[i].s, 1)) / 2, b[i].y + (b[i].h - 7) / 2, b[i].s, col, 1);
    }
    /* the gas slider: the bar fills from the bottom, the handle is the lever */
    int h = GS_Y1 - GS_Y0, lv = (int)(s_gas_lever * (float)(h - 16)) + 8;
    shade_box(px, GS_X, GS_Y0, GS_X + GS_W, GS_Y1, 5);
    int fh = (int)(g_gas_show * (float)(h - 16)) + 8;
    if (g_gas_show > 0)
        fill(px, GS_X + 2, GS_Y1 - fh, GS_W - 4, fh - 1, 0x2C64);
    frame_box(px, GS_X, GS_Y0, GS_W, h, 0x8410);
    for (int q = 1; q < 4; q++)
        fill(px, GS_X + 1, GS_Y1 - 8 - q * (h - 16) / 4, 6, 1, 0x8410);
    int hy = GS_Y1 - lv;
    shade_box(px, GS_X - 2, hy - 6, GS_X + GS_W + 2, hy + 6, 8);
    frame_box(px, GS_X - 2, hy - 6, GS_W + 4, 12, 0xFE60);
    text_at(px, GS_X + (GS_W - text_w("GAS", 1)) / 2, GS_Y0 + 4, "GAS", 0xE71C, 1);
}

static void draw_hud(uint16_t *px, const actions_t *act, float fps)
{
    char line[64], t1[16], t2[16];
    const car_t *c = &g_car;
    if (g_state == ST_TITLE) {
        shade_box(px, 0, 40, W, 176, 4);
        text_c(px, 54, "DUSTLINE", 0xFFFF, 5);
        text_c(px, 96, "A RALLY STAGE AGAINST THE CLOCK", 0xFE60, 1);
        snprintf(line, sizeof(line), "STAGE  %s", g_st->name);
        text_c(px, 106, line, 0xFFFF, 1);
        if (((int)(g_t * 2.f)) & 1)
            text_c(px, 116, "PRESS A OR TAP TO RACE", 0xFFFF, 1);
        if (g_ctrl == CTRL_TWIN) {
            text_c(px, 134, "RIGHT STICK STEERS   LEFT STICK UP GAS   DOWN BRAKE", 0xA534, 1);
            text_c(px, 144, "A BRAKE  X HANDBRAKE  Y BACK ON ROAD  D-PAD DOWN REVERSE", 0xA534, 1);
        } else if (g_ctrl == CTRL_PAD) {
            text_c(px, 134, "LEFT/RIGHT OR STICK STEERS   R2 OR UP GAS   A BRAKE", 0xA534, 1);
            text_c(px, 144, "X HANDBRAKE  Y BACK ON ROAD  DOWN REVERSE  START PAUSE", 0xA534, 1);
        } else {
            text_c(px, 134, "ROLLER STEERS   GAS SLIDER OR R2   A BRAKE   DOWN REVERSE", 0xA534, 1);
            text_c(px, 144, "X HANDBRAKE   Y BACK ON THE ROAD   START PAUSE", 0xA534, 1);
        }
        fmt_time(t1, sizeof(t1), g_best_lap);
        fmt_time(t2, sizeof(t2), g_best_race);
        snprintf(line, sizeof(line), "BEST LAP %s   BEST RACE %s", t1, t2);
        text_c(px, 158, line, 0x9FFF, 1);
        if (g_save_lap >= 0) {
            snprintf(line, sizeof(line), "SAVED RACE  LAP %d", g_save_lap + 1);
            text_c(px, 168, line, 0x07E0, 1);
        }
        return;
    }
    /* times, top left */
    shade_box(px, 0, 0, 128, 40, 5);
    snprintf(line, sizeof(line), "LAP %d/%d", g_lap < g_laps ? g_lap + 1 : g_laps, g_laps);
    text_at(px, 4, 3, line, 0xFFE0, 1);
    fmt_time(t1, sizeof(t1), g_race_t);
    text_at(px, 52, 3, t1, 0xFFFF, 1);
    fmt_time(t1, sizeof(t1), g_lap_t);
    snprintf(line, sizeof(line), "LAP  %s", t1);
    text_at(px, 4, 14, line, 0xFFFF, 1);
    fmt_time(t1, sizeof(t1), g_best_lap);
    snprintf(line, sizeof(line), "BEST %s", t1);
    text_at(px, 4, 25, line, 0x9FFF, 1);
    /* map, top right */
    draw_map(px, W - MAP_W - 4, 4);
    text_at(px, W - MAP_W - 18, 4, "II", 0xC618, 1);
    /* speed, gear and the steering, bottom centre */
    int kmh = (int)(hypotf(c->vx, c->vz) * 3.6f + 0.5f);
    int cx = s_touch_used && g_state != ST_FINISH ? GS_X + GS_W + 104 : W / 2;   /* clear of the slider and buttons */
    shade_box(px, cx - 92, H - 36, cx + 54, H - 2, 5);
    snprintf(line, sizeof(line), "%d", kmh);
    text_r(px, cx + 14, H - 31, line, 0xFFFF, 3);
    text_at(px, cx + 18, H - 31, "KM/H", 0xBDF7, 1);
    snprintf(line, sizeof(line), "%c", c->vf < -0.5f && act->d.rev > 0 ? 'R' : (char)('0' + c->gear));
    text_at(px, cx + 18, H - 21, line, 0xFE60, 2);
    text_at(px, cx - 88, H - 31, c->surf == SURF_GRASS ? g_st->offroad : surf_name[c->surf],
            c->surf == SURF_TARMAC ? 0xC618 : 0xFD20, 1);
    /* steering bar */
    int bx = cx - 50, by = H - 8, bw = 100;
    fill(px, bx, by, bw, 3, 0x4208);
    fill(px, bx + bw / 2, by - 1, 1, 5, 0x8410);
    int sx = bx + bw / 2 + (int)(g_steer_show * (float)(bw / 2 - 2));
    fill(px, sx - 2, by - 1, 5, 5, 0xFE60);
    if (g_hand_held && g_state == ST_RACE)
        text_at(px, cx - 88, H - 21, "HANDBRAKE", 0xF800, 1);

    if (s_touch_used && g_state != ST_FINISH)
        draw_touch(px, act);

    if (g_state == ST_RACE && g_notes_on)
        draw_note(px);
    if (g_state == ST_COUNT) {
        int n = 3 - (int)g_state_t;
        snprintf(line, sizeof(line), "%d", n);
        text_c(px, 70, line, 0xFFE0, 6);
        if (g_lap == 0 && g_race_t == 0)
            text_c(px, 120, g_ctrl == CTRL_ROLLER ? "HOLD THE ROLLER STRAIGHT" : "GET READY", 0xFFFF, 1);
        else {
            snprintf(line, sizeof(line), "LAP %d", g_lap + 1);
            text_c(px, 120, line, 0xFFFF, 2);
        }
    } else if (g_state == ST_RACE && g_state_t < 1.f)
        text_c(px, 70, "GO", 0x07E0, 6);
    if (g_split_t > 0 && g_state == ST_RACE) {
        fmt_time(t1, sizeof(t1), g_last_lap_t);
        snprintf(line, sizeof(line), "LAP %s%s", t1, g_new_lap_rec && g_last_lap_t == g_best_lap ? " BEST" : "");
        text_c(px, 56, line, 0x07FF, 2);
        if (g_lap == g_laps - 1)
            text_c(px, 78, "FINAL LAP", 0xFFE0, 2);
    }
    if (g_delta_t > 0 && (g_state == ST_RACE || g_state == ST_FINISH)) {
        /* against the ghost: green ahead, red behind */
        float d = g_delta;
        int cs = (int)(fabsf(d) * 100.f + 0.5f);
        snprintf(line, sizeof(line), "%c%d.%02d", d < 0 ? '-' : '+', cs / 100, cs % 100);
        shade_box(px, 0, 40, 74, 60, 5);
        text_at(px, 4, 43, line, d < 0 ? 0x07E0 : 0xF9A6, 2);
    }
    if (g_wrong_t > 1.f && ((int)(g_t * 3.f) & 1))
        text_c(px, 96, "WRONG WAY", 0xF800, 3);
    if (g_state == ST_FINISH) {
        shade_box(px, 60, 46, W - 60, 182, 3);
        text_c(px, 54, "FINISH", 0xFFE0, 4);
        fmt_time(t1, sizeof(t1), g_race_t);
        snprintf(line, sizeof(line), "TIME %s", t1);
        text_c(px, 92, line, 0xFFFF, 2);
        for (int i = 0; i < g_laps && i < 5; i++) {
            fmt_time(t1, sizeof(t1), g_lap_times[i]);
            snprintf(line, sizeof(line), "LAP %d  %s", i + 1, t1);
            text_c(px, 114 + i * 10, line, 0xC618, 1);
        }
        if (g_new_race_rec)
            text_c(px, 150, "NEW RECORD", 0x07E0, 2);
        else if (g_new_lap_rec)
            text_c(px, 150, "NEW BEST LAP", 0x07E0, 2);
        if (g_state_t > 2.f)
            text_c(px, 170, "PRESS A OR TAP", 0xBDF7, 1);
    }
    if (g_msg_t > 0 && g_msg)
        text_c(px, 140, g_msg, 0x07FF, 2);
    if (g_show_fps) {
        snprintf(line, sizeof(line), "%.0f FPS", (double)fps);
        text_at(px, 134, 3, line, 0x07E0, 1);
    }
}

/* ── Meshes ──────────────────────────────────────────────────────────────── */
static bool make_meshes(void)
{
    /* the body: a rally hatchback. Lower body and cabin from side profiles; red with a white roof,
       a white and yellow band along the doors, door number panels, wheel arch flares, a light pod
       on the nose, a bonnet vent, a roof vent and a big rear wing */
    b_reset();
    {
        const uint32_t BODY = 0xC81E28, WHITE = 0xF2F2F0, YEL = 0xF6BE1C, DARK = 0x1C1E24, GLASS = 0x18263A;
        static const float low[][2] = { { 2.08f, 0.26f }, { 2.16f, 0.60f }, { 1.30f, 0.86f }, { -1.85f, 0.92f },
                                        { -2.04f, 0.68f }, { -1.98f, 0.26f } };
        uint32_t ls[6] = { DARK, BODY, BODY, BODY, DARK, DARK };
        b_prism(low, 6, -0.86f, 0.86f, ls, BODY);
        static const float cab[][2] = { { 1.30f, 0.86f }, { 0.30f, 1.36f }, { -1.30f, 1.38f }, { -1.85f, 0.92f } };
        uint32_t cs[4] = { GLASS, WHITE, GLASS, GLASS };
        b_prism(cab, 4, -0.74f, 0.74f, cs, GLASS);
        /* the pillar between the side windows, and the sills under them */
        b_box(-0.42f, 0.9f, -0.76f, -0.26f, 1.36f, 0.76f, BODY);
        /* bands along the doors: white, then a thin yellow one */
        b_box(-1.30f, 0.34f, -0.88f, 1.10f, 0.48f, 0.88f, WHITE);
        b_box(-1.30f, 0.48f, -0.88f, 1.10f, 0.53f, 0.88f, YEL);
        /* door number panels */
        b_box(-0.50f, 0.58f, -0.875f, 0.20f, 0.84f, 0.875f, WHITE);
        b_box(-0.30f, 0.64f, -0.88f, -0.24f, 0.78f, 0.88f, DARK);
        b_box(-0.05f, 0.64f, -0.88f, 0.01f, 0.78f, 0.88f, DARK);
        /* wheel arch flares */
        for (int i = 0; i < 2; i++) {
            float x = i ? -CAR_B : CAR_A + 0.1f;
            b_box(x - 0.48f, 0.56f, -0.96f, x + 0.48f, 0.70f, -0.84f, BODY);
            b_box(x - 0.48f, 0.56f, 0.84f, x + 0.48f, 0.70f, 0.96f, BODY);
        }
        /* bonnet vent, roof vent (low and dark) */
        b_box(1.45f, 0.70f, -0.30f, 1.80f, 0.76f, 0.30f, DARK);
        b_box(0.05f, 1.36f, -0.22f, 0.40f, 1.42f, 0.22f, DARK);
        /* rear wing: a wide plate on two struts, yellow end plates */
        b_box(-2.08f, 1.40f, -0.82f, -1.66f, 1.46f, 0.82f, BODY);
        b_box(-2.10f, 1.30f, -0.86f, -1.64f, 1.52f, -0.82f, YEL);
        b_box(-2.10f, 1.30f, 0.82f, -1.64f, 1.52f, 0.86f, YEL);
        b_box(-1.92f, 0.92f, -0.55f, -1.86f, 1.40f, -0.47f, DARK);
        b_box(-1.92f, 0.92f, 0.47f, -1.86f, 1.40f, 0.55f, DARK);
        /* light pod on the nose: a dark bar with four lamps */
        b_box(2.10f, 0.56f, -0.74f, 2.18f, 0.80f, 0.74f, DARK);
        for (int i = 0; i < 4; i++) {
            float z = -0.54f + 0.36f * (float)i;
            b_box(2.18f, 0.60f, z - 0.13f, 2.24f, 0.76f, z + 0.13f, 0xFFF4C0);
        }
        /* the rear: a dark panel, bright tail lights, a number plate */
        b_box(-2.06f, 0.58f, -0.84f, -2.01f, 0.80f, 0.84f, DARK);
        b_box(-2.08f, 0.61f, -0.80f, -2.04f, 0.77f, -0.48f, 0xFF4A1E);
        b_box(-2.08f, 0.61f, 0.48f, -2.04f, 0.77f, 0.80f, 0xFF4A1E);
        b_box(-2.05f, 0.36f, -0.26f, -2.00f, 0.50f, 0.26f, WHITE);
        /* mud flaps */
        b_box(-1.76f, 0.10f, -0.88f, -1.71f, 0.44f, -0.60f, 0x2A2A2E);
        b_box(-1.76f, 0.10f, 0.60f, -1.71f, 0.44f, 0.88f, 0x2A2A2E);
        b_box(0.62f, 0.10f, -0.88f, 0.67f, 0.36f, -0.62f, 0x2A2A2E);
        b_box(0.62f, 0.10f, 0.62f, 0.67f, 0.36f, 0.88f, 0x2A2A2E);
    }
    m_body = b_bake(25.f);
    /* a wheel: a cylinder across z, tread in two shades so it shows turning */
    b_reset();
    {
        const int seg = 10;
        const float r = 0.33f, hw = 0.13f;
        int base = B.nv;
        for (int i = 0; i < seg; i++) {
            float a = 6.2832f * (float)i / (float)seg;
            b_v(cosf(a) * r, sinf(a) * r, -hw);
            b_v(cosf(a) * r, sinf(a) * r, hw);
        }
        int c0 = b_v(0, 0, -hw - 0.02f), c1 = b_v(0, 0, hw + 0.02f);
        float c[3] = { 0, 0, 0 };
        for (int i = 0; i < seg; i++) {
            int j = (i + 1) % seg;
            b_quad(base + 2 * i, base + 2 * i + 1, base + 2 * j + 1, base + 2 * j, (i & 1) ? 0x202020 : 0x303030, c);
            b_tri(base + 2 * i, base + 2 * j, c0, (i & 2) ? 0xB0B0B8 : 0x707078, c);
            b_tri(base + 2 * i + 1, base + 2 * j + 1, c1, (i & 2) ? 0xB0B0B8 : 0x707078, c);
        }
    }
    m_wheel = b_bake(40.f);
    /* a dust puff: an octahedron */
    b_reset();
    {
        int v0 = B.nv;
        b_v(1, 0, 0);
        b_v(-1, 0, 0);
        b_v(0, 1, 0);
        b_v(0, -1, 0);
        b_v(0, 0, 1);
        b_v(0, 0, -1);
        float c[3] = { 0, 0, 0 };
        for (int a = 0; a < 2; a++)
            for (int b = 2; b < 4; b++)
                for (int d = 4; d < 6; d++)
                    b_tri(v0 + a, v0 + b, v0 + d, 0xFFFFFF, c);
    }
    m_dust = b_bake(1.f);
    /* the ghost: the two body prisms only (wireframe costs by the edge) */
    b_reset();
    {
        static const float low[][2] = { { 2.05f, 0.30f }, { 2.12f, 0.62f }, { 1.30f, 0.86f }, { -1.85f, 0.92f },
                                        { -2.02f, 0.66f }, { -1.95f, 0.30f } };
        static const float cab[][2] = { { 1.30f, 0.86f }, { 0.35f, 1.36f }, { -1.25f, 1.38f }, { -1.85f, 0.92f } };
        uint32_t c6[6] = { 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF, 0xFFFFFF };
        b_prism(low, 6, -0.86f, 0.86f, c6, 0xFFFFFF);
        b_prism(cab, 4, -0.74f, 0.74f, c6, 0xFFFFFF);
    }
    m_ghost = b_bake(1.f);
    return m_body && m_wheel && m_dust && m_ghost;
}

static void stage_free(void);
static void free_meshes(void)
{
    stage_free();
    e3d_mesh_free(m_body);
    e3d_mesh_free(m_ghost);
    e3d_mesh_free(m_wheel);
    e3d_mesh_free(m_dust);
}

/* ── Presenting: a thread scales one frame with the PPA while the next is drawn ── */
static papp_surface_t *g_surf;
static papp_sem_t *g_pres_go, *g_pres_done;
static const uint16_t *volatile g_pres_buf;

static int presenter_main(void *arg)
{
    for (;;) {
        if (!papp_sem_take(g_pres_go, 100))
            continue;
        const uint16_t *buf = g_pres_buf;
        if (!buf)
            break;
        papp_disp_present_ext(g_surf, buf);
        papp_sem_give(g_pres_done);
    }
    return 0;
}

static int menu(const char *title, const char *const *items, int n)
{
    papp_sem_take(g_pres_done, PAPP_WAIT_FOREVER);   /* nothing is being presented now */
    s_eng_vol = s_skid_vol = s_grit_vol = 0;
    int r = papp_ui_menu(title, items, n, 0);
    papp_sem_give(g_pres_done);
    papp_input_flush();
    s_prev_btn = ~0u;          /* ignore the buttons still held */
    return r;
}

/* returns false to exit */
static bool pause_menu(void)
{
    /* papp_ui_menu shows 7 items at most; a saved race is loaded by Continue on the title */
    enum { M_RESUME, M_GHOST, M_EVENING, M_SAVE, M_SAVE_EXIT, M_RESTART, M_EXIT };
    const char *const items[] = { "Resume", g_ghost_on ? "Ghost: on" : "Ghost: off",
                                  g_evening ? "Evening: on" : "Evening: off", "Save race", "Save and exit",
                                  "Restart race", "Exit" };
    int r = menu("Dustline", items, 7);
    if (r == M_EVENING) {
        g_evening = !g_evening;
        g_look_dirty = true;
        settings_io(true);
    } else if (r == M_GHOST) {
        g_ghost_on = !g_ghost_on;
        note(g_ghost_on ? "GHOST ON" : "GHOST OFF");
    } else if (r == M_SAVE || r == M_SAVE_EXIT) {
        bool ok = save_race();
        note(ok ? "RACE SAVED" : (g_state != ST_RACE && g_state != ST_COUNT) ? "NOTHING TO SAVE" : "COULD NOT SAVE");
        if (ok && r == M_SAVE_EXIT)
            return false;
    } else if (r == M_RESTART)
        new_race();
    return r != M_EXIT && !papp_exit_requested();
}

/* ── Building a stage ─────────────────────────────────────────────────────
 * The course, its scenery meshes, background, notes and map; switching frees the old ones
 * and builds the new stage in place (about 0.8 s). */
static e3d_t *g_e;

static bool stage_build(void)
{
    if (!b_init()) {
        b_free();
        return false;
    }
    build_track();
    place_props();
    bool grid = g_gstart && g_gidx, ok = grid && build_chunks();
    b_free();
    if (!ok) {
        papp_log("dustline: stage %s: %s failed (grid %dx%d, %d props)\n", g_st->name,
                 grid ? "meshes" : "props grid", g_gw, g_gh, g_nprops);
        return false;
    }
    make_background();
    make_notes();
    make_map();
    return true;
}

static void stage_free(void)
{
    for (int i = 0; i < g_nchunks; i++)
        for (int l = 0; l < 2; l++) {
            e3d_mesh_free(g_chunk[i].road[l]);
            e3d_mesh_free(g_chunk[i].scen[l]);
            g_chunk[i].road[l] = g_chunk[i].scen[l] = NULL;
        }
    g_nchunks = 0;
    e3d_mesh_free(m_gantry);
    e3d_mesh_free(m_lake[0]);
    e3d_mesh_free(m_lake[1]);
    m_gantry = m_lake[0] = m_lake[1] = NULL;
    papp_free(g_gstart);
    papp_free(g_gidx);
    g_gstart = NULL;
    g_gidx = NULL;
}

/* the records, ghost and saved race of the stage */
static void stage_files(void)
{
    g_best_lap = g_best_race = 0;
    g_nghost = 0;
    g_ghost_t = 0;
    records_io(false);
    ghost_io(false);
    save_peek();
}

static bool stage_switch(int st)
{
    int64_t t0 = papp_time_us();
    stage_free();
    g_stage = st;
    g_st = &k_stage[st];
    bool ok = stage_build();
    if (g_e)
        apply_look(g_e);
    stage_files();
    race_reset();
    g_state = ST_TITLE;
    g_state_t = 0;
    papp_log("dustline: stage %s built in %d ms\n", g_st->name, (int)((papp_time_us() - t0) / 1000));
    return ok;
}

static void start_menu(void)
{
    char cont[32], stg[40];
    snprintf(cont, sizeof(cont), "Continue (lap %d)", g_save_lap + 1);
    snprintf(stg, sizeof(stg), "Stage: %s", g_st->name);
    char ctl[40];
    snprintf(ctl, sizeof(ctl), "Controls: %s", ctrl_name[g_ctrl]);
    const char *items[4];
    int n = 0, i_cont = -1;
    if (g_save_lap >= 0) {
        i_cont = n;
        items[n++] = cont;
    }
    int i_new = n;
    items[n++] = g_save_lap >= 0 ? "New race" : "Start race";
    int i_stage = n;
    items[n++] = stg;
    int i_ctl = n;
    items[n++] = ctl;
    int r = menu("Dustline", items, n);
    if (r == i_cont) {
        if (!load_race()) {
            note("SAVE DAMAGED");
            new_race();
        }
    } else if (r == i_new)
        new_race();
    else if (r == i_stage) {
        const char *names[NSTAGES];
        for (int i = 0; i < NSTAGES; i++)
            names[i] = k_stage[i].name;
        int c = menu("Stage", names, NSTAGES);
        if (c >= 0 && c < NSTAGES && c != g_stage) {
            if (!stage_switch(c)) {
                note("OUT OF MEMORY");
                if (c != 0)
                    stage_switch(0);
            }
            settings_io(true);
        }
    } else if (r == i_ctl) {
        int c = menu("Controls", ctrl_name, NCTRL_MODES);
        if (c >= 0 && c < NCTRL_MODES && c != g_ctrl) {
            g_ctrl = c;
            s_touch_used = c == CTRL_ROLLER;     /* the touch controls show again on a touch */
            settings_io(true);
        }
    }
}

int main(int argc, char **argv)
{
    settings_io(false);
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "auto"))
            g_auto = true;
        else if (!strcmp(argv[i], "fps"))
            g_show_fps = true;
        else if (!strcmp(argv[i], "evening"))
            g_evening = true;
        else if (!strcmp(argv[i], "day"))
            g_evening = false;
        else if (!strcmp(argv[i], "nofog"))
            g_nofog = true;
        else if (!strcmp(argv[i], "rec"))
            g_auto_rec = true;
        else if (!strcmp(argv[i], "icon"))
            g_icon = true;
        else if (!strcmp(argv[i], "stage") && i + 1 < argc) {
            int v = atoi(argv[++i]);
            if (v >= 0 && v < NSTAGES) {
                g_stage = v;
                g_st = &k_stage[v];
            }
        } else if (!strcmp(argv[i], "laps") && i + 1 < argc)
            g_laps = (int)clampf((float)atoi(argv[++i]), 1, 9);
    }
    srand((unsigned)papp_time_us());
    if (g_ctrl < 0)
        g_ctrl = papp_input_analog(PAPP_ANALOG_PADDLE) >= 0 ? CTRL_ROLLER : CTRL_PAD;
    s_touch_used = g_ctrl == CTRL_ROLLER;

    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 1 };
    g_surf = papp_disp_surface_create(&cfg);
    uint16_t *fb[2];
    for (int i = 0; i < 2; i++)
        fb[i] = papp_alloc(sizeof(uint16_t) * W * H, PAPP_MEM_LARGE | PAPP_MEM_DMA, 64);
    g_pres_go = papp_sem_create(0, 1);
    g_pres_done = papp_sem_create(1, 1);
    papp_thread_cfg_t pcfg = { .name = "dl_present", .stack_size = 4096, .prio = PAPP_PRIO_HIGH, .core = 0 };
    papp_thread_t *pres = (g_surf && fb[0] && fb[1] && g_pres_go && g_pres_done)
                              ? papp_thread_create(presenter_main, NULL, &pcfg) : NULL;
    e3d_t *e = e3d_create(W, H, 2);
    int64_t t0 = papp_time_us();
    g_e = e;
    bool ok = pres && e && b_init();
    if (ok)
        ok = make_meshes();
    b_free();
    ok = ok && stage_build();
    if (!ok) {
        papp_log("dustline: out of memory\n");
        return 1;
    }
    papp_log("dustline: %s built in %d ms, %d chunks\n", g_st->name, (int)((papp_time_us() - t0) / 1000), g_nchunks);
    e3d_set_focal(e, FOCAL);
    e3d_set_background_rows(e, sky_row, NULL);
    e3d_params *par = e3d_params_get(e);
    par->shadows = true;
    par->shadow_y = 0.02f;
    par->shininess = 16.f;
    apply_look(e);

    g_snd_lock = papp_mutex_create(false);
    papp_audio_cfg_t acfg = { .rate = SR, .channels = 1, .format = PAPP_AUDIO_S16, .latency_ms = 60 };
    g_snd = g_snd_lock ? papp_audio_open(&acfg) : NULL;
    papp_thread_cfg_t acfg_t = { .name = "dl_audio", .stack_size = 4096, .prio = PAPP_PRIO_HIGH, .core = 1 };
    papp_thread_t *aud = g_snd ? papp_thread_create(audio_main, NULL, &acfg_t) : NULL;
    if (!aud && g_snd) {
        papp_audio_close(g_snd);
        g_snd = NULL;
    }

    stage_files();
    race_reset();
    g_state = ST_TITLE;
    if (g_auto)
        new_race();
    if (g_icon) {
        g_state = ST_RACE;
        g_car.idx = 100;
        car_place(&g_car, 100, 0);
        g_car.steer = 0.3f;
        g_car.roll = -3.f;
        for (int i = 0; i < 25; i++) {
            float a = frand() * 6.2832f, d = 1.f + frand() * 4.f;
            spawn_dust(g_car.x - cosf(g_car.h) * (2.f + d) + cosf(a), 0.3f + frand(), g_car.z - sinf(g_car.h) * (2.f + d) + sinf(a), 0,
                       0, 0xB89870, 0.6f + frand() * 0.4f);
        }
    }
    papp_log("dustline: running%s, %d laps\n", g_auto ? " (auto)" : "", g_laps);

    int cur = 0, frames = 0;
    int64_t last = papp_time_us(), stat_t = last, log_t = last;
    float fps = 0;
    bool run = true;
    /* performance per lap: average fps, the worst 1 s, render times (the log line at each lap) */
    int pf_n = 0, pf_lap = -1;
    double pf_t = 0, pf_geom = 0, pf_rast = 0;
    float pf_worst = 999.f;
    int pf_max = 0;
    while (run && !papp_exit_requested()) {
        int64_t now = papp_time_us();
        float dt = (float)(now - last) * 1e-6f;
        float dt_raw = dt;
        last = now;
        if (dt > 0.05f)
            dt = 0.05f;
        if (g_msg_t > 0)
            g_msg_t -= dt;

        if (g_look_dirty) {
            apply_look(e);
            g_look_dirty = false;
        }
        actions_t act = read_input(g_surf, dt);
        if (act.pause && g_state != ST_TITLE) {
            run = pause_menu();
            last = papp_time_us();
            continue;
        }
        bool go = act.start || act.tap;
        if (g_state == ST_TITLE) {
            if (go) {
                start_menu();
                last = papp_time_us();
                continue;
            } else if (act.pause)
                run = pause_menu();
        } else if (g_state == ST_FINISH && go && g_state_t > 2.f) {
            g_state = ST_TITLE;
            g_state_t = 0;
            race_reset();
        }
        if (g_icon)
            dt = 0;
        update(&act, dt);

        uint16_t *px = fb[cur];
        e3d_begin(e, px, W);
        draw_scene(e);
        e3d_end(e);
        if (!g_icon)
            draw_hud(px, &act, fps);

        frames++;
        if (now - stat_t >= 1000000) {
            fps = (float)frames * 1e6f / (float)(now - stat_t);
            frames = 0;
            stat_t = now;
            if (g_state == ST_RACE && pf_n > 30)
                pf_worst = fminf(pf_worst, fps);
        }
        if (g_state == ST_RACE && pf_lap == g_lap) {
            const e3d_stats *st = e3d_get_stats(e);
            pf_n++;
            pf_t += dt_raw;
            pf_geom += st->t_geom_us;
            pf_rast += st->t_raster_us;
            if (st->t_geom_us + st->t_raster_us > pf_max)
                pf_max = st->t_geom_us + st->t_raster_us;
        } else if (pf_lap != g_lap || g_state != ST_RACE) {
            if (pf_n > 100)
                papp_log("dustline: perf lap %d%s: %.1f fps average, worst 1 s %.1f, geom %.1f ms + raster %.1f ms average, "
                         "worst frame %.1f ms\n", pf_lap + 1, g_evening ? " (evening)" : "", pf_n / pf_t, (double)pf_worst,
                         pf_geom / pf_n / 1000.0, pf_rast / pf_n / 1000.0, pf_max / 1000.0);
            pf_n = 0;
            pf_t = pf_geom = pf_rast = 0;
            pf_worst = 999.f;
            pf_max = 0;
            pf_lap = g_state == ST_RACE ? g_lap : -1;
        }
        if (now - log_t >= 5000000) {
            const e3d_stats *st = e3d_get_stats(e);
            papp_audio_stats_t as = { 0 };
            if (g_snd)
                papp_audio_stats(g_snd, &as);
            papp_log("dustline: %.1f fps, lap %d, t %.1f, idx %d, %s, %.0f km/h, objs %d, tris %d/%d, shadow %d, "
                     "geom %d us, raster %d us, roller %d, underruns %lu\n",
                     (double)fps, g_lap + 1, (double)g_race_t, g_car.idx, surf_name[g_car.surf],
                     (double)(hypotf(g_car.vx, g_car.vz) * 3.6f), st->objects, st->tris_drawn, st->tris_in, st->shadow_tris,
                     st->t_geom_us, st->t_raster_us, s_pot_raw, (unsigned long)as.underruns);
            log_t = now;
        }
        papp_sem_take(g_pres_done, PAPP_WAIT_FOREVER);
        g_pres_buf = px;
        papp_sem_give(g_pres_go);
        cur ^= 1;
    }

    papp_sem_take(g_pres_done, 1000);
    g_pres_buf = NULL;
    papp_sem_give(g_pres_go);
    papp_thread_join(pres, PAPP_WAIT_FOREVER, NULL);
    if (aud) {
        g_snd_quit = true;
        papp_thread_join(aud, 1000, NULL);
        papp_audio_close(g_snd);
    }
    if (g_snd_lock)
        papp_mutex_destroy(g_snd_lock);
    e3d_destroy(e);
    free_meshes();
    papp_sem_destroy(g_pres_go);
    papp_sem_destroy(g_pres_done);
    papp_free(fb[0]);
    papp_free(fb[1]);
    papp_disp_surface_destroy(g_surf);
    return 0;
}
