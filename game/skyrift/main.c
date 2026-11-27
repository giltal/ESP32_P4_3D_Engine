/*
 * Skyrift — anti-gravity racing on the e3d engine, in the spirit of the PS1-era
 * future racers: six hover ships on elevated neon tracks high above a city.
 *
 * The tracks are 3D splines with hills, drops and banked corners, an overpass
 * and a tunnel, built in chunks of 32 m (the track, its neon trim and the
 * scenery, each in two levels of detail). The ground far below is a glowing
 * grid drawn by the background row function, with the city skyline on the
 * horizon, so the floor costs no triangles at all.
 *
 * Ships fly in track space (distance along the track, offset across it, yaw
 * against the track): thrust, a little drift, airbrakes for the hairpins, and
 * walls that hurt. Speed pads (blue chevrons) throw you forward; weapon pads
 * (amber) give a ROCKET, MINES, TURBO or a SHIELD. Every ship has an energy
 * shield: walls and weapons drain it, the green recharge lane on the start
 * straight fills it, and absorbing a weapon gives some back. At zero you are
 * eliminated.
 *
 * Speed classes: VENOM, FLASH, RAPIER, PHANTOM. Three laps by default.
 * Records (best lap, best race per track and class) in /sd/data/skyrift/records.txt.
 *
 * Controls:
 *   LEFT/RIGHT, left stick   steer          (roller boards: the roller steers)
 *   A, R2, UP                thrust         (touch play: thrust is automatic)
 *   L1 / R1                  left / right airbrake (both, or DOWN: brake)
 *   B                        fire the weapon
 *   X                        absorb the weapon (energy)
 *   START / SELECT           pause
 *   Touch: bottom left / right thirds steer, bottom middle fires,
 *          top left absorbs, the top-right corner pauses.
 *
 * Arguments: `auto` = the computer flies your ship too (testing),
 *            `fps` = show the frame rate, `track N`, `class N`, `laps N`,
 *            `res N` = frame size 0..3 (400x240, 480x288, 560x336, 640x384; also in Settings).
 */
#include "papp/papp.h"
#include "e3d.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

PAPP_MANIFEST(.id = "michael.skyrift", .name = "Skyrift", .version = PAPP_APP_VERSION, .main_stack = 16384);

/* the frame is drawn at one of these sizes and scaled to the display; bigger is sharper and slower */
#define WMAX 640
#define HMAX 384
static const int k_res[][2] = { { 400, 240 }, { 480, 288 }, { 560, 336 }, { 640, 384 } };
static const char *const k_res_name[] = { "400 x 240 (fastest)", "480 x 288", "560 x 336", "640 x 384 (slowest)" };
#define NRES ((int)(sizeof(k_res) / sizeof(k_res[0])))
static int g_res = 1;
static int g_w = 480, g_h = 288;
#define W g_w
#define H g_h
#define DEG (180.f / (float)M_PI)
#define RAD ((float)M_PI / 180.f)
#define NC(a) ((int)(sizeof(a) / sizeof(a[0])))

static float frand(void) { return (float)rand() / (float)RAND_MAX; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }

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

/* small 3-vector helpers */
static void v3_set(float *o, float x, float y, float z) { o[0] = x; o[1] = y; o[2] = z; }
static void v3_madd(float *o, const float *a, const float *b, float k)
{
    o[0] = a[0] + b[0] * k;
    o[1] = a[1] + b[1] * k;
    o[2] = a[2] + b[2] * k;
}
static float v3_dot(const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }
static void v3_cross(float *o, const float *a, const float *b)
{
    float x = a[1] * b[2] - a[2] * b[1], y = a[2] * b[0] - a[0] * b[2], z = a[0] * b[1] - a[1] * b[0];
    v3_set(o, x, y, z);
}
static void v3_norm(float *a)
{
    float l = sqrtf(v3_dot(a, a));
    if (l > 1e-9f) {
        a[0] /= l;
        a[1] /= l;
        a[2] /= l;
    }
}

/* ── Sound: an engine whine, boost roar, wind, and one-shot effects ───────── */
#define SR 22050
enum { WAVE_SQUARE, WAVE_TRI, WAVE_NOISE, WAVE_SAW };
typedef struct {
    int   left, delay, total;
    float ph, f, fmul, vol;
    int   wave;
} voice_t;
#define NVOICES 8
static voice_t g_voice[NVOICES];
static papp_mutex_t *g_snd_lock;
static papp_audio_stream_t *g_snd;
static volatile bool g_snd_quit;
static uint32_t g_noise = 0x1234567u;
/* continuous sounds, set by the game each frame */
static volatile float s_eng_f, s_eng_vol, s_boost_vol, s_wind_vol, s_scrape_vol;

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

/* the named effects */
static void snd_boost(void) { sfx(300, 1400, 380, WAVE_SAW, 0.22f); sfx(80, 40, 500, WAVE_NOISE, 0.25f); }
static void snd_pickup(void) { sfx(880, 880, 70, WAVE_SQUARE, 0.12f); sfx_at(1320, 1320, 110, WAVE_SQUARE, 0.12f, 70); }
static void snd_rocket(void) { sfx(900, 200, 450, WAVE_NOISE, 0.30f); sfx(600, 150, 300, WAVE_SAW, 0.12f); }
static void snd_mine(void) { sfx(220, 110, 120, WAVE_SQUARE, 0.15f); sfx_at(220, 110, 120, WAVE_SQUARE, 0.15f, 140); }
static void snd_boom(float vol) { sfx(160, 30, 700, WAVE_NOISE, vol); sfx(90, 35, 500, WAVE_SQUARE, vol * 0.5f); }
static void snd_wall(float vol) { sfx(400, 120, 160, WAVE_NOISE, vol); sfx(110, 70, 120, WAVE_SQUARE, vol * 0.5f); }
static void snd_absorb(void) { sfx(500, 1500, 300, WAVE_TRI, 0.22f); }
static void snd_shield(void) { sfx(200, 800, 400, WAVE_TRI, 0.2f); sfx_at(800, 400, 300, WAVE_TRI, 0.15f, 400); }
static void snd_beep(bool hi) { sfx(hi ? 1320.f : 660.f, hi ? 1320.f : 660.f, hi ? 500 : 180, WAVE_SQUARE, 0.16f); }
static void snd_lap(void)
{
    sfx(660, 660, 90, WAVE_SQUARE, 0.12f);
    sfx_at(880, 880, 90, WAVE_SQUARE, 0.12f, 100);
    sfx_at(1320, 1320, 200, WAVE_SQUARE, 0.12f, 200);
}

static int audio_main(void *arg)
{
    static int16_t buf[256];
    float eph = 0, eph2 = 0, ef = 80.f, ev = 0, bv = 0, wv = 0, sv = 0;
    float lp1 = 0, lp2 = 0, lp3 = 0, hp = 0;
    while (!g_snd_quit && !papp_exit_requested()) {
        papp_mutex_lock(g_snd_lock);
        float tf = s_eng_f, tv = s_eng_vol, tb = s_boost_vol, tw = s_wind_vol, ts = s_scrape_vol;
        for (int i = 0; i < 256; i++) {
            /* glide towards the targets (no clicks) */
            ef += (tf - ef) * 0.002f;
            ev += (tv - ev) * 0.002f;
            bv += (tb - bv) * 0.003f;
            wv += (tw - wv) * 0.002f;
            sv += (ts - sv) * 0.004f;
            g_noise = g_noise * 1664525u + 1013904223u;
            float nz = (float)(int32_t)g_noise * (1.f / 2147483648.f);
            /* the engine: a saw and a detuned pulse an octave up (the turbine whine) */
            eph += ef * (1.f / SR);
            if (eph >= 1.f) eph -= 1.f;
            eph2 += ef * 2.013f * (1.f / SR);
            if (eph2 >= 1.f) eph2 -= 1.f;
            float eng = (eph * 2.f - 1.f) * 0.55f + (eph2 < 0.4f ? 0.35f : -0.25f);
            lp1 += (eng - lp1) * 0.30f;
            float acc = lp1 * ev;
            /* boost: low roar; wind: soft hiss; scrape: harsh high noise */
            lp2 += (nz - lp2) * 0.06f;
            acc += lp2 * bv * 2.5f;
            lp3 += (nz - lp3) * 0.20f;
            acc += lp3 * wv * 0.6f;
            hp += (nz - hp) * 0.5f;
            acc += (nz - hp) * sv;
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
                        : v->wave == WAVE_SAW    ? v->ph * 2.f - 1.f
                                                 : nz;
                acc += s * v->vol * env;
                v->ph += v->f * (1.f / SR);
                if (v->ph >= 1.f)
                    v->ph -= 1.f;
                v->f *= v->fmul;
                v->left--;
            }
            acc = clampf(acc, -1.f, 1.f);
            buf[i] = (int16_t)(acc * 22000.f);
        }
        papp_mutex_unlock(g_snd_lock);
        papp_audio_write(g_snd, buf, 256, 100);
    }
    return 0;
}

/* ── Mesh building ───────────────────────────────────────────────────────────
 * A builder collects vertices and triangles, then bakes them into an e3d mesh.
 * Triangles are turned to face away from a given inside point. */
#define BMAX_V 12000
#define BMAX_T 9000
typedef struct {
    int nv, nt;
    float (*pos)[3];
    struct { uint16_t v[3]; uint32_t rgb; } *tri;
} builder_t;
static builder_t B;

static bool b_init(void)
{
    if (!B.pos)
        B.pos = papp_alloc(sizeof(float) * 3 * BMAX_V, PAPP_MEM_LARGE, 4);
    if (!B.tri)
        B.tri = papp_alloc(sizeof(B.tri[0]) * BMAX_T, PAPP_MEM_LARGE, 4);
    B.nv = B.nt = 0;
    return B.pos && B.tri;
}
static void b_free(void)
{
    papp_free(B.pos);
    papp_free(B.tri);
    B.pos = NULL;
    B.tri = NULL;
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
static int b_vp(const float *p) { return b_v(p[0], p[1], p[2]); }

/* outward from ctr: (v0 - v1) x (v2 - v1) points away from it */
static void b_tri(int a, int b, int c, uint32_t rgb, const float ctr[3])
{
    if (B.nt >= BMAX_T)
        return;
    float *p0 = B.pos[a], *p1 = B.pos[b], *p2 = B.pos[c];
    float u[3] = { p0[0] - p1[0], p0[1] - p1[1], p0[2] - p1[2] };
    float v[3] = { p2[0] - p1[0], p2[1] - p1[1], p2[2] - p1[2] };
    float n[3];
    v3_cross(n, u, v);
    float d[3] = { (p0[0] + p1[0] + p2[0]) / 3 - ctr[0], (p0[1] + p1[1] + p2[1]) / 3 - ctr[1],
                   (p0[2] + p1[2] + p2[2]) / 3 - ctr[2] };
    if (v3_dot(n, d) < 0) {
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
/* a quad facing along n (for open surfaces: the inside point is behind it) */
static void b_quad_n(int a, int b, int c, int d, uint32_t rgb, const float *n)
{
    float ctr[3] = { (B.pos[a][0] + B.pos[c][0]) * 0.5f - n[0] * 10.f, (B.pos[a][1] + B.pos[c][1]) * 0.5f - n[1] * 10.f,
                     (B.pos[a][2] + B.pos[c][2]) * 0.5f - n[2] * 10.f };
    b_quad(a, b, c, d, rgb, ctr);
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

/* an axis-aligned box (no bottom when open_bottom) */
static void b_box(float x0, float y0, float z0, float x1, float y1, float z1, uint32_t rgb, bool open_bottom)
{
    int v[8];
    for (int i = 0; i < 8; i++)
        v[i] = b_v((i & 1) ? x1 : x0, (i & 2) ? y1 : y0, (i & 4) ? z1 : z0);
    float c[3] = { (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f };
    if (!open_bottom)
        b_quad(v[0], v[1], v[5], v[4], shade_rgb(rgb, 0.85f), c);   /* bottom */
    b_quad(v[2], v[3], v[7], v[6], shade_rgb(rgb, 1.10f), c);       /* top */
    b_quad(v[0], v[1], v[3], v[2], rgb, c);                         /* z0 */
    b_quad(v[4], v[5], v[7], v[6], rgb, c);                         /* z1 */
    b_quad(v[0], v[2], v[6], v[4], shade_rgb(rgb, 0.92f), c);
    b_quad(v[1], v[3], v[7], v[5], shade_rgb(rgb, 0.92f), c);
}

/* a loft: rings of n points (y, z) at x positions, joined by quads, capped at both ends.
   ring k is ring[k * n .. ]; each ring is convex. Used for the ships. */
static void b_loft(const float *xs, const float (*ring)[2], int nr, int n, float zoff, const uint32_t *band_rgb,
                   uint32_t cap_rgb)
{
    int base = B.nv;
    for (int k = 0; k < nr; k++)
        for (int i = 0; i < n; i++)
            b_v(xs[k], ring[k * n + i][0], ring[k * n + i][1] + zoff);
    for (int k = 0; k + 1 < nr; k++) {
        float c[3] = { (xs[k] + xs[k + 1]) * 0.5f, 0, zoff };
        for (int i = 0; i < n; i++) {
            c[1] += (ring[k * n + i][0] + ring[(k + 1) * n + i][0]) * 0.5f / (float)n;
        }
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            int a = base + k * n + i, b = base + k * n + j, cc = base + (k + 1) * n + j, d = base + (k + 1) * n + i;
            b_quad(a, b, cc, d, band_rgb[(k * n + i) % (n * (nr - 1))], c);
        }
    }
    for (int e = 0; e < 2; e++) {
        int k = e ? nr - 1 : 0;
        float c[3] = { xs[k] + (e ? -0.3f : 0.3f), 0, zoff };
        for (int i = 0; i < n; i++)
            c[1] += ring[k * n + i][0] / (float)n;
        for (int i = 1; i + 1 < n; i++)
            b_tri(base + k * n, base + k * n + i, base + k * n + i + 1, cap_rgb, c);
    }
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

static e3d_mat mat_basis(const float *pos, const float *fwd, const float *up, const float *right, float s)
{
    e3d_mat m;
    for (int i = 0; i < 3; i++) {
        m.m[i][0] = fwd[i] * s;
        m.m[i][1] = up[i] * s;
        m.m[i][2] = right[i] * s;
        m.m[i][3] = pos[i];
    }
    m.m[3][0] = m.m[3][1] = m.m[3][2] = 0;
    m.m[3][3] = 1;
    return m;
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

/* ── The tracks ──────────────────────────────────────────────────────────────
 * Control points of a closed loop (metres: x, z on the ground plan, y the height of the
 * surface, hw the half width), sampled with a Catmull-Rom spline every STEP m along the
 * 3D curve. Banking comes from the curvature. fl = CF_TUNNEL: a tunnel from this point to
 * the next. */
enum { CF_TUNNEL = 1 };
typedef struct { float x, z, y, hw; uint8_t fl; } ctrl_t;

/* Altima Ridge: a figure of eight at sunset, the second half flies over the start straight */
static const ctrl_t k_ctrl_altima[] = {
    { -120, 0, 6, 8, 0 },     { 0, 0, 8, 8, 0 },        { 160, 0, 8, 8, 0 },      { 300, -10, 10, 8, 0 },
    { 400, -60, 14, 7.5f, 0 }, { 440, -150, 18, 7.5f, 0 }, { 400, -240, 22, 7.5f, 0 }, { 300, -270, 24, 7.5f, 0 },
    { 200, -230, 24, 7.5f, 0 }, { 120, -150, 22, 7.5f, 0 }, { 60, -60, 22, 8, 0 },     { 0, 40, 22, 8, 0 },
    { -60, 140, 20, 8, 0 },   { -140, 220, 16, 7.5f, CF_TUNNEL }, { -250, 250, 12, 7.5f, 0 },
    { -350, 210, 8, 7.5f, 0 }, { -390, 120, 4, 7.5f, 0 },  { -350, 40, 2, 7.5f, 0 },   { -250, 10, 4, 8, 0 },
};
/* Neon Bay: a night loop over the docks, a long climb to a crest, a chicane and a tunnel */
static const ctrl_t k_ctrl_neon[] = {
    { 0, 0, 6, 8, 0 },        { 180, 0, 6, 8, 0 },       { 330, 20, 10, 8, 0 },     { 430, 90, 20, 7.5f, 0 },
    { 460, 200, 34, 7.5f, 0 }, { 420, 300, 40, 7.5f, 0 }, { 330, 330, 34, 7.5f, 0 }, { 250, 290, 22, 7.5f, 0 },
    { 225, 215, 12, 7.5f, 0 }, { 160, 180, 10, 7.5f, 0 }, { 80, 230, 13, 7.5f, 0 },  { 0, 215, 12, 7.5f, 0 },
    { -80, 245, 16, 7.5f, CF_TUNNEL }, { -200, 280, 26, 8, CF_TUNNEL }, { -320, 240, 30, 8, 0 },
    { -380, 140, 22, 8, 0 },  { -350, 50, 12, 8, 0 },    { -240, 0, 6, 8, 0 },      { -110, -10, 6, 8, 0 },
};

typedef struct {
    const char *name, *file;
    const ctrl_t *ctrl;
    int nctrl;
    float charge0, charge1;               /* the recharge lane: from .. to (m along the track) */
    uint32_t sky_top, sky_mid, sky_hz;    /* sky: zenith, middle, at the horizon */
    uint32_t fog, ground, grid;           /* fog and haze; the ground far below and its grid lines */
    uint32_t skyline, skyline2, window;   /* the city on the horizon */
    uint32_t road, road2, edge, wall, neon, neon2, under, pillar;
    uint32_t bld[4], bld_glow[3];
    float fog_density, ambient;
    float light[3];                       /* the light, relative to the ship */
    uint32_t light_rgb;
} track_def_t;

static const track_def_t k_track[] = {
    { "ALTIMA RIDGE", "altima", k_ctrl_altima, NC(k_ctrl_altima), 30.f, 230.f,
      0x24407C, 0x7C6A98, 0xF0A878,
      0xC89080, 0x1C2030, 0x4A6A9C,
      0x6A5878, 0x5A4C6C, 0xFFD890,
      0x4E545E, 0x585E68, 0xE8E8EC, 0xB8C0CC, 0x30E8FF, 0xFF4890, 0x3A3E48, 0x8890A0,
      { 0xC8CCD4, 0xA8B0C0, 0xE0D8CC, 0x8890A8 }, { 0xFF7030, 0x30E8FF, 0xFFE040 },
      0.0030f, 0.42f, { -160.f, 260.f, 120.f }, 0xFFE8C8 },
    { "NEON BAY", "neonbay", k_ctrl_neon, NC(k_ctrl_neon), 20.f, 170.f,
      0x04030E, 0x180C34, 0x4A1E6A,
      0x2A1440, 0x08060E, 0x9030D0,
      0x140C24, 0x1C1030, 0xFFC860,
      0x363A46, 0x3E4250, 0xD8DCE8, 0x707890, 0xFF30C0, 0x30F0FF, 0x262A34, 0x50586C,
      { 0x404858, 0x303848, 0x584C68, 0x283040 }, { 0xFF30C0, 0x30F0FF, 0xFFB020 },
      0.0042f, 0.50f, { 60.f, 200.f, -150.f }, 0xC8D0FF },
};
#define NTRACKS NC(k_track)
static int g_track_i;
static const track_def_t *g_td = &k_track[0];

#define STEP       2.f
#define MAX_SAMP   1400
#define CHUNK      16                    /* samples per chunk (32 m) */
#define MAX_CHUNKS (MAX_SAMP / CHUNK + 1)
#define FLOOR_Y    (-30.f)               /* the ground, far below */
#define WALL_H     1.15f
#define SLAB       0.9f                  /* the track's thickness under the surface */
#define TUN_H      7.0f

typedef struct {
    float p[3];          /* the centre of the surface */
    float t[3], r[3], u[3];   /* tangent, right, up (banked) */
    float hw;            /* half width */
    float curv;          /* 1/radius on the ground plan, + = turning right */
    float bank;          /* radians, + = the right side lower */
    uint8_t fl;
} samp_t;

static samp_t g_tr[MAX_SAMP];
static int    g_ns;
static float  g_len;
static float  g_bx0, g_bz0, g_bx1, g_bz1;

static float wrapd(float d)              /* a distance along the track into -L/2 .. L/2 */
{
    while (d > g_len * 0.5f) d -= g_len;
    while (d < -g_len * 0.5f) d += g_len;
    return d;
}
static float wrapu(float u)
{
    u = fmodf(u, g_len);
    return u < 0 ? u + g_len : u;
}

static void build_track(void)
{
    enum { SUB = 64 };
    static float pp[24 * SUB + 1][4];
    static uint8_t pf[24 * SUB + 1];
    const ctrl_t *kc = g_td->ctrl;
    const int nc = g_td->nctrl < 24 ? g_td->nctrl : 24;
    int np = 0;
    for (int i = 0; i < nc; i++) {
        const ctrl_t *p0 = &kc[(i + nc - 1) % nc], *p1 = &kc[i], *p2 = &kc[(i + 1) % nc], *p3 = &kc[(i + 2) % nc];
        const float a0[4] = { p0->x, p0->y, p0->z, p0->hw }, a1[4] = { p1->x, p1->y, p1->z, p1->hw },
                    a2[4] = { p2->x, p2->y, p2->z, p2->hw }, a3[4] = { p3->x, p3->y, p3->z, p3->hw };
        for (int s = 0; s < SUB; s++) {
            float t = (float)s / SUB, t2 = t * t, t3 = t2 * t;
            for (int k = 0; k < 4; k++)
                pp[np][k] = 0.5f * (2 * a1[k] + (-a0[k] + a2[k]) * t + (2 * a0[k] - 5 * a1[k] + 4 * a2[k] - a3[k]) * t2 +
                                    (-a0[k] + 3 * a1[k] - 3 * a2[k] + a3[k]) * t3);
            pf[np] = p1->fl;
            np++;
        }
    }
    /* arc length */
    static float cum[24 * SUB + 1];
    cum[0] = 0;
    for (int i = 1; i <= np; i++) {
        const float *a = pp[i - 1], *b = pp[i % np];
        cum[i] = cum[i - 1] + sqrtf((b[0] - a[0]) * (b[0] - a[0]) + (b[1] - a[1]) * (b[1] - a[1]) + (b[2] - a[2]) * (b[2] - a[2]));
    }
    float tot = cum[np];
    int n = (int)(tot / STEP);
    if (n > MAX_SAMP)
        n = MAX_SAMP;
    g_ns = n;
    g_len = (float)n * STEP;
    int j = 0;
    for (int k = 0; k < n; k++) {
        float d = (float)k * tot / (float)n;
        while (j < np - 1 && cum[j + 1] < d)
            j++;
        float f = (d - cum[j]) / fmaxf(cum[j + 1] - cum[j], 1e-6f);
        const float *a = pp[j], *b = pp[(j + 1) % np];
        samp_t *s = &g_tr[k];
        v3_set(s->p, lerpf(a[0], b[0], f), lerpf(a[1], b[1], f), lerpf(a[2], b[2], f));
        s->hw = lerpf(a[3], b[3], f);
        s->fl = pf[j];
    }
    /* tangents, curvature on the ground plan */
    static float head[MAX_SAMP], cv[MAX_SAMP];
    for (int k = 0; k < n; k++) {
        const float *a = g_tr[(k + n - 1) % n].p, *b = g_tr[(k + 1) % n].p;
        samp_t *s = &g_tr[k];
        v3_set(s->t, b[0] - a[0], b[1] - a[1], b[2] - a[2]);
        v3_norm(s->t);
        head[k] = atan2f(s->t[2], s->t[0]);
    }
    for (int k = 0; k < n; k++) {
        float dh = head[(k + 2) % n] - head[(k + n - 2) % n];
        while (dh > (float)M_PI) dh -= 2.f * (float)M_PI;
        while (dh < -(float)M_PI) dh += 2.f * (float)M_PI;
        cv[k] = dh / (4.f * STEP);
    }
    /* smooth the curvature, bank from a wider window */
    for (int k = 0; k < n; k++) {
        float c = 0, b = 0;
        for (int d = -4; d <= 4; d++)
            c += cv[(k + d + n) % n];
        for (int d = -10; d <= 10; d++)
            b += cv[(k + d + n) % n];
        g_tr[k].curv = c / 9.f;
        g_tr[k].bank = clampf(b / 21.f * 22.f, -0.40f, 0.40f);
    }
    g_bx0 = g_bz0 = 1e9f;
    g_bx1 = g_bz1 = -1e9f;
    for (int k = 0; k < n; k++) {
        samp_t *s = &g_tr[k];
        float up[3] = { 0, 1, 0 }, u0[3], r0[3];
        v3_madd(u0, up, s->t, -v3_dot(up, s->t));
        v3_norm(u0);
        v3_cross(r0, s->t, u0);
        float cb = cosf(s->bank), sb = sinf(s->bank);
        for (int i = 0; i < 3; i++) {
            s->r[i] = r0[i] * cb - u0[i] * sb;
            s->u[i] = u0[i] * cb + r0[i] * sb;
        }
        g_bx0 = fminf(g_bx0, s->p[0]);
        g_bx1 = fmaxf(g_bx1, s->p[0]);
        g_bz0 = fminf(g_bz0, s->p[2]);
        g_bz1 = fmaxf(g_bz1, s->p[2]);
    }
}

/* the track's frame at distance u (interpolated) */
typedef struct { float p[3], t[3], r[3], u[3]; float hw, curv, slope; uint8_t fl; } frame_t;
static void track_frame(float u, frame_t *f)
{
    u = wrapu(u);
    float fi = u / STEP;
    int i = (int)fi;
    float k = fi - (float)i;
    i %= g_ns;
    const samp_t *a = &g_tr[i], *b = &g_tr[(i + 1) % g_ns];
    for (int q = 0; q < 3; q++) {
        f->p[q] = lerpf(a->p[q], b->p[q], k);
        f->t[q] = lerpf(a->t[q], b->t[q], k);
        f->r[q] = lerpf(a->r[q], b->r[q], k);
        f->u[q] = lerpf(a->u[q], b->u[q], k);
    }
    f->hw = lerpf(a->hw, b->hw, k);
    f->curv = lerpf(a->curv, b->curv, k);
    f->slope = f->t[1];
    f->fl = a->fl;
}
/* a point in track space: along u, across x, h above the surface */
static void track_point(float u, float x, float h, float *out)
{
    frame_t f;
    track_frame(u, &f);
    for (int q = 0; q < 3; q++)
        out[q] = f.p[q] + f.r[q] * x + f.u[q] * h;
}
static void samp_point(int k, float x, float h, float *out)
{
    const samp_t *s = &g_tr[(k % g_ns + g_ns) % g_ns];
    for (int q = 0; q < 3; q++)
        out[q] = s->p[q] + s->r[q] * x + s->u[q] * h;
}

/* ── Pads ──────────────────────────────────────────────────────────────────
 * Speed pads where a straight starts, rows of weapon pads at fixed places. */
enum { PAD_BOOST, PAD_WEAPON };
typedef struct { float u, x; uint8_t type; } pad_t;
#define MAX_PADS 64
static pad_t g_pad[MAX_PADS];
static int g_npads;
#define PAD_LEN 6.f
#define PAD_W   3.2f

static float max_curv(float u0, float len)
{
    float m = 0;
    for (float d = 0; d < len; d += STEP * 2) {
        int k = (int)(wrapu(u0 + d) / STEP) % g_ns;
        m = fmaxf(m, fabsf(g_tr[k].curv));
    }
    return m;
}
static bool in_charge(float u) { return u >= g_td->charge0 && u <= g_td->charge1; }

static void place_pads(void)
{
    g_npads = 0;
    /* weapon rows: two pads across */
    static const float wf[] = { 0.17f, 0.46f, 0.74f };
    for (int i = 0; i < NC(wf); i++) {
        float u = g_len * wf[i];
        int k = (int)(u / STEP);
        float hw = g_tr[k].hw;
        g_pad[g_npads++] = (pad_t){ u, -hw * 0.42f, PAD_WEAPON };
        g_pad[g_npads++] = (pad_t){ u, hw * 0.42f, PAD_WEAPON };
    }
    /* speed pads: where the next 90 m are nearly straight, at least 130 m apart */
    float last = -1e9f;
    int side = 0;
    for (float u = 60.f; u < g_len - 60.f && g_npads < MAX_PADS; u += 10.f) {
        if (u - last < 130.f || in_charge(u) || in_charge(u + 20.f))
            continue;
        bool near_weapon = false;
        for (int i = 0; i < NC(wf); i++)
            if (fabsf(u - g_len * wf[i]) < 30.f)
                near_weapon = true;
        if (near_weapon || max_curv(u, 90.f) > 1.f / 260.f)
            continue;
        int k = (int)(u / STEP);
        float hw = g_tr[k].hw;
        static const float sx[] = { -0.45f, 0.45f, 0.f };
        g_pad[g_npads++] = (pad_t){ u, hw * sx[side % 3], PAD_BOOST };
        /* a second one further on, the other side */
        if (g_npads < MAX_PADS)
            g_pad[g_npads++] = (pad_t){ u + 24.f, hw * sx[(side + 1) % 3], PAD_BOOST };
        side++;
        last = u;
    }
}

/* ── Chunks: the track and scenery in pieces of CHUNK samples ─────────────── */
typedef struct {
    e3d_mesh *road[2], *rneon[2];   /* the track: near / far detail, lit and neon */
    e3d_mesh *scen, *sneon;         /* scenery */
    float c[3], r;                  /* the track part's bounds */
} chunk_t;
static chunk_t g_chunk[MAX_CHUNKS];
static int g_nchunks;
static e3d_mesh *g_gantry;          /* the start gantry */
static e3d_mesh *g_lamp;            /* one start light */

/* an oriented box: centre c, axes a0 (length 2*h0), a1, a2 */
static void b_obox(const float *c, const float *a0, const float *a1, const float *a2, float h0, float h1, float h2,
                   uint32_t rgb)
{
    int v[8];
    for (int i = 0; i < 8; i++) {
        float s0 = (i & 1) ? h0 : -h0, s1 = (i & 2) ? h1 : -h1, s2 = (i & 4) ? h2 : -h2;
        v[i] = b_v(c[0] + a0[0] * s0 + a1[0] * s1 + a2[0] * s2, c[1] + a0[1] * s0 + a1[1] * s1 + a2[1] * s2,
                   c[2] + a0[2] * s0 + a1[2] * s1 + a2[2] * s2);
    }
    b_quad(v[0], v[1], v[5], v[4], shade_rgb(rgb, 0.8f), c);
    b_quad(v[2], v[3], v[7], v[6], shade_rgb(rgb, 1.1f), c);
    b_quad(v[0], v[1], v[3], v[2], rgb, c);
    b_quad(v[4], v[5], v[7], v[6], rgb, c);
    b_quad(v[0], v[2], v[6], v[4], shade_rgb(rgb, 0.9f), c);
    b_quad(v[1], v[3], v[7], v[5], shade_rgb(rgb, 0.9f), c);
}

/* a flat quad on the track surface: u0..u1 along, x0..x1 across, h above */
static void b_surf(float u0, float u1, float x0, float x1, float h, uint32_t rgb)
{
    float a[3], b[3], c[3], d[3], n[3];
    track_point(u0, x0, h, a);
    track_point(u0, x1, h, b);
    track_point(u1, x1, h, c);
    track_point(u1, x0, h, d);
    frame_t f;
    track_frame((u0 + u1) * 0.5f, &f);
    v3_set(n, f.u[0], f.u[1], f.u[2]);
    b_quad_n(b_vp(a), b_vp(b), b_vp(c), b_vp(d), rgb, n);
}

/* the track between samples k0 and k1 (k1 may run past the end), every `step` samples */
static void road_lit(int k0, int k1, int step, bool far)
{
    const track_def_t *td = g_td;
    for (int k = k0; k < k1; k += step) {
        int ka = k, kb = k + step > k1 ? k1 : k + step;
        const samp_t *sa = &g_tr[ka % g_ns], *sb = &g_tr[kb % g_ns];
        float hwa = sa->hw, hwb = sb->hw;
        bool alt = ((k / 2) & 1) != 0;
        float P[2][8][3];
        /* cross-section: 0 wall base out, 1 wall base, 2 edge in, 3 edge in (right), 4 right wall base,
           5 right out, 6 underside left, 7 underside right */
        for (int e = 0; e < 2; e++) {
            int kk = e ? kb : ka;
            float hw = e ? hwb : hwa;
            samp_point(kk, -hw, 0, P[e][1]);
            samp_point(kk, -hw + 0.8f, 0, P[e][2]);
            samp_point(kk, hw - 0.8f, 0, P[e][3]);
            samp_point(kk, hw, 0, P[e][4]);
            samp_point(kk, -hw - 0.5f, -SLAB, P[e][6]);
            samp_point(kk, hw + 0.5f, -SLAB, P[e][7]);
        }
        const float *nu = sa->u;
        float nr[3] = { sa->r[0], sa->r[1], sa->r[2] }, nl[3] = { -nr[0], -nr[1], -nr[2] };
        float nd[3] = { -nu[0], -nu[1], -nu[2] };
        uint32_t road = alt ? td->road2 : td->road;
        if (far) {
            b_quad_n(b_vp(P[0][1]), b_vp(P[0][4]), b_vp(P[1][4]), b_vp(P[1][1]), road, nu);
        } else {
            b_quad_n(b_vp(P[0][1]), b_vp(P[0][2]), b_vp(P[1][2]), b_vp(P[1][1]), td->edge, nu);
            b_quad_n(b_vp(P[0][2]), b_vp(P[0][3]), b_vp(P[1][3]), b_vp(P[1][2]), road, nu);
            b_quad_n(b_vp(P[0][3]), b_vp(P[0][4]), b_vp(P[1][4]), b_vp(P[1][3]), td->edge, nu);
        }
        /* walls: the inner faces (lower part, the neon band is above), outer faces down to the slab */
        float wl[2][3], wr[2][3], tl[2][3], tr[2][3], ol[2][3], orr[2][3];
        for (int e = 0; e < 2; e++) {
            int kk = e ? kb : ka;
            float hw = e ? hwb : hwa;
            samp_point(kk, -hw, far ? WALL_H : 0.72f, wl[e]);
            samp_point(kk, hw, far ? WALL_H : 0.72f, wr[e]);
            samp_point(kk, -hw - 0.5f, WALL_H, tl[e]);
            samp_point(kk, hw + 0.5f, WALL_H, tr[e]);
            samp_point(kk, -hw - 0.5f, -SLAB, ol[e]);
            samp_point(kk, hw + 0.5f, -SLAB, orr[e]);
        }
        b_quad_n(b_vp(P[0][1]), b_vp(wl[0]), b_vp(wl[1]), b_vp(P[1][1]), td->wall, nr);
        b_quad_n(b_vp(P[0][4]), b_vp(wr[0]), b_vp(wr[1]), b_vp(P[1][4]), td->wall, nl);
        if (!far || (sa->fl & CF_TUNNEL) == 0) {
            b_quad_n(b_vp(ol[0]), b_vp(tl[0]), b_vp(tl[1]), b_vp(ol[1]), td->under, nl);
            b_quad_n(b_vp(orr[0]), b_vp(tr[0]), b_vp(tr[1]), b_vp(orr[1]), td->under, nr);
        }
        b_quad_n(b_vp(P[0][6]), b_vp(P[0][7]), b_vp(P[1][7]), b_vp(P[1][6]), td->under, nd);
        /* a tunnel: walls up to the roof, the roof; inside faces and outside faces */
        if (sa->fl & CF_TUNNEL) {
            float a[2][4][3];   /* left top, right top, left roof, right roof */
            for (int e = 0; e < 2; e++) {
                int kk = e ? kb : ka;
                float hw = e ? hwb : hwa;
                samp_point(kk, -hw - 0.5f, WALL_H, a[e][0]);
                samp_point(kk, hw + 0.5f, WALL_H, a[e][1]);
                samp_point(kk, -hw - 0.5f, TUN_H, a[e][2]);
                samp_point(kk, hw + 0.5f, TUN_H, a[e][3]);
            }
            uint32_t tw = shade_rgb(td->wall, 0.55f), tro = shade_rgb(td->under, 1.2f);
            b_quad_n(b_vp(a[0][0]), b_vp(a[0][2]), b_vp(a[1][2]), b_vp(a[1][0]), tw, nr);
            b_quad_n(b_vp(a[0][1]), b_vp(a[0][3]), b_vp(a[1][3]), b_vp(a[1][1]), tw, nl);
            b_quad_n(b_vp(a[0][2]), b_vp(a[0][3]), b_vp(a[1][3]), b_vp(a[1][2]), shade_rgb(td->under, 0.7f), nd);
            if (!far) {
                b_quad_n(b_vp(a[0][0]), b_vp(a[0][2]), b_vp(a[1][2]), b_vp(a[1][0]), tro, nl);
                b_quad_n(b_vp(a[0][1]), b_vp(a[0][3]), b_vp(a[1][3]), b_vp(a[1][1]), tro, nr);
            }
            b_quad_n(b_vp(a[0][2]), b_vp(a[0][3]), b_vp(a[1][3]), b_vp(a[1][2]), tro, nu);
        }
    }
}

/* the glowing parts of the track: wall bands and tops, pads, the start line, the recharge lane */
static void road_neon(int k0, int k1, int step, bool far)
{
    const track_def_t *td = g_td;
    for (int k = k0; k < k1; k += step) {
        int ka = k, kb = k + step > k1 ? k1 : k + step;
        const samp_t *sa = &g_tr[ka % g_ns];
        bool alt = ((k / 2) & 1) != 0;
        uint32_t band = alt ? td->neon : shade_rgb(td->neon, 0.55f);
        float hwa = sa->hw, hwb = g_tr[kb % g_ns].hw;
        float a[2][6][3];
        for (int e = 0; e < 2; e++) {
            int kk = e ? kb : ka;
            float hw = e ? hwb : hwa;
            samp_point(kk, -hw, 0.72f, a[e][0]);
            samp_point(kk, -hw, WALL_H, a[e][1]);
            samp_point(kk, -hw - 0.5f, WALL_H, a[e][2]);
            samp_point(kk, hw, 0.72f, a[e][3]);
            samp_point(kk, hw, WALL_H, a[e][4]);
            samp_point(kk, hw + 0.5f, WALL_H, a[e][5]);
        }
        float nr[3] = { sa->r[0], sa->r[1], sa->r[2] }, nl[3] = { -nr[0], -nr[1], -nr[2] };
        if (!far) {
            b_quad_n(b_vp(a[0][0]), b_vp(a[0][1]), b_vp(a[1][1]), b_vp(a[1][0]), band, nr);
            b_quad_n(b_vp(a[0][3]), b_vp(a[0][4]), b_vp(a[1][4]), b_vp(a[1][3]), band, nl);
        }
        b_quad_n(b_vp(a[0][1]), b_vp(a[0][2]), b_vp(a[1][2]), b_vp(a[1][1]), td->neon, sa->u);
        b_quad_n(b_vp(a[0][4]), b_vp(a[0][5]), b_vp(a[1][5]), b_vp(a[1][4]), td->neon, sa->u);
        /* tunnel: two light strips on the ceiling */
        if ((sa->fl & CF_TUNNEL) && !far && k % 4 == 0) {
            float u0 = (float)ka * STEP, u1 = u0 + STEP * 1.5f;
            float hw = hwa;
            float p[4][3], nd[3] = { -sa->u[0], -sa->u[1], -sa->u[2] };
            for (int s = -1; s <= 1; s += 2) {
                track_point(u0, s * hw * 0.5f - 0.6f, TUN_H - 0.05f, p[0]);
                track_point(u0, s * hw * 0.5f + 0.6f, TUN_H - 0.05f, p[1]);
                track_point(u1, s * hw * 0.5f + 0.6f, TUN_H - 0.05f, p[2]);
                track_point(u1, s * hw * 0.5f - 0.6f, TUN_H - 0.05f, p[3]);
                b_quad_n(b_vp(p[0]), b_vp(p[1]), b_vp(p[2]), b_vp(p[3]), td->neon2, nd);
            }
        }
    }
    if (far)
        return;
    float u0 = (float)k0 * STEP, u1 = (float)k1 * STEP;
    /* the recharge lane: green dashes along the left side */
    for (float u = ceilf(fmaxf(u0, g_td->charge0) / 4.f) * 4.f; u < u1 && u < g_td->charge1; u += 4.f) {
        int k = (int)(u / STEP) % g_ns;
        float hw = g_tr[k].hw;
        b_surf(u, u + 2.5f, -hw + 0.9f, -hw + 3.3f, 0.06f, ((int)(u / 4.f) & 1) ? 0x20FF70 : 0x109040);
    }
    /* the start line: a chequered strip */
    if (u0 <= 0.01f) {
        float hw = g_tr[0].hw;
        for (int r = 0; r < 2; r++)
            for (int i = 0; i < 10; i++) {
                float xa = -hw + 2.f * hw * (float)i / 10.f, xb = -hw + 2.f * hw * (float)(i + 1) / 10.f;
                b_surf((float)r * 1.5f, (float)r * 1.5f + 1.5f, xa, xb, 0.05f, ((i + r) & 1) ? 0x101010 : 0xF0F0F0);
            }
    }
    /* pads */
    for (int i = 0; i < g_npads; i++) {
        const pad_t *p = &g_pad[i];
        if (p->u < u0 || p->u >= u1)
            continue;
        float a = p->u - PAD_LEN * 0.5f, b = p->u + PAD_LEN * 0.5f, x0 = p->x - PAD_W * 0.5f, x1 = p->x + PAD_W * 0.5f;
        if (p->type == PAD_BOOST) {
            b_surf(a, b, x0, x1, 0.06f, 0x0838A0);
            /* two chevrons pointing ahead */
            for (int c = 0; c < 2; c++) {
                float s = a + 0.8f + (float)c * 2.4f;
                float P[6][3];
                track_point(s, x0 + 0.3f, 0.11f, P[0]);
                track_point(s + 0.7f, x0 + 0.3f, 0.11f, P[1]);
                track_point(s + 1.9f, p->x, 0.11f, P[2]);
                track_point(s + 1.2f, p->x, 0.11f, P[3]);
                track_point(s, x1 - 0.3f, 0.11f, P[4]);
                track_point(s + 0.7f, x1 - 0.3f, 0.11f, P[5]);
                float n[3];
                frame_t f;
                track_frame(s, &f);
                v3_set(n, f.u[0], f.u[1], f.u[2]);
                int v[6];
                for (int q = 0; q < 6; q++)
                    v[q] = b_vp(P[q]);
                b_quad_n(v[0], v[1], v[2], v[3], 0x40F0FF, n);
                b_quad_n(v[4], v[5], v[2], v[3], 0x40F0FF, n);
            }
        } else {
            b_surf(a, b, x0, x1, 0.06f, 0xFFA010);
            b_surf(a + 1.0f, b - 1.0f, x0 + 0.7f, x1 - 0.7f, 0.10f, 0x804000);
            b_surf(a + 2.2f, b - 2.2f, x0 + 0.2f, x1 - 0.2f, 0.12f, 0xFFE060);
            b_surf(a + 0.4f, b - 0.4f, p->x - 0.25f, p->x + 0.25f, 0.12f, 0xFFE060);
        }
    }
}

/* the scenery: pillars under the track, light pylons, arches over it, the towers of the city */
static bool clear_of_track(float x, float z, float margin, int skip_k, int skip_span)
{
    for (int k = 0; k < g_ns; k += 2) {
        if (skip_span > 0) {
            int d = abs(k - skip_k);
            if (d > g_ns / 2)
                d = g_ns - d;
            if (d < skip_span)
                continue;
        }
        float dx = g_tr[k].p[0] - x, dz = g_tr[k].p[2] - z, m = g_tr[k].hw + margin;
        if (dx * dx + dz * dz < m * m)
            return false;
    }
    return true;
}

typedef struct { float x, z, w, d, h; uint8_t style; } bld_t;
#define MAX_BLD 260
static bld_t g_bld[MAX_BLD];
static int g_nbld;
static uint32_t s_seed = 1;
static float rnd(void)
{
    s_seed = s_seed * 1664525u + 1013904223u;
    return (float)(s_seed >> 8) * (1.f / 16777216.f);
}

static void place_buildings(void)
{
    s_seed = 0xC17A + (uint32_t)g_track_i * 977u;
    g_nbld = 0;
    float x0 = g_bx0 - 260.f, x1 = g_bx1 + 260.f, z0 = g_bz0 - 260.f, z1 = g_bz1 + 260.f;
    for (int tries = 0; tries < 4000 && g_nbld < MAX_BLD; tries++) {
        bld_t b;
        b.x = x0 + (x1 - x0) * rnd();
        b.z = z0 + (z1 - z0) * rnd();
        b.w = 14.f + rnd() * 26.f;
        b.d = 14.f + rnd() * 26.f;
        /* the nearer the track, the more likely a tall tower */
        b.h = 25.f + rnd() * rnd() * 150.f;
        b.style = (uint8_t)(rnd() * 4.f);
        float m = fmaxf(b.w, b.d) * 0.75f + 10.f;
        if (!clear_of_track(b.x, b.z, m, 0, 0))
            continue;
        bool ok = true;
        for (int i = 0; i < g_nbld && ok; i++) {
            float dx = g_bld[i].x - b.x, dz = g_bld[i].z - b.z, mm = (g_bld[i].w + b.w) * 0.5f + 6.f;
            if (fabsf(dx) < mm && fabsf(dz) < (g_bld[i].d + b.d) * 0.5f + 6.f)
                ok = false;
        }
        if (ok)
            g_bld[g_nbld++] = b;
    }
}

static int nearest_samp(float x, float z)
{
    int best = 0;
    float bd = 1e18f;
    for (int k = 0; k < g_ns; k += 2) {
        float dx = g_tr[k].p[0] - x, dz = g_tr[k].p[2] - z, d = dx * dx + dz * dz;
        if (d < bd) {
            bd = d;
            best = k;
        }
    }
    return best;
}

static void scenery(int ci, bool neon)
{
    const track_def_t *td = g_td;
    int k0 = ci * CHUNK, k1 = k0 + CHUNK > g_ns ? g_ns : k0 + CHUNK;
    float up[3] = { 0, 1, 0 };
    for (int k = k0; k < k1; k++) {
        const samp_t *s = &g_tr[k];
        float h = s->p[1] - SLAB - FLOOR_Y;
        /* pillars every 24 m */
        if (k % 12 == 6 && h > 3.f && !neon) {
            float c[3] = { s->p[0], FLOOR_Y + h * 0.5f, s->p[2] };
            float fx[3] = { s->t[0], 0, s->t[2] }, rx[3];
            v3_norm(fx);
            v3_cross(rx, fx, up);
            if (clear_of_track(c[0], c[2], 3.f, k, 20)) {
                b_obox(c, fx, up, rx, 1.3f, h * 0.5f + 0.3f, 1.6f, td->pillar);
                float cc[3] = { s->p[0], s->p[1] - SLAB - 0.6f, s->p[2] };
                b_obox(cc, fx, up, rx, 2.0f, 0.6f, s->hw * 0.8f, shade_rgb(td->pillar, 0.85f));
            }
        }
        /* pylons every 60 m, alternate sides: a mast with a glowing top */
        if (k % 30 == 15) {
            float side = (k / 30) & 1 ? 1.f : -1.f;
            float base[3];
            samp_point(k, side * (s->hw + 7.f), 0, base);
            if (clear_of_track(base[0], base[2], 2.f, k, 14)) {
                float top = s->p[1] + 16.f;
                float fx[3] = { s->t[0], 0, s->t[2] }, rx[3];
                v3_norm(fx);
                v3_cross(rx, fx, up);
                if (!neon) {
                    float c[3] = { base[0], (FLOOR_Y + top) * 0.5f, base[2] };
                    b_obox(c, fx, up, rx, 0.6f, (top - FLOOR_Y) * 0.5f, 0.6f, td->pillar);
                } else {
                    float c[3] = { base[0], top + 1.2f, base[2] };
                    b_obox(c, fx, up, rx, 1.0f, 1.2f, 1.0f, (k / 30) % 3 ? td->neon : td->neon2);
                }
            }
        }
        /* an arch over the track every 220 m (not in tunnels, not at the start gantry) */
        if (k % 110 == 55 && !(s->fl & CF_TUNNEL) && k > 20) {
            float hw = s->hw + 1.6f;
            float L[3], R[3], T[3];
            samp_point(k, -hw, 4.5f, L);
            samp_point(k, hw, 4.5f, R);
            samp_point(k, 0, 9.4f, T);
            if (!neon) {
                b_obox(L, s->t, s->u, s->r, 0.5f, 5.0f, 0.5f, td->wall);
                b_obox(R, s->t, s->u, s->r, 0.5f, 5.0f, 0.5f, td->wall);
                b_obox(T, s->t, s->u, s->r, 0.7f, 0.6f, hw + 0.5f, td->wall);
            } else {
                float B2[3];
                samp_point(k, 0, 8.1f, B2);
                b_obox(B2, s->t, s->u, s->r, 0.25f, 0.6f, hw - 0.6f, (k / 110) & 1 ? td->neon2 : td->neon);
            }
        }
    }
    /* buildings whose nearest sample is in this chunk */
    for (int i = 0; i < g_nbld; i++) {
        const bld_t *b = &g_bld[i];
        int k = nearest_samp(b->x, b->z);
        if (k < k0 || k >= k1)
            continue;
        float top = FLOOR_Y + b->h;
        if (!neon) {
            b_box(b->x - b->w * 0.5f, FLOOR_Y, b->z - b->d * 0.5f, b->x + b->w * 0.5f, top, b->z + b->d * 0.5f,
                  td->bld[b->style], true);
            if (b->style == 1 && b->h > 70.f) {    /* a stepped top */
                float s = 0.32f;
                b_box(b->x - b->w * s, top, b->z - b->d * s, b->x + b->w * s, top + 12.f, b->z + b->d * s,
                      td->bld[(b->style + 2) & 3], true);
            }
        } else {
            /* a glowing band near the top, a second one lower on tall towers */
            uint32_t g = td->bld_glow[i % 3];
            int bands = b->h > 90.f ? 2 : 1;
            for (int q = 0; q < bands; q++) {
                float y0 = top - 5.f - (float)q * b->h * 0.38f, y1 = y0 + 1.6f;
                float e = 0.15f;
                b_box(b->x - b->w * 0.5f - e, y0, b->z - b->d * 0.5f - e, b->x + b->w * 0.5f + e, y1, b->z + b->d * 0.5f + e,
                      q ? shade_rgb(g, 0.7f) : g, true);
            }
        }
    }
}

static bool build_chunks(void)
{
    g_nchunks = (g_ns + CHUNK - 1) / CHUNK;
    for (int i = 0; i < g_nchunks; i++) {
        chunk_t *c = &g_chunk[i];
        int k0 = i * CHUNK, k1 = k0 + CHUNK;
        if (k1 > g_ns)
            k1 = g_ns;
        for (int l = 0; l < 2; l++) {
            b_reset();
            road_lit(k0, k1, l ? 4 : 1, l == 1);
            c->road[l] = b_bake(25.f);
            b_reset();
            road_neon(k0, k1, l ? 4 : 1, l == 1);
            c->rneon[l] = b_bake(25.f);
            if (!c->road[l] || !c->rneon[l])
                return false;
        }
        b_reset();
        scenery(i, false);
        c->scen = B.nt ? b_bake(30.f) : NULL;
        b_reset();
        scenery(i, true);
        c->sneon = B.nt ? b_bake(30.f) : NULL;
        c->c[0] = c->road[0]->center[0];
        c->c[1] = c->road[0]->center[1];
        c->c[2] = c->road[0]->center[2];
        c->r = c->road[0]->radius;
    }
    /* the start gantry: two towers and a beam with a light board */
    b_reset();
    {
        const samp_t *s = &g_tr[0];
        float hw = s->hw + 2.f, L[3], R[3], T[3], Bd[3];
        samp_point(0, -hw, 4.5f, L);
        samp_point(0, hw, 4.5f, R);
        samp_point(0, 0, 9.8f, T);
        samp_point(0, 0, 8.0f, Bd);
        b_obox(L, s->t, s->u, s->r, 0.9f, 5.4f, 0.9f, 0xE0E0E8);
        b_obox(R, s->t, s->u, s->r, 0.9f, 5.4f, 0.9f, 0xE0E0E8);
        b_obox(T, s->t, s->u, s->r, 1.0f, 0.9f, hw + 0.9f, 0x30343C);
        b_obox(Bd, s->t, s->u, s->r, 0.4f, 1.0f, 4.0f, 0x181A20);
    }
    g_gantry = b_bake(25.f);
    b_reset();
    {
        float c[3] = { 0, 0, 0 }, a0[3] = { 1, 0, 0 }, a1[3] = { 0, 1, 0 }, a2[3] = { 0, 0, 1 };
        b_obox(c, a0, a1, a2, 0.3f, 0.6f, 0.6f, 0xFFFFFF);
    }
    g_lamp = b_bake(25.f);
    return g_gantry && g_lamp;
}

static void free_track(void)
{
    for (int i = 0; i < g_nchunks; i++) {
        chunk_t *c = &g_chunk[i];
        for (int l = 0; l < 2; l++) {
            e3d_mesh_free(c->road[l]);
            e3d_mesh_free(c->rneon[l]);
        }
        e3d_mesh_free(c->scen);
        e3d_mesh_free(c->sneon);
        memset(c, 0, sizeof(*c));
    }
    g_nchunks = 0;
    e3d_mesh_free(g_gantry);
    e3d_mesh_free(g_lamp);
    g_gantry = g_lamp = NULL;
}

/* ── Teams and ship meshes ───────────────────────────────────────────────── */
#define NSHIPS 6
typedef struct { const char *name; uint32_t prim, sec, stripe; } team_t;
static const team_t k_team[NSHIPS] = {
    { "VOLTA", 0x2060E0, 0xF0F0F4, 0xFFC020 },
    { "KESTREL", 0xE02828, 0xF0F0F4, 0x202020 },
    { "NOVA", 0x8040C8, 0xF0C040, 0xF0F0F4 },
    { "AXION", 0xF0D020, 0x2040A0, 0x202020 },
    { "RAPTA", 0x26262C, 0xFF7010, 0xFF7010 },
    { "ZENITH", 0x20B060, 0xE8E8EC, 0x104030 },
};
static e3d_mesh *m_ship[NSHIPS], *m_ship_hi[NSHIPS], *m_flame, *m_blob, *m_spark, *m_rocket, *m_mine, *m_shield;

/* a hexagonal ring (y, z) of width w, top and bottom heights */
static void hex_ring(float (*r)[2], float w, float top, float bot)
{
    float mid = bot + (top - bot) * 0.45f;
    r[0][0] = bot; r[0][1] = -w * 0.6f;
    r[1][0] = mid; r[1][1] = -w;
    r[2][0] = top; r[2][1] = -w * 0.45f;
    r[3][0] = top; r[3][1] = w * 0.45f;
    r[4][0] = mid; r[4][1] = w;
    r[5][0] = bot; r[5][1] = w * 0.6f;
}

static e3d_mesh *make_ship(const team_t *t)
{
    b_reset();
    uint32_t dark = shade_rgb(t->prim, 0.45f), under = 0x30343C;
    /* fuselage */
    {
        static const float xs[5] = { 2.9f, 1.6f, 0.0f, -1.6f, -2.2f };
        float ring[5 * 6][2];
        hex_ring(&ring[0], 0.06f, 0.12f, 0.06f);
        hex_ring(&ring[6], 0.45f, 0.36f, -0.08f);
        hex_ring(&ring[12], 0.72f, 0.58f, -0.14f);
        hex_ring(&ring[18], 0.80f, 0.55f, -0.14f);
        hex_ring(&ring[24], 0.62f, 0.44f, -0.05f);
        uint32_t band[4 * 6];
        for (int k = 0; k < 4; k++) {
            band[k * 6 + 0] = under;
            band[k * 6 + 1] = k == 1 ? t->sec : t->prim;
            band[k * 6 + 2] = k == 1 || k == 2 ? t->stripe : t->prim;
            band[k * 6 + 3] = k == 1 || k == 2 ? t->stripe : t->prim;
            band[k * 6 + 4] = k == 1 ? t->sec : t->prim;
            band[k * 6 + 5] = under;
        }
        b_loft(xs, (const float (*)[2])ring, 5, 6, 0.f, band, dark);
    }
    /* the two pods */
    for (int s = -1; s <= 1; s += 2) {
        static const float xs[4] = { 2.3f, 1.2f, -1.4f, -2.05f };
        float ring[4 * 6][2];
        hex_ring(&ring[0], 0.10f, 0.10f, 0.0f);
        hex_ring(&ring[6], 0.34f, 0.30f, -0.18f);
        hex_ring(&ring[12], 0.38f, 0.34f, -0.18f);
        hex_ring(&ring[18], 0.30f, 0.28f, -0.10f);
        uint32_t band[3 * 6];
        for (int k = 0; k < 3; k++) {
            band[k * 6 + 0] = under;
            band[k * 6 + 1] = t->sec;
            band[k * 6 + 2] = k == 0 ? t->stripe : t->prim;
            band[k * 6 + 3] = k == 0 ? t->stripe : t->prim;
            band[k * 6 + 4] = t->sec;
            band[k * 6 + 5] = under;
        }
        b_loft(xs, (const float (*)[2])ring, 4, 6, (float)s * 1.35f, band, 0x202024);
        /* a fin on each pod */
        b_box(-2.0f, 0.25f, (float)s * 1.35f - 0.05f, -1.1f, 0.95f, (float)s * 1.35f + 0.05f, t->prim, true);
    }
    /* the wing between them */
    b_box(-1.5f, 0.02f, -1.30f, 0.7f, 0.14f, 1.30f, t->sec, false);
    /* tail fin */
    b_box(-2.1f, 0.45f, -0.06f, -1.0f, 1.10f, 0.06f, t->stripe, true);
    /* canopy */
    {
        static const float xs[3] = { 1.3f, 0.5f, -0.5f };
        float ring[3 * 6][2];
        hex_ring(&ring[0], 0.05f, 0.40f, 0.34f);
        hex_ring(&ring[6], 0.30f, 0.78f, 0.40f);
        hex_ring(&ring[12], 0.26f, 0.68f, 0.45f);
        uint32_t band[2 * 6];
        for (int i = 0; i < 12; i++)
            band[i] = 0x183048;
        b_loft(xs, (const float (*)[2])ring, 3, 6, 0.f, band, 0x183048);
    }
    return b_bake(35.f);
}


/* ── The detailed ships, for those near the camera ─────────────────────────
 * Rounded cross-sections (a superellipse, flatter underneath), swept wings, fins on the
 * pods with a spoiler between them, intakes and nozzles: about 600 triangles. */
#define HR 12      /* points round the fuselage */
#define HP 10      /* round a pod */
typedef struct { float x, cy, rz, top, bot; } sect_t;

/* ring point i of n: from the bottom round the right side, over the top and back */
static void round_ring(float (*r)[2], int n, const sect_t *s)
{
    for (int i = 0; i < n; i++) {
        float a = -(float)M_PI * 0.5f + 2.f * (float)M_PI * ((float)i + 0.5f) / (float)n;
        float sa = sinf(a), ca = cosf(a);
        float ys = sa < 0 ? -powf(-sa, 0.8f) : powf(sa, 0.8f), zs = ca < 0 ? -powf(-ca, 0.8f) : powf(ca, 0.8f);
        r[i][0] = s->cy + ys * (sa < 0 ? s->bot : s->top);
        r[i][1] = zs * s->rz;
    }
}

/* the colour of the face between ring points i and i+1 of n: under, sides, top, a stripe along the top */
static uint32_t ring_rgb(int i, int n, const team_t *t, bool stripe, bool sec_sides)
{
    float a = -(float)M_PI * 0.5f + 2.f * (float)M_PI * ((float)i + 1.f) / (float)n;
    float sa = sinf(a), ca = cosf(a);
    if (sa < -0.45f)
        return 0x2A2E36;
    if (sa > 0.55f)
        return stripe && fabsf(ca) < 0.45f ? t->stripe : t->prim;
    return sec_sides ? t->sec : shade_rgb(t->prim, 0.9f);
}

static void loft_sections(const sect_t *sec, int nr, int n, float zoff, const team_t *t, int stripe_from, int stripe_to,
                          bool sec_sides, uint32_t cap)
{
    static float ring[12 * 12][2];
    static uint32_t band[12 * 12];
    float xs[12];
    for (int k = 0; k < nr; k++) {
        xs[k] = sec[k].x;
        round_ring(&ring[k * n], n, &sec[k]);
    }
    for (int k = 0; k + 1 < nr; k++)
        for (int i = 0; i < n; i++)
            band[k * n + i] = ring_rgb(i, n, t, k >= stripe_from && k <= stripe_to, sec_sides);
    b_loft(xs, (const float (*)[2])ring, nr, n, zoff, band, cap);
}

/* a flat convex plate: outline (a, b) pairs in the plane of axes ia and ib, thickness along the third */
static void b_plate(const float (*p)[2], int n, int ia, int ib, float t0, float t1, uint32_t rgb, uint32_t edge)
{
    int ic = 3 - ia - ib, base = B.nv;
    float c[3] = { 0, 0, 0 };
    for (int e = 0; e < 2; e++)
        for (int i = 0; i < n; i++) {
            float v[3];
            v[ia] = p[i][0];
            v[ib] = p[i][1];
            v[ic] = e ? t1 : t0;
            b_v(v[0], v[1], v[2]);
            for (int q = 0; q < 3; q++)
                c[q] += v[q] / (float)(2 * n);
        }
    for (int i = 1; i + 1 < n; i++) {
        b_tri(base, base + i, base + i + 1, rgb, c);
        b_tri(base + n, base + n + i, base + n + i + 1, rgb, c);
    }
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        b_quad(base + i, base + j, base + n + j, base + n + i, edge, c);
    }
}

static e3d_mesh *make_ship_hi(const team_t *t)
{
    b_reset();
    /* fuselage */
    static const sect_t fus[8] = {
        { 3.05f, 0.10f, 0.05f, 0.04f, 0.03f }, { 2.45f, 0.12f, 0.28f, 0.14f, 0.10f }, { 1.55f, 0.15f, 0.50f, 0.26f, 0.16f },
        { 0.50f, 0.18f, 0.68f, 0.40f, 0.18f }, { -0.60f, 0.20f, 0.78f, 0.42f, 0.20f }, { -1.50f, 0.20f, 0.74f, 0.36f, 0.20f },
        { -2.10f, 0.18f, 0.58f, 0.28f, 0.16f }, { -2.35f, 0.18f, 0.40f, 0.18f, 0.10f },
    };
    loft_sections(fus, 8, HR, 0.f, t, 1, 4, false, 0x18181C);
    /* pods: a stripe on the nose, dark nozzles at the back */
    static const sect_t pod[7] = {
        { 2.50f, 0.08f, 0.06f, 0.05f, 0.04f }, { 2.10f, 0.08f, 0.22f, 0.16f, 0.14f }, { 1.30f, 0.08f, 0.32f, 0.22f, 0.20f },
        { -0.50f, 0.08f, 0.36f, 0.25f, 0.22f }, { -1.60f, 0.08f, 0.34f, 0.24f, 0.20f }, { -2.10f, 0.08f, 0.28f, 0.20f, 0.16f },
        { -2.30f, 0.08f, 0.22f, 0.15f, 0.12f },
    };
    for (int s = -1; s <= 1; s += 2) {
        loft_sections(pod, 7, HP, (float)s * 1.35f, t, 0, 1, true, 0x101014);
        /* an intake on the inner side of the pod's nose */
        b_box(1.2f, 0.0f, (float)s * 1.35f - 0.12f, 1.6f, 0.18f, (float)s * 1.35f + 0.12f, 0x101014, false);
        /* swept fin on the pod */
        static const float fin[4][2] = { { -0.9f, 0.25f }, { -2.15f, 0.25f }, { -2.45f, 0.98f }, { -1.75f, 0.98f } };
        b_plate(fin, 4, 0, 1, (float)s * 1.35f - 0.05f, (float)s * 1.35f + 0.05f, t->prim, t->sec);
    }
    /* swept wings between fuselage and pods */
    for (int s = -1; s <= 1; s += 2) {
        float z0 = 0.55f * (float)s, z1 = 1.15f * (float)s;
        const float w[4][2] = { { 0.9f, z0 }, { -1.4f, z0 }, { -1.7f, z1 }, { 0.1f, z1 } };
        b_plate(w, 4, 0, 2, 0.06f, 0.16f, t->sec, shade_rgb(t->sec, 0.7f));
    }
    /* spoiler across the fin tops, and the tail fin */
    {
        const float sp[4][2] = { { -1.85f, -1.42f }, { -2.45f, -1.42f }, { -2.45f, 1.42f }, { -1.85f, 1.42f } };
        b_plate(sp, 4, 0, 2, 0.90f, 0.97f, t->stripe, shade_rgb(t->stripe, 0.7f));
        static const float tf[4][2] = { { -0.8f, 0.50f }, { -2.10f, 0.45f }, { -2.30f, 1.10f }, { -1.70f, 1.10f } };
        b_plate(tf, 4, 0, 1, -0.05f, 0.05f, t->stripe, t->prim);
    }
    /* canopy: glass with a light band along the top */
    {
        static const sect_t can[5] = {
            { 1.25f, 0.42f, 0.04f, 0.02f, 0.01f }, { 0.80f, 0.46f, 0.22f, 0.14f, 0.04f }, { 0.00f, 0.50f, 0.32f, 0.22f, 0.04f },
            { -0.80f, 0.50f, 0.30f, 0.16f, 0.04f }, { -1.20f, 0.48f, 0.15f, 0.06f, 0.02f },
        };
        team_t glass = { "", 0x1C3048, 0x24405C, 0x5C8CB0 };
        loft_sections(can, 5, HP, 0.f, &glass, 1, 3, true, 0x1C3048);
    }
    return b_bake(40.f);
}

static bool make_meshes(void)
{
    for (int i = 0; i < NSHIPS; i++)
        if (!(m_ship[i] = make_ship(&k_team[i])) || !(m_ship_hi[i] = make_ship_hi(&k_team[i])))
            return false;
    /* engine flame: a square pyramid pointing back (-x) */
    b_reset();
    {
        int a = b_v(0, 0.22f, 0), b = b_v(0, 0, 0.22f), c = b_v(0, -0.22f, 0), d = b_v(0, 0, -0.22f), tip = b_v(-1.f, 0, 0);
        float ctr[3] = { -0.3f, 0, 0 };
        b_tri(a, b, tip, 0xFFFFFF, ctr);
        b_tri(b, c, tip, 0xC0F0FF, ctr);
        b_tri(c, d, tip, 0xFFFFFF, ctr);
        b_tri(d, a, tip, 0xC0F0FF, ctr);
        b_quad(a, b, c, d, 0xFFFFFF, ctr);
    }
    m_flame = b_bake(80.f);
    /* the shadow under a ship: a flat diamond */
    b_reset();
    {
        int a = b_v(2.4f, 0, 0), b = b_v(0, 0, 1.6f), c = b_v(-2.2f, 0, 0), d = b_v(0, 0, -1.6f);
        float n[3] = { 0, 1, 0 };
        b_quad_n(a, b, c, d, 0x101014, n);
    }
    m_blob = b_bake(10.f);
    /* a spark / debris: a tetrahedron */
    b_reset();
    {
        int a = b_v(0.5f, 0, 0), b = b_v(-0.3f, 0.4f, 0.3f), c = b_v(-0.3f, -0.4f, 0.3f), d = b_v(-0.1f, 0, -0.5f);
        float ctr[3] = { 0, 0, 0 };
        b_tri(a, b, c, 0xFFFFFF, ctr);
        b_tri(a, c, d, 0xFFFFFF, ctr);
        b_tri(a, d, b, 0xFFFFFF, ctr);
        b_tri(b, c, d, 0xFFFFFF, ctr);
    }
    m_spark = b_bake(10.f);
    /* rocket: a long thin box with a bright tail */
    b_reset();
    b_box(-0.6f, -0.12f, -0.12f, 0.8f, 0.12f, 0.12f, 0xE0E0E0, false);
    b_box(-1.4f, -0.18f, -0.18f, -0.6f, 0.18f, 0.18f, 0xFF8020, false);
    m_rocket = b_bake(30.f);
    /* mine: an octahedron */
    b_reset();
    {
        int v[6] = { b_v(0.7f, 0, 0), b_v(-0.7f, 0, 0), b_v(0, 0.7f, 0), b_v(0, -0.7f, 0), b_v(0, 0, 0.7f), b_v(0, 0, -0.7f) };
        float ctr[3] = { 0, 0, 0 };
        for (int i = 0; i < 8; i++)
            b_tri(v[i & 1], v[2 + ((i >> 1) & 1)], v[4 + ((i >> 2) & 1)], (i & 1) ? 0xFF2020 : 0x801010, ctr);
    }
    m_mine = b_bake(10.f);
    m_shield = e3d_mesh_sphere(2.7f, 8, 5, 80, 220, 255);
    return m_flame && m_blob && m_spark && m_rocket && m_mine && m_shield;
}

static void free_meshes(void)
{
    for (int i = 0; i < NSHIPS; i++) {
        e3d_mesh_free(m_ship[i]);
        e3d_mesh_free(m_ship_hi[i]);
        m_ship[i] = m_ship_hi[i] = NULL;
    }
    e3d_mesh_free(m_flame);
    e3d_mesh_free(m_blob);
    e3d_mesh_free(m_spark);
    e3d_mesh_free(m_rocket);
    e3d_mesh_free(m_mine);
    e3d_mesh_free(m_shield);
}

/* ── Speed classes ───────────────────────────────────────────────────────── */
typedef struct { const char *name; float vmax, accel, turn; } class_t;
static const class_t k_class[] = {
    { "VENOM", 72.f, 40.f, 1.70f },
    { "FLASH", 86.f, 46.f, 1.85f },
    { "RAPIER", 100.f, 53.f, 2.00f },
    { "PHANTOM", 116.f, 60.f, 2.20f },
};
#define NCLASSES NC(k_class)
static int g_class = 1;
static const class_t *g_cl = &k_class[1];
#define AB_TURN   1.15f      /* extra yaw rate of one airbrake, rad/s */
#define SPEED_KMH 5.6f       /* m/s to the km/h on the speedo (it is the future) */
#define SHIP_HALF 1.25f      /* half width, for the walls */
#define HOVER     0.95f
#define SHIP_HI_DIST 70.f    /* the detailed ship model within this distance of the camera */
#define NEAR_TRACK   90.f    /* the track in full detail (a section every 2 m) this near, every 8 m beyond */

/* ── Ships ─────────────────────────────────────────────────────────────── */
enum { WPN_NONE, WPN_ROCKET, WPN_MINES, WPN_TURBO, WPN_SHIELD, NWPN };
static const char *const wpn_name[NWPN] = { "", "ROCKET", "MINES", "TURBO", "SHIELD" };

typedef struct {
    /* track space */
    double dist;          /* distance flown since the start line (negative on the grid) */
    float u, x;           /* along (0..L), across */
    float yaw;            /* nose against the track direction, + = right */
    float va, vl;         /* velocity in the track frame */
    float yawrate;
    /* state */
    float energy;
    float boost_t, shield_t, stun_t, turbo_t, hit_t, scrape_t;
    int   weapon;
    int   last_pad;
    float pad_t;
    int   team;
    bool  dead, finished, human;
    float dead_t;
    int   lap;            /* laps completed */
    float lap_start, best_lap, finish_t, last_lap;
    int   place;
    /* controls this step */
    float steer, thrust;
    bool  ab_l, ab_r, fire, absorb;
    /* the computer */
    float skill, line_off, ai_fire_t;
    /* looks */
    float roll, nose, hover_ph, flame;
    float pos[3], fwd[3], up[3], right[3];
} ship_t;
static ship_t g_ship[NSHIPS];
#define PLAYER (&g_ship[0])

/* projectiles and mines live in track space too */
typedef struct { float u, x, v, life; int owner; uint8_t type; } proj_t;
#define MAX_PROJ 24
static proj_t g_proj[MAX_PROJ];

typedef struct { float p[3], v[3], life, life0, size; uint32_t rgb; } spark_t;
#define MAX_SPARKS 80
static spark_t g_spark[MAX_SPARKS];

/* race state */
enum { ST_TITLE, ST_COUNT, ST_RACE, ST_FINISH, ST_RESULTS };
static int g_state = ST_TITLE;
static float g_state_t, g_race_t;
static int g_laps = 3;
static bool g_auto, g_show_fps;
static float g_shake;
static char g_msg[32];
static float g_msg_t;
static float g_best_lap_rec, g_best_race_rec;    /* records of this track and class */
static bool g_new_lap_rec, g_new_race_rec;

static void note(const char *m, float t)
{
    snprintf(g_msg, sizeof(g_msg), "%s", m);
    g_msg_t = t;
}

static void spawn_spark(const float *p, float spread, float speed, uint32_t rgb, float size, float life, const float *vel)
{
    for (int i = 0; i < MAX_SPARKS; i++) {
        spark_t *s = &g_spark[i];
        if (s->life > 0)
            continue;
        for (int q = 0; q < 3; q++) {
            s->p[q] = p[q] + (frand() - 0.5f) * spread;
            s->v[q] = (vel ? vel[q] : 0) + (frand() - 0.5f) * speed;
        }
        s->v[1] += speed * 0.3f;
        s->life = s->life0 = life * (0.6f + 0.4f * frand());
        s->size = size;
        s->rgb = rgb;
        return;
    }
}
static void burst(const float *p, int n, uint32_t rgb, float speed, const float *vel)
{
    for (int i = 0; i < n; i++)
        spawn_spark(p, 1.5f, speed, i & 1 ? rgb : 0xFFFFFF, 0.35f + frand() * 0.4f, 0.9f, vel);
}

/* the world pose of a ship from its track state */
static void ship_pose(ship_t *s, float dt)
{
    frame_t f;
    track_frame(s->u, &f);
    s->hover_ph += dt * 5.3f;
    float h = HOVER + 0.07f * sinf(s->hover_ph) + (s->dead ? -0.6f : 0);
    float cy = cosf(s->yaw), sy = sinf(s->yaw);
    float fwd[3], up[3], right[3];
    for (int q = 0; q < 3; q++) {
        s->pos[q] = f.p[q] + f.r[q] * s->x + f.u[q] * h;
        fwd[q] = f.t[q] * cy + f.r[q] * sy;
    }
    v3_norm(fwd);
    /* lean into turns, pitch the nose with thrust */
    float lean = clampf(s->yawrate * 0.22f + s->steer * 0.12f, -0.5f, 0.5f);
    if (s->ab_l != s->ab_r)
        lean += s->ab_r ? 0.12f : -0.12f;
    s->roll += (lean - s->roll) * fminf(1.f, dt * 6.f);
    float nose = s->thrust * 0.03f + (s->boost_t > 0 ? 0.04f : 0) - ((s->ab_l && s->ab_r) ? 0.05f : 0);
    s->nose += (nose - s->nose) * fminf(1.f, dt * 4.f);
    float r0[3];
    v3_cross(r0, fwd, f.u);
    v3_norm(r0);
    float u0[3];
    v3_cross(u0, r0, fwd);
    float cr = cosf(s->roll), sr = sinf(s->roll), cn = cosf(s->nose), sn = sinf(s->nose);
    for (int q = 0; q < 3; q++) {
        up[q] = u0[q] * cr + r0[q] * sr;
        right[q] = r0[q] * cr - u0[q] * sr;
    }
    for (int q = 0; q < 3; q++) {
        float a = fwd[q] * cn + up[q] * sn, b = up[q] * cn - fwd[q] * sn;
        fwd[q] = a;
        up[q] = b;
    }
    memcpy(s->fwd, fwd, sizeof(fwd));
    memcpy(s->up, up, sizeof(up));
    memcpy(s->right, right, sizeof(right));
}

#ifdef SKY_HOST
float g_dbg[NSHIPS][3];
#define DBG(s, k, v) (g_dbg[(s) - g_ship][k] += (v))
#else
#define DBG(s, k, v) ((void)0)
#endif
static void ship_damage(ship_t *s, float amount)
{
    if (s->dead || s->finished)
        return;
    s->energy -= amount;
    s->hit_t = 0.5f;
    if (s->energy <= 0) {
        s->energy = 0;
        s->dead = true;
        s->dead_t = 0;
        burst(s->pos, 30, 0xFF6020, 26.f, NULL);
        snd_boom(s->human ? 0.5f : 0.25f);
        if (s->human) {
            note("ELIMINATED", 4.f);
            g_shake = 1.f;
        }
    }
}

static void place_on_grid(ship_t *s, int slot)
{
    float hw = g_tr[0].hw;
    s->dist = -14.0 - 11.0 * (slot / 2) - (slot & 1) * 5.5;
    s->u = wrapu((float)s->dist);
    s->x = (slot & 1) ? hw * 0.4f : -hw * 0.4f;
    s->yaw = s->va = s->vl = s->yawrate = 0;
}

/* one physics step for one ship */
static void ship_step(ship_t *s, float dt)
{
    const class_t *cl = g_cl;
    frame_t f;
    track_frame(s->u, &f);
    if (s->dead) {
        s->va *= expf(-1.5f * dt);
        s->vl *= expf(-2.f * dt);
        s->dead_t += dt;
    }
    bool racing = g_state == ST_RACE || g_state == ST_FINISH || g_state == ST_RESULTS || g_state == ST_TITLE;
    float steer = s->dead ? 0 : s->steer, thrust = (s->dead || !racing) ? 0 : s->thrust;
    /* yaw rate: steering plus the airbrakes */
    float turn = cl->turn * steer;
    if (s->ab_l && !s->ab_r) turn -= AB_TURN;
    if (s->ab_r && !s->ab_l) turn += AB_TURN;
    s->yawrate += (turn - s->yawrate) * fminf(1.f, dt * 7.f);
    s->yaw += s->yawrate * dt;
    /* velocity in the ship's frame */
    float cy = cosf(s->yaw), sy = sinf(s->yaw);
    float vf = s->va * cy + s->vl * sy, vs = -s->va * sy + s->vl * cy;
    float vmax = cl->vmax * (s->turbo_t > 0 ? 1.28f : 1.f) * (s->stun_t > 0 ? 0.55f : 1.f) * (s->dead ? 0 : 1);
    float acc = cl->accel * (s->turbo_t > 0 ? 1.6f : 1.f);
    if (thrust > 0 && vf < vmax)
        vf += acc * thrust * (1.f - vf / vmax) * dt;
    else if (thrust <= 0)
        vf -= vf * 0.30f * dt;
    if (vf > vmax)        /* the speed above the limit (pads) bleeds off */
        vf += (vmax - vf) * (s->boost_t > 0 ? 0.35f : 1.1f) * dt;
    if (s->ab_l) vf -= vf * 0.28f * dt;
    if (s->ab_r) vf -= vf * 0.28f * dt;
    /* hills: a share of gravity */
    vf -= 9.8f * f.slope * 0.55f * dt;
    /* grip: the sideways speed fades (less with an airbrake: it drifts) */
    float grip = (s->ab_l != s->ab_r) ? 6.0f : 9.5f;
    vs -= vs * fminf(1.f, grip * dt);
    s->va = vf * cy - vs * sy;
    s->vl = vf * sy + vs * cy;
    /* advance in track space; the track turns under the ship */
    float denom = clampf(1.f - f.curv * s->x, 0.4f, 1.6f);
    float du = s->va * dt / denom;
    float th = f.curv * du;
    float ct = cosf(th), st = sinf(th);
    float va = s->va * ct + s->vl * st, vl = -s->va * st + s->vl * ct;
    s->va = va;
    s->vl = vl;
    s->yaw -= th;
    s->x += s->vl * dt;
    s->dist += du;
    s->u = wrapu(s->u + du);
    /* walls */
    float lim = f.hw - SHIP_HALF;
    s->scrape_t = fmaxf(0, s->scrape_t - dt);
    if (fabsf(s->x) > lim) {
        float side = s->x > 0 ? 1.f : -1.f;
        float into = s->vl * side;
        s->x = lim * side;
        if (into > 0) {
            s->vl = -s->vl * 0.35f;
            float hit = into;
            s->va *= 1.f - fminf(0.35f, hit * 0.018f);
            s->yaw *= 0.55f;
            s->yawrate *= 0.3f;
            if (hit > 5.f) {
                float dmg = clampf(hit * 0.7f, 1.5f, 14.f);
                if (s->shield_t <= 0)
                    ship_damage(s, dmg);
                DBG(s, 0, dmg);
                float p[3];
                track_point(s->u, side * (f.hw - 0.2f), 0.6f, p);
                burst(p, (int)clampf(hit * 0.6f, 3, 12), 0xFFC040, 10.f, NULL);
                if (s->human) {
                    snd_wall(clampf(hit * 0.02f, 0.1f, 0.45f));
                    g_shake = fmaxf(g_shake, clampf(hit * 0.03f, 0.1f, 0.8f));
                }
            }
        }
        s->scrape_t = 0.1f;
        s->va -= s->va * 0.25f * dt;
    }
    /* the nose can't swing round past the wall */
    if (s->yaw > 1.15f) { s->yaw = 1.15f; s->yawrate = fminf(s->yawrate, 0); }
    if (s->yaw < -1.15f) { s->yaw = -1.15f; s->yawrate = fmaxf(s->yawrate, 0); }
    if (s->va < -5.f)
        s->va = -5.f;
    /* the recharge lane */
    if (!s->dead && in_charge(s->u) && s->x < -f.hw + 3.6f && s->energy < 100.f)
        s->energy = fminf(100.f, s->energy + 22.f * dt);
    /* timers */
    s->boost_t = fmaxf(0, s->boost_t - dt);
    s->shield_t = fmaxf(0, s->shield_t - dt);
    s->stun_t = fmaxf(0, s->stun_t - dt);
    s->turbo_t = fmaxf(0, s->turbo_t - dt);
    s->hit_t = fmaxf(0, s->hit_t - dt);
    s->pad_t = fmaxf(0, s->pad_t - dt);
}

static int pick_weapon(const ship_t *s)
{
    /* the leaders get fewer turbos, the back of the field more */
    float r = frand();
    float turbo = s->place <= 2 ? 0.12f : 0.30f;
    if (r < turbo)
        return WPN_TURBO;
    r = frand();
    return r < 0.40f ? WPN_ROCKET : r < 0.72f ? WPN_MINES : WPN_SHIELD;
}

static void pads_check(ship_t *s)
{
    if (s->dead)
        return;
    for (int i = 0; i < g_npads; i++) {
        const pad_t *p = &g_pad[i];
        float du = wrapd(s->u - p->u);
        if (fabsf(du) > PAD_LEN * 0.5f + 0.5f || fabsf(s->x - p->x) > PAD_W * 0.5f + 0.4f)
            continue;
        if (s->last_pad == i && s->pad_t > 0)
            continue;
        s->last_pad = i;
        s->pad_t = 1.5f;
        if (p->type == PAD_BOOST) {
            float cy = cosf(s->yaw);
            s->va += 26.f * cy;
            s->boost_t = 1.6f;
            if (s->human) {
                snd_boost();
                g_shake = fmaxf(g_shake, 0.15f);
            }
        } else if (s->weapon == WPN_NONE) {
            s->weapon = pick_weapon(s);
            if (s->human) {
                snd_pickup();
                note(wpn_name[s->weapon], 1.2f);
            }
        }
    }
}

static void fire_weapon(ship_t *s)
{
    int w = s->weapon;
    if (w == WPN_NONE || s->dead)
        return;
    s->weapon = WPN_NONE;
    if (w == WPN_TURBO) {
        s->turbo_t = 2.6f;
        s->boost_t = 2.6f;
        s->va += 12.f;
        if (s->human) snd_boost();
    } else if (w == WPN_SHIELD) {
        s->shield_t = 5.f;
        if (s->human) snd_shield();
    } else if (w == WPN_ROCKET) {
        for (int i = 0; i < MAX_PROJ; i++)
            if (g_proj[i].life <= 0) {
                g_proj[i] = (proj_t){ wrapu(s->u + 4.f), s->x, fmaxf(s->va, 20.f) + 75.f, 2.6f, (int)(s - g_ship), WPN_ROCKET };
                break;
            }
        if (s->human) snd_rocket();
        else if (fabsf(wrapd(s->u - PLAYER->u)) < 80.f) sfx(700, 200, 300, WAVE_NOISE, 0.12f);
    } else if (w == WPN_MINES) {
        int n = 0;
        for (int i = 0; i < MAX_PROJ && n < 3; i++)
            if (g_proj[i].life <= 0) {
                g_proj[i] = (proj_t){ wrapu(s->u - 4.f - (float)n * 1.5f), clampf(s->x + ((float)n - 1.f) * 1.8f, -6.f, 6.f), 0,
                                      22.f, (int)(s - g_ship), WPN_MINES };
                n++;
            }
        if (s->human) snd_mine();
    }
}

static void absorb_weapon(ship_t *s)
{
    if (s->weapon == WPN_NONE || s->dead)
        return;
    s->weapon = WPN_NONE;
    s->energy = fminf(100.f, s->energy + 18.f);
    if (s->human) {
        snd_absorb();
        note("ABSORBED", 1.f);
    }
}

static void hit_ship(ship_t *s, int type)
{
    float p[3];
    memcpy(p, s->pos, sizeof(p));
    if (s->shield_t > 0) {
        burst(p, 8, 0x60E0FF, 12.f, NULL);
        return;
    }
    burst(p, 16, type == WPN_ROCKET ? 0xFF8020 : 0xFF2020, 18.f, NULL);
    ship_damage(s, type == WPN_ROCKET ? 17.f : 9.f);
    DBG(s, type == WPN_ROCKET ? 1 : 2, type == WPN_ROCKET ? 17.f : 9.f);
    s->stun_t = type == WPN_ROCKET ? 1.0f : 0.6f;
    s->yawrate += (frand() - 0.5f) * 3.f;
    if (s->human) {
        g_shake = 0.9f;
        snd_boom(0.35f);
    } else if (fabsf(wrapd(s->u - PLAYER->u)) < 90.f)
        snd_boom(0.15f);
}

static void proj_step(float dt)
{
    for (int i = 0; i < MAX_PROJ; i++) {
        proj_t *p = &g_proj[i];
        if (p->life <= 0)
            continue;
        p->life -= dt;
        p->u = wrapu(p->u + p->v * dt);
        for (int k = 0; k < NSHIPS; k++) {
            ship_t *s = &g_ship[k];
            if (s->dead || (k == p->owner && p->type == WPN_ROCKET))
                continue;
            float du = wrapd(s->u - p->u), dx = s->x - p->x;
            float ru = p->type == WPN_ROCKET ? 2.8f : 1.8f, rx = p->type == WPN_ROCKET ? 1.7f : 1.4f;
            if (fabsf(du) < ru && fabsf(dx) < rx) {
                hit_ship(s, p->type);
                p->life = 0;
                break;
            }
        }
        if (p->life <= 0 && p->type == WPN_ROCKET) {
            float q[3];
            track_point(p->u, p->x, 1.0f, q);
            burst(q, 6, 0xFF8020, 10.f, NULL);
        }
    }
}

static void collide_ships(void)
{
    for (int i = 0; i < NSHIPS; i++)
        for (int j = i + 1; j < NSHIPS; j++) {
            ship_t *a = &g_ship[i], *b = &g_ship[j];
            if (a->dead || b->dead)
                continue;
            float du = wrapd(b->u - a->u), dx = b->x - a->x;
            if (fabsf(du) > 4.4f || fabsf(dx) > 2.3f)
                continue;
            /* push apart across the track; the one behind loses speed into the one ahead */
            float push = (2.3f - fabsf(dx)) * 0.5f, side = dx >= 0 ? 1.f : -1.f;
            a->x -= push * side;
            b->x += push * side;
            float rv = b->vl - a->vl;
            if (rv * side < 0) {
                a->vl += rv * 0.5f;
                b->vl -= rv * 0.5f;
            }
            if (fabsf(dx) < 1.6f) {
                ship_t *back = du > 0 ? a : b, *front = du > 0 ? b : a;
                if (back->va > front->va) {
                    float d = back->va - front->va;
                    back->va -= d * 0.6f;
                    front->va += d * 0.3f;
                }
            }
            if ((a->human || b->human) && fabsf(rv) > 3.f) {
                snd_wall(0.18f);
                g_shake = fmaxf(g_shake, 0.25f);
            }
        }
}

/* the computer: a racing line towards the inside of the bends ahead, airbrakes when the
   turn needs more than steering, lifts off for the tightest ones */
static void ai_drive(ship_t *s, float dt)
{
    const class_t *cl = g_cl;
    s->steer = 0;
    s->thrust = 0;
    s->ab_l = s->ab_r = false;
    if (s->dead)
        return;
    frame_t f;
    track_frame(s->u, &f);
    float v = fmaxf(s->va, 10.f);
    float look = fmaxf(18.f, v * 0.55f);
    frame_t fa;
    track_frame(s->u + look, &fa);
    float c_ahead = 0;
    for (int i = 1; i <= 3; i++) {
        frame_t q;
        track_frame(s->u + v * 0.35f * (float)i, &q);
        c_ahead += q.curv / 3.f;
    }
    float lim = f.hw - 2.4f;
    float xt = clampf(c_ahead * 420.f, -lim, lim) + s->line_off;
    /* dodge a ship just ahead in the same lane */
    for (int k = 0; k < NSHIPS; k++) {
        const ship_t *o = &g_ship[k];
        if (o == s || o->dead)
            continue;
        float du = wrapd(o->u - s->u);
        if (du > 0 && du < 18.f && fabsf(o->x - s->x) < 2.6f && o->va < s->va + 2.f)
            xt += (o->x > s->x ? -1.f : 1.f) * 3.2f;
    }
    /* go for speed pads that are close to the line */
    for (int i = 0; i < g_npads; i++) {
        float du = wrapd(g_pad[i].u - s->u);
        if (du > 5.f && du < 60.f && fabsf(g_pad[i].x - xt) < 4.5f &&
            (g_pad[i].type == PAD_BOOST || s->weapon == WPN_NONE)) {
            xt = g_pad[i].x;
            break;
        }
    }
    /* low on energy: through the recharge lane */
    if (s->energy < 55.f && (in_charge(s->u) || (wrapd(g_td->charge0 - s->u) > 0 && wrapd(g_td->charge0 - s->u) < 70.f)))
        xt = -lim;
    xt = clampf(xt, -lim, lim);
    float denom = clampf(1.f - f.curv * s->x, 0.4f, 1.6f);
    float yaw_t = atan2f(xt - s->x, look) * 0.9f;
    float want = f.curv * s->va / denom + (yaw_t - s->yaw) * 3.2f - s->vl * 0.05f;
    float st = want / cl->turn;
    if (st > 1.15f) s->ab_r = true;
    if (st < -1.15f) s->ab_l = true;
    s->steer = clampf(st, -1.f, 1.f);
    /* speed: the class top speed scaled by skill and a little rubber band to the player */
    float lead = (float)(s->dist - PLAYER->dist);
    float rubber = (g_auto || PLAYER->dead || PLAYER->finished) ? 1.f : clampf(1.f - lead * 0.00035f, 0.93f, 1.05f);
    float vt = cl->vmax * s->skill * rubber;
    float cmax = 0;
    for (int i = 1; i <= 4; i++) {
        frame_t q;
        track_frame(s->u + v * 0.3f * (float)i, &q);
        cmax = fmaxf(cmax, fabsf(q.curv));
    }
    float vlim = (cl->turn + AB_TURN) * 0.92f / fmaxf(cmax, 1e-4f);
    if (vlim < vt) vt = vlim;
    s->thrust = s->va < vt ? 1.f : 0.f;
    if (s->va > vt + 8.f)
        s->ab_l = s->ab_r = true;
    /* weapons */
    s->ai_fire_t -= dt;
    if (s->weapon != WPN_NONE && s->ai_fire_t <= 0) {
        s->ai_fire_t = 0.4f + frand() * 0.6f;
        bool use = false;
        if (s->weapon == WPN_TURBO)
            use = cmax < 1.f / 300.f;
        else if (s->weapon == WPN_SHIELD)
            use = s->energy < 60.f || frand() < 0.1f;
        else if (s->energy < 30.f) {
            absorb_weapon(s);
            return;
        } else
            for (int k = 0; k < NSHIPS; k++) {
                const ship_t *o = &g_ship[k];
                if (o == s || o->dead)
                    continue;
                float du = wrapd(o->u - s->u);
                if (s->weapon == WPN_ROCKET && du > 6.f && du < 90.f && fabsf(o->x - s->x) < 2.2f)
                    use = frand() < 0.7f;
                if (s->weapon == WPN_MINES && du < -4.f && du > -45.f)
                    use = frand() < 0.6f;
            }
        if (use)
            fire_weapon(s);
    }
}

/* ── Records and settings ──────────────────────────────────────────────── */
#define DATA_DIR "/sd/data/skyrift"
#define REC_PATH DATA_DIR "/records.txt"
#define SET_PATH DATA_DIR "/settings.txt"
static float g_rec[NTRACKS][4][2];          /* best lap, best race */
enum { CTRL_PAD, CTRL_ROLLER, NCTRL };
static const char *const ctrl_name[NCTRL] = { "Pad / stick", "Roller" };
static int g_ctrl = -1;
static bool g_autothrust;
static int g_team;                          /* the player's team */

static void records_io(bool save)
{
    if (save && g_auto)
        return;
    if (save) {
        mkdir("/sd/data", 0777);
        mkdir(DATA_DIR, 0777);
        FILE *f = fopen(REC_PATH, "w");
        if (!f)
            return;
        for (int t = 0; t < NTRACKS; t++)
            for (int c = 0; c < NCLASSES; c++)
                fprintf(f, "%d %d %.3f %.3f\n", t, c, (double)g_rec[t][c][0], (double)g_rec[t][c][1]);
        fclose(f);
    } else {
        FILE *f = fopen(REC_PATH, "r");
        if (!f)
            return;
        int t, c;
        float a, b;
        while (fscanf(f, "%d %d %f %f", &t, &c, &a, &b) == 4)
            if (t >= 0 && t < NTRACKS && c >= 0 && c < NCLASSES) {
                g_rec[t][c][0] = a;
                g_rec[t][c][1] = b;
            }
        fclose(f);
    }
}

static void settings_io(bool save)
{
    if (save) {
        mkdir("/sd/data", 0777);
        mkdir(DATA_DIR, 0777);
        FILE *f = fopen(SET_PATH, "w");
        if (f) {
            fprintf(f, "track=%d\nclass=%d\nlaps=%d\ncontrols=%d\nauto=%d\nteam=%d\nres=%d\n", g_track_i, g_class, g_laps,
                    g_ctrl, g_autothrust ? 1 : 0, g_team, g_res);
            fclose(f);
        }
    } else {
        FILE *f = fopen(SET_PATH, "r");
        if (f) {
            int v;
            if (fscanf(f, "track=%d ", &v) == 1 && v >= 0 && v < NTRACKS) g_track_i = v;
            if (fscanf(f, "class=%d ", &v) == 1 && v >= 0 && v < NCLASSES) g_class = v;
            if (fscanf(f, "laps=%d ", &v) == 1 && v >= 1 && v <= 9) g_laps = v;
            if (fscanf(f, "controls=%d ", &v) == 1 && v >= 0 && v < NCTRL) g_ctrl = v;
            if (fscanf(f, "auto=%d ", &v) == 1) g_autothrust = v != 0;
            if (fscanf(f, "team=%d ", &v) == 1 && v >= 0 && v < NSHIPS) g_team = v;
            if (fscanf(f, "res=%d", &v) == 1 && v >= 0 && v < NRES) g_res = v;
            fclose(f);
        }
    }
    g_td = &k_track[g_track_i];
    g_cl = &k_class[g_class];
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

/* ── The race ──────────────────────────────────────────────────────────── */
static int g_order[NSHIPS];      /* ships by place */
static int g_watch;              /* the ship the camera follows */
static float g_cam_f[3] = { 1, 0, 0 }, g_cam_p[3], g_cam_h, g_cam_pitch, g_focal = 330.f;
static float g_tv_u, g_tv_x;     /* title: the trackside camera */
static int g_beeps;

static void race_reset(void)
{
    memset(g_ship, 0, sizeof(g_ship));
    memset(g_proj, 0, sizeof(g_proj));
    memset(g_spark, 0, sizeof(g_spark));
    /* the player starts at the back, the other teams in front */
    int t = 0;
    for (int i = 0; i < NSHIPS; i++) {
        ship_t *s = &g_ship[i];
        if (i == 0)
            s->team = g_team;
        else {
            if (t == g_team)
                t++;
            s->team = t++;
        }
        s->human = i == 0 && !g_auto;
        s->energy = 100.f;
        s->last_pad = -1;
        s->skill = 0.90f + 0.018f * (float)(NSHIPS - i) + frand() * 0.02f;
        s->line_off = (frand() - 0.5f) * 2.4f;
        s->hover_ph = frand() * 6.f;
        place_on_grid(s, i == 0 ? NSHIPS - 1 : i - 1);
        g_order[i] = i;
    }
    if (g_auto)
        g_ship[0].skill = 0.985f;
    g_race_t = 0;
    g_state_t = 0;
    g_beeps = -1;
    g_new_lap_rec = g_new_race_rec = false;
    g_best_lap_rec = g_rec[g_track_i][g_class][0];
    g_best_race_rec = g_rec[g_track_i][g_class][1];
    g_watch = 0;
    g_cam_f[0] = g_tr[0].t[0];
    g_cam_f[1] = g_tr[0].t[1];
    g_cam_f[2] = g_tr[0].t[2];
    for (int i = 0; i < NSHIPS; i++)
        ship_pose(&g_ship[i], 0);
}

static void start_race(void)
{
    race_reset();
    g_state = ST_COUNT;
    g_state_t = 0;
    g_msg_t = 0;
}

/* the title screen: the field races on its own, the camera watches from beside the track */
static void start_attract(void)
{
    bool a = g_auto;
    g_auto = true;
    race_reset();
    g_auto = a;
    g_ship[0].human = false;
    g_state = ST_TITLE;
    g_state_t = 0;
    g_watch = NSHIPS - 1;
    g_tv_u = -1e9f;
}

static void sort_places(void)
{
    for (int i = 0; i < NSHIPS; i++)
        g_order[i] = i;
    for (int i = 1; i < NSHIPS; i++) {
        int k = g_order[i], j = i - 1;
        while (j >= 0) {
            const ship_t *a = &g_ship[g_order[j]], *b = &g_ship[k];
            /* b goes before a? finished ones by time, then the living by distance, the dead last */
            bool before;
            if (a->finished != b->finished)
                before = b->finished;
            else if (a->finished)
                before = b->finish_t < a->finish_t;
            else if (a->dead != b->dead)
                before = !b->dead;
            else
                before = b->dist > a->dist;
            if (!before)
                break;
            g_order[j + 1] = g_order[j];
            j--;
        }
        g_order[j + 1] = k;
    }
    for (int i = 0; i < NSHIPS; i++)
        g_ship[g_order[i]].place = i + 1;
}

static void laps_check(ship_t *s)
{
    if (s->finished || s->dead || g_state == ST_COUNT)
        return;
    int done = s->dist < 0 ? 0 : (int)(s->dist / (double)g_len);
    if (done <= s->lap)
        return;
    float lt = g_race_t - s->lap_start;
    s->lap = done;
    s->lap_start = g_race_t;
    s->last_lap = lt;
    if (s->best_lap <= 0 || lt < s->best_lap)
        s->best_lap = lt;
    if (s == PLAYER && g_state == ST_RACE) {
        if (lt > 0 && (g_best_lap_rec <= 0 || lt < g_best_lap_rec)) {
            g_best_lap_rec = lt;
            g_new_lap_rec = true;
            g_rec[g_track_i][g_class][0] = lt;
            records_io(true);
        }
        char m[32];
        if (s->lap == g_laps - 1)
            note("FINAL LAP", 2.f);
        else if (s->lap < g_laps) {
            snprintf(m, sizeof(m), "LAP %d", s->lap + 1);
            note(m, 1.5f);
        }
        snd_lap();
    }
    if (s->lap >= g_laps) {
        s->finished = true;
        s->finish_t = g_race_t;
        if (s == PLAYER && g_state == ST_RACE) {
            if (g_best_race_rec <= 0 || g_race_t < g_best_race_rec) {
                g_best_race_rec = g_race_t;
                g_new_race_rec = true;
                g_rec[g_track_i][g_class][1] = g_race_t;
                records_io(true);
            }
            g_state = ST_FINISH;
            g_state_t = 0;
            s->human = false;      /* the computer brings it home */
        }
    }
}

/* ── Input ─────────────────────────────────────────────────────────────── */
typedef struct {
    bool start, pause, tap, fire, absorb;
    float steer, thrust;
    bool ab_l, ab_r;
} input_t;
static uint32_t s_prev_btn = ~0u;
static int s_touch_n;
static bool s_touch_used;
static float s_pad_steer, s_pot_f = -1.f;
#define POT_MID   32768.f
#define POT_RANGE 26000.f

static float stick_curve(int raw)
{
    const float dz = 0.12f;
    float v = clampf((float)raw / 30000.f, -1.f, 1.f), m = fabsf(v);
    if (m <= dz)
        return 0;
    float u = (m - dz) / (1.f - dz);
    u = u * (0.25f + 0.75f * u);
    return v < 0 ? -u : u;
}

/* touch zones */
#define TZ_Y (H * 3 / 5)
static input_t read_input(papp_surface_t *surf, float dt)
{
    input_t a = { 0 };
    papp_gamepad_t pad = { 0 };
    uint32_t held = papp_input_gamepad(0, &pad) ? pad.buttons : 0;
    uint32_t press = held & ~s_prev_btn;
    s_prev_btn = held;
    if (press & (PAPP_BTN_A | PAPP_BTN_START))
        a.start = true;
    if (press & (PAPP_BTN_START | PAPP_BTN_SELECT))
        a.pause = true;
    if (press & PAPP_BTN_B)
        a.fire = true;
    if (press & PAPP_BTN_X)
        a.absorb = true;
    float thrust = 0;
    if (held & (PAPP_BTN_A | PAPP_BTN_R2 | PAPP_BTN_UP))
        thrust = 1.f;
    if (pad.rt > 2000)
        thrust = fmaxf(thrust, (float)pad.rt / 65535.f);
    if (held & PAPP_BTN_L1) a.ab_l = true;
    if (held & PAPP_BTN_R1) a.ab_r = true;
    if (held & (PAPP_BTN_DOWN | PAPP_BTN_L2))
        a.ab_l = a.ab_r = true;
    float dig = 0;
    if (held & PAPP_BTN_LEFT) dig -= 1.f;
    if (held & PAPP_BTN_RIGHT) dig += 1.f;
    /* touch: bottom thirds steer, bottom middle fires, top left absorbs, top right pauses */
    papp_touch_point_t tp[3];
    int n = papp_disp_touch(surf, tp, 3);
    if (n > 0) {
        s_touch_used = true;
        for (int i = 0; i < n && i < 3; i++) {
            int x = tp[i].x, y = tp[i].y;
            bool first = s_touch_n == 0;
            if (x > W - 50 && y < 36) {
                if (first) a.pause = true;
            } else if (g_state == ST_TITLE || g_state == ST_RESULTS) {
                if (first) a.tap = true;
            } else if (y >= TZ_Y) {
                if (x < W / 3) dig = -1.f;
                else if (x > W * 2 / 3) dig = 1.f;
                else if (first) a.fire = true;
            } else if (x < 90 && y < 70) {
                if (first) a.absorb = true;
            } else if (first)
                a.tap = true;
        }
    }
    s_touch_n = n > 0 ? n : 0;
    /* pad left/right ramp in quickly; the stick is analog; the roller is absolute */
    float ramp = (dig == 0 || dig * s_pad_steer < 0 ? 9.f : 5.f) * dt;
    s_pad_steer += clampf(dig - s_pad_steer, -ramp, ramp);
    float steer = s_pad_steer;
    if (dig == 0 && (pad.lx > 3600 || pad.lx < -3600))
        steer = stick_curve(pad.lx);
    if (g_ctrl == CTRL_ROLLER && dig == 0) {
        int pv = papp_input_analog(PAPP_ANALOG_PADDLE);
        if (pv >= 0) {
            if (s_pot_f < 0) s_pot_f = (float)pv;
            s_pot_f += ((float)pv - s_pot_f) * fminf(1.f, dt * 30.f);
            float v = clampf((s_pot_f - POT_MID) / POT_RANGE, -1.f, 1.f);
            const float dz = 0.04f;
            steer = fabsf(v) < dz ? 0 : (v - copysignf(dz, v)) / (1.f - dz);
        }
    }
    a.steer = steer;
    if (g_autothrust || s_touch_used)
        thrust = (a.ab_l && a.ab_r) ? 0.f : 1.f;
    a.thrust = thrust;
    return a;
}

/* ── Update ─────────────────────────────────────────────────────────────── */
static void update_camera(float dt)
{
    const ship_t *s = &g_ship[g_watch];
    frame_t f;
    track_frame(s->u, &f);
    float cy = cosf(s->yaw * 0.4f), sy = sinf(s->yaw * 0.4f);
    float tf[3];
    for (int q = 0; q < 3; q++)
        tf[q] = f.t[q] * cy + f.r[q] * sy;
    v3_norm(tf);
    float k = fminf(1.f, dt * 6.f);
    for (int q = 0; q < 3; q++)
        g_cam_f[q] += (tf[q] - g_cam_f[q]) * k;
    v3_norm(g_cam_f);
    float back = 7.2f, upk = 2.5f;
    if (g_state == ST_TITLE) {
        /* a trackside camera ahead of the leader; when it has passed, jump further on */
        float du = wrapd(s->u - g_tv_u);
        if (du > 12.f || du < -120.f) {
            g_tv_u = wrapu(s->u + 55.f + frand() * 35.f);
            g_tv_x = (frand() < 0.5f ? -1.f : 1.f) * (g_tr[(int)(g_tv_u / STEP) % g_ns].hw + 1.5f + frand() * 4.f);
        }
        float p[3];
        track_point(g_tv_u, g_tv_x, 2.2f + frand() * 0.02f + fabsf(g_tv_x) * 0.12f, p);
        memcpy(g_cam_p, p, sizeof(p));
        float d[3] = { s->pos[0] - p[0], s->pos[1] - p[1], s->pos[2] - p[2] };
        g_cam_h = atan2f(d[2], d[0]) * DEG;
        g_cam_pitch = atan2f(d[1], sqrtf(d[0] * d[0] + d[2] * d[2])) * DEG;
        g_focal = clampf(260.f + sqrtf(v3_dot(d, d)) * 3.5f, 280.f, 520.f);
        return;
    }
    float upv[3] = { f.u[0] * 0.5f, 0.5f + f.u[1] * 0.5f, f.u[2] * 0.5f };
    v3_norm(upv);
    for (int q = 0; q < 3; q++)
        g_cam_p[q] = s->pos[q] - g_cam_f[q] * back + upv[q] * upk;
    if (g_shake > 0) {
        for (int q = 0; q < 3; q++)
            g_cam_p[q] += (frand() - 0.5f) * g_shake * 0.35f;
    }
    g_cam_h = atan2f(g_cam_f[2], g_cam_f[0]) * DEG;
    g_cam_pitch = asinf(clampf(g_cam_f[1], -1.f, 1.f)) * DEG - 7.5f;
    float sp = clampf(s->va / 120.f, 0, 1.3f);
    float want = 335.f - 70.f * sp - (s->boost_t > 0 ? 35.f : 0);
    g_focal += (want - g_focal) * fminf(1.f, dt * 3.f);
}

static void sparks_step(float dt)
{
    for (int i = 0; i < MAX_SPARKS; i++) {
        spark_t *s = &g_spark[i];
        if (s->life <= 0)
            continue;
        s->life -= dt;
        for (int q = 0; q < 3; q++)
            s->p[q] += s->v[q] * dt;
        s->v[1] -= 14.f * dt;
        for (int q = 0; q < 3; q++)
            s->v[q] *= 1.f - fminf(1.f, 1.5f * dt);
    }
}

static void update(const input_t *in, float dt)
{
    g_state_t += dt;
    if (g_msg_t > 0)
        g_msg_t -= dt;
    g_shake = fmaxf(0, g_shake - dt * 1.8f);
    ship_t *pl = PLAYER;
    if (g_state == ST_COUNT) {
        int b = (int)g_state_t;
        if (b > g_beeps && b <= 3) {
            g_beeps = b;
            snd_beep(b == 3);
        }
        if (g_state_t >= 3.f) {
            g_state = ST_RACE;
            g_race_t = 0;
            note("GO", 1.f);
            /* a launch boost for full thrust at the start */
            for (int i = 0; i < NSHIPS; i++)
                if (g_ship[i].thrust > 0.9f)
                    g_ship[i].va += g_ship[i].human ? 14.f : 6.f + frand() * 8.f;
        }
    }
    if (g_state == ST_RACE || g_state == ST_FINISH || g_state == ST_RESULTS)
        g_race_t += dt;
    /* controls */
    for (int i = 0; i < NSHIPS; i++) {
        ship_t *s = &g_ship[i];
        if (s->human && (g_state == ST_RACE || g_state == ST_COUNT)) {
            s->steer = in->steer;
            s->thrust = in->thrust;
            s->ab_l = in->ab_l;
            s->ab_r = in->ab_r;
            if (g_state == ST_RACE) {
                if (in->fire) fire_weapon(s);
                if (in->absorb) absorb_weapon(s);
            }
        } else {
            ai_drive(s, dt);
            if (g_state == ST_COUNT)
                s->thrust = g_state_t > 2.2f ? 1.f : 0.f;
        }
    }
    /* physics in small steps */
    int n = (int)ceilf(dt / (1.f / 120.f));
    if (n < 1) n = 1;
    float h = dt / (float)n;
    for (int k = 0; k < n; k++) {
        for (int i = 0; i < NSHIPS; i++)
            ship_step(&g_ship[i], h);
        collide_ships();
        for (int i = 0; i < NSHIPS; i++)
            pads_check(&g_ship[i]);
    }
    proj_step(dt);
    for (int i = 0; i < NSHIPS; i++) {
        laps_check(&g_ship[i]);
        ship_pose(&g_ship[i], dt);
        ship_t *s = &g_ship[i];
        s->flame += ((s->dead ? 0 : 0.35f + s->thrust * 0.6f + (s->boost_t > 0 ? 0.6f : 0)) - s->flame) * fminf(1.f, dt * 8.f);
    }
    sort_places();
    sparks_step(dt);
    /* the player out: watch the race on, then the results */
    if (g_state == ST_RACE && pl->dead && pl->dead_t > 0.f) {
        g_state = ST_FINISH;
        g_state_t = 0;
    }
    if (g_state == ST_FINISH && g_state_t > 3.5f) {
        g_state = ST_RESULTS;
        g_state_t = 0;
    }
    if (g_state == ST_TITLE) {
        g_watch = g_order[0];
        /* the attract race goes round for ever */
        for (int i = 0; i < NSHIPS; i++)
            if (g_ship[i].dead && g_ship[i].dead_t > 3.f) {
                g_ship[i].dead = false;
                g_ship[i].energy = 100.f;
            }
    }
    update_camera(dt);
    /* sound */
    const ship_t *w = &g_ship[g_watch];
    bool loud = g_state != ST_TITLE && !w->dead;
    s_eng_f = 70.f + fmaxf(w->va, 0) * 2.4f + w->thrust * 25.f;
    s_eng_vol = loud ? 0.14f + 0.12f * w->thrust : (g_state == ST_TITLE ? 0.04f : 0);
    s_boost_vol = loud && (w->boost_t > 0 || w->turbo_t > 0) ? 0.12f : 0;
    s_wind_vol = loud ? clampf(w->va / 140.f, 0, 1) * 0.25f : 0;
    s_scrape_vol = loud && w->scrape_t > 0 ? 0.18f : 0;
}

/* ── The background: sky, the city on the horizon and the glowing grid below ──
 * Everything here is filled in row by row by the engine (from both cores at once),
 * from tables made per track (the skyline) and per frame (the ground rows). */
#define SKYN 1024                         /* skyline columns round the horizon */
static uint8_t  s_sk_h[SKYN], s_sk_far[SKYN], s_sun_lo[SKYN], s_sun_hi[SKYN];
static uint16_t s_sk_win[SKYN];
static int      s_sk_max, s_sun_max;
static uint16_t s_sky_tab[512];           /* sky colour by rows above the horizon */
static uint16_t s_c_near, s_c_far, s_c_win, s_c_sun;

typedef struct { int32_t x, z, dx, dz; uint16_t c0, c1; uint8_t grid, pad; } grow_t;
static grow_t   s_gr[HMAX];
static uint16_t s_col[WMAX];
static uint16_t s_rowwin[HMAX];
static int      s_hz;                     /* the horizon row this frame */

static void make_background(void)
{
    const track_def_t *td = g_td;
    for (int up = 0; up < 512; up++) {
        uint32_t c;
        if (up < 14)
            c = mix_rgb(td->fog, td->sky_hz, (float)up / 14.f);
        else if (up < 90)
            c = mix_rgb(td->sky_hz, td->sky_mid, (float)(up - 14) / 76.f);
        else
            c = mix_rgb(td->sky_mid, td->sky_top, (float)(up - 90) / 140.f);
        s_sky_tab[up] = rgb565(c);
    }
    s_c_near = rgb565(td->skyline);
    s_c_far = rgb565(mix_rgb(td->skyline2, td->fog, 0.45f));
    s_c_win = rgb565(td->window);
    s_c_sun = rgb565(g_track_i == 0 ? 0xFFE8B0 : 0xE8ECF8);
    /* towers: runs of columns, a far layer behind */
    uint32_t r = 0xBEEF + (uint32_t)g_track_i * 31u;
#define CR() ((r = r * 1664525u + 1013904223u), (float)(r >> 8) * (1.f / 16777216.f))
    memset(s_sk_h, 0, sizeof(s_sk_h));
    memset(s_sk_far, 0, sizeof(s_sk_far));
    s_sk_max = 0;
    for (int i = 0; i < SKYN;) {
        int w = 3 + (int)(CR() * 10.f), h = (int)(CR() * CR() * 46.f) + 3;
        bool gap = CR() < 0.22f;
        for (int k = 0; k < w && i < SKYN; k++, i++) {
            s_sk_h[i] = gap ? 0 : (uint8_t)h;
            s_sk_win[i] = (k == 0 || k == w - 1) ? 0 : (uint16_t)(CR() * 65535.f) & (uint16_t)(CR() * 65535.f);
            if (s_sk_h[i] > s_sk_max) s_sk_max = s_sk_h[i];
        }
    }
    for (int i = 0; i < SKYN;) {
        int w = 6 + (int)(CR() * 16.f), h = 10 + (int)(CR() * 50.f);
        for (int k = 0; k < w && i < SKYN; k++, i++) {
            s_sk_far[i] = (uint8_t)h;
            if (h > s_sk_max) s_sk_max = h;
        }
    }
    /* a big low sun (or the moon) */
    memset(s_sun_lo, 255, sizeof(s_sun_lo));
    memset(s_sun_hi, 0, sizeof(s_sun_hi));
    int sc = g_track_i == 0 ? SKYN * 200 / 360 : SKYN * 60 / 360, sup = g_track_i == 0 ? 22 : 80, srad = g_track_i == 0 ? 14 : 8;
    s_sun_max = sup + srad;
    for (int d = -srad; d <= srad; d++) {
        int i = (sc + d + SKYN) % SKYN;
        float hh = sqrtf((float)(srad * srad - d * d));
        s_sun_lo[i] = (uint8_t)(sup - hh);
        s_sun_hi[i] = (uint8_t)(sup + hh);
    }
#undef CR
}

/* per frame: where each screen column looks, the horizon, and each ground row in world space */
static void background_frame(const e3d_camera *cam, float focal)
{
    const track_def_t *td = g_td;
    float p = cam->pitch * RAD, hd = cam->heading * RAD;
    float cp = cosf(p), sp = sinf(p), ch = cosf(hd), sh = sinf(hd);
    s_hz = (int)floorf((float)H * 0.5f + focal * tanf(p) + 0.5f);
    for (int x = 0; x < W; x++) {
        float a = cam->heading + atanf(((float)x + 0.5f - W * 0.5f) / focal) * DEG;
        int i = (int)(a * (SKYN / 360.f)) % SKYN;
        if (i < 0) i += SKYN;
        s_col[x] = (uint16_t)i;
    }
    float camh = cam->pos[1] - FLOOR_Y;
    float u0 = (0.5f - W * 0.5f) / focal;
    for (int y = 0; y < H; y++) {
        int up = s_hz - y;
        s_rowwin[y] = up > 0 && up % 3 == 1 ? (uint16_t)(1u << ((up / 3) & 15)) : 0;
        grow_t *g = &s_gr[y];
        if (up > 0)
            continue;
        float v = ((float)y + 0.5f - H * 0.5f) / focal;
        float den = v * cp - sp;
        float t = den > 1e-4f ? camh / den : 1e6f;
        float fog = expf(-t * td->fog_density);
        g->c0 = rgb565(mix_rgb(td->fog, td->ground, fog));
        if (t > 650.f) {
            g->grid = 0;
            continue;
        }
        float fade = clampf(1.f - (t - 250.f) / 400.f, 0, 1);
        g->c1 = rgb565(mix_rgb(td->fog, mix_rgb(td->ground, td->grid, fade), fog));
        float fd = t * (cp + v * sp);
        float lat = t / focal;
        float X = cam->pos[0] + ch * fd - sh * t * u0, Z = cam->pos[2] + sh * fd + ch * t * u0;
        g->x = (int32_t)(X * 65536.f);
        g->z = (int32_t)(Z * 65536.f);
        g->dx = (int32_t)(-sh * lat * 65536.f);
        g->dz = (int32_t)(ch * lat * 65536.f);
        g->grid = 1;
    }
}

static void bg_row(uint16_t *row, int y, int w, void *ctx)
{
    int up = s_hz - y;
    if (up <= 0) {
        const grow_t *g = &s_gr[y];
        uint16_t c0 = g->c0;
        if (!g->grid) {
            for (int x = 0; x < w; x++)
                row[x] = c0;
            return;
        }
        uint16_t c1 = g->c1;
        int32_t X = g->x, Z = g->z;
        const int32_t dX = g->dx, dZ = g->dz;
        for (int x = 0; x < w; x++) {
            row[x] = (((X >> 17) & 15) == 0 || ((Z >> 17) & 15) == 0) ? c1 : c0;
            X += dX;
            Z += dZ;
        }
        return;
    }
    uint16_t sky = s_sky_tab[up < 512 ? up : 511];
    if (up > s_sk_max && up > s_sun_max) {
        for (int x = 0; x < w; x++)
            row[x] = sky;
        return;
    }
    uint16_t near = s_c_near, far = s_c_far, win = s_c_win, sun = s_c_sun, wb = s_rowwin[y];
    for (int x = 0; x < w; x++) {
        int i = s_col[x];
        uint16_t c = sky;
        if (up <= s_sk_h[i])
            c = (s_sk_win[i] & wb) ? win : near;
        else if (up <= s_sk_far[i])
            c = far;
        else if (up >= s_sun_lo[i] && up <= s_sun_hi[i])
            c = sun;
        row[x] = c;
    }
}

/* ── Drawing the world ─────────────────────────────────────────────────── */
/* how far the track and scenery are drawn: eased down when the frame rate drops, back up when it recovers.
   The distances of the detailed ships and the detailed track shrink in proportion. */
static float g_view = 520.f;
static void view_adapt(float fps)
{
    if (fps <= 0)
        return;
    if (fps < 40.f)
        g_view = fmaxf(280.f, g_view - 20.f);
    else if (fps > 52.f)
        g_view = fminf(520.f, g_view + 10.f);
}

static bool in_view(const float *c, float r, float maxd, const float *cp, const float *fwd, const float *right, float tanx)
{
    float d[3] = { c[0] - cp[0], c[1] - cp[1], c[2] - cp[2] };
    float dist = sqrtf(v3_dot(d, d));
    if (dist - r > maxd)
        return false;
    float x = v3_dot(d, fwd);
    if (x < -r)
        return false;
    float z = fabsf(v3_dot(d, right));
    return z - r <= x * tanx + r;
}

static void draw_ship(e3d_t *e, const ship_t *s, const float *cp)
{
    if (s->dead && s->dead_t > 1.5f && g_state != ST_TITLE)
        return;
    e3d_mat m = mat_basis(s->pos, s->fwd, s->up, s->right, 1.f);
    bool flash = s->hit_t > 0 && ((int)(s->hit_t * 20.f) & 1);
    float dx = s->pos[0] - cp[0], dy = s->pos[1] - cp[1], dz = s->pos[2] - cp[2];
    float d2 = dx * dx + dy * dy + dz * dz;
    /* the detailed model near the camera (a few hundred triangles), the simple one further off */
    float hd = SHIP_HI_DIST * g_view / 520.f;   /* shrinks with the draw distance on a slow frame */
    const e3d_mesh *mesh = d2 < hd * hd ? m_ship_hi[s->team] : m_ship[s->team];
    draw_obj(e, mesh, &m, s->dead ? 0x202024 : flash ? 0xFFFFFF : -1, E3D_NO_SHADOW);
    bool near = dx * dx + dz * dz < 140.f * 140.f;
    /* the shadow on the track */
    if (near) {
        frame_t f;
        track_frame(s->u, &f);
        float p[3], fw[3], rt[3];
        for (int q = 0; q < 3; q++)
            p[q] = f.p[q] + f.r[q] * s->x + f.u[q] * 0.07f;
        float cy = cosf(s->yaw), sy = sinf(s->yaw);
        for (int q = 0; q < 3; q++)
            fw[q] = f.t[q] * cy + f.r[q] * sy;
        v3_cross(rt, fw, f.u);
        e3d_mat b = mat_basis(p, fw, f.u, rt, 1.f);
        draw_obj(e, m_blob, &b, -1, E3D_UNLIT | E3D_NO_SHADOW);
    }
    if (s->dead)
        return;
    /* engine flames */
    if (near && s->flame > 0.05f) {
        float k = s->flame * (0.85f + 0.3f * frand());
        int32_t col = s->boost_t > 0 || s->turbo_t > 0 ? 0xFFB040 : 0x80E8FF;
        for (int side = -1; side <= 1; side += 2) {
            float p[3];
            for (int q = 0; q < 3; q++)
                p[q] = s->pos[q] - s->fwd[q] * 2.05f + s->right[q] * 1.35f * (float)side + s->up[q] * 0.07f;
            e3d_mat fm = mat_basis(p, s->fwd, s->up, s->right, k);
            draw_obj(e, m_flame, &fm, col, E3D_UNLIT | E3D_NO_SHADOW);
        }
    }
    if (s->shield_t > 0 && ((int)(s->shield_t * 10.f) & 1 || s->shield_t > 1.5f)) {
        e3d_mat sm = mat_at(s->pos[0], s->pos[1] + 0.3f, s->pos[2], 0, g_race_t * 90.f, 0, 1.f);
        draw_obj(e, m_shield, &sm, 0x60E0FF, E3D_WIRE | E3D_UNLIT | E3D_NO_SHADOW | E3D_DOUBLE_SIDED);
    }
}

static void draw_scene(e3d_t *e)
{
    const track_def_t *td = g_td;
    e3d_camera *cam = e3d_cam(e);
    *cam = (e3d_camera){ .pos = { g_cam_p[0], g_cam_p[1], g_cam_p[2] }, .heading = g_cam_h, .pitch = g_cam_pitch };
    float focal = g_focal * (float)W / 400.f;     /* the same view at any frame size */
    e3d_set_focal(e, focal);
    const ship_t *w = &g_ship[g_watch];
    *e3d_light_get(e) = (e3d_light){ .pos = { w->pos[0] + td->light[0], w->pos[1] + td->light[1], w->pos[2] + td->light[2] },
                                     .r = (float)(td->light_rgb >> 16), .g = (float)((td->light_rgb >> 8) & 255),
                                     .b = (float)(td->light_rgb & 255) };
    background_frame(cam, focal);

    float hd = g_cam_h * RAD, pt = g_cam_pitch * RAD;
    float fwd[3] = { cosf(pt) * cosf(hd), sinf(pt), cosf(pt) * sinf(hd) };
    float right[3] = { -sinf(hd), 0, cosf(hd) };
    float tanx = (W * 0.5f + 8.f) / focal;
    const float *cp = cam->pos;
    e3d_mat id;
    e3d_identity(&id);
    for (int i = 0; i < g_nchunks; i++) {
        const chunk_t *k = &g_chunk[i];
        if (in_view(k->c, k->r, g_view, cp, fwd, right, tanx)) {
            float d[3] = { k->c[0] - cp[0], k->c[1] - cp[1], k->c[2] - cp[2] };
            int lod = sqrtf(v3_dot(d, d)) - k->r > NEAR_TRACK * g_view / 520.f ? 1 : 0;
            draw_obj(e, k->road[lod], &id, -1, E3D_NO_SHADOW);
            draw_obj(e, k->rneon[lod], &id, -1, E3D_UNLIT | E3D_NO_SHADOW);
        }
        if (k->scen && in_view(k->scen->center, k->scen->radius, g_view + 40.f, cp, fwd, right, tanx))
            draw_obj(e, k->scen, &id, -1, E3D_NO_SHADOW);
        if (k->sneon && in_view(k->sneon->center, k->sneon->radius, g_view + 40.f, cp, fwd, right, tanx))
            draw_obj(e, k->sneon, &id, -1, E3D_UNLIT | E3D_NO_SHADOW);
    }
    /* the start gantry and its lights */
    if (g_gantry && in_view(g_gantry->center, g_gantry->radius, 450.f, cp, fwd, right, tanx)) {
        draw_obj(e, g_gantry, &id, -1, E3D_NO_SHADOW);
        const samp_t *s = &g_tr[0];
        for (int i = 0; i < 3; i++) {
            float p[3];
            samp_point(0, -2.4f + 2.4f * (float)i, 8.0f, p);
            v3_madd(p, p, s->t, -0.45f);
            int32_t col = 0x301010;
            if (g_state == ST_COUNT)
                col = (int)g_state_t > i ? 0xFF2020 : 0x301010;
            else if (g_state == ST_RACE && g_race_t < 2.f)
                col = 0x20FF40;
            else if (g_state == ST_TITLE)
                col = 0x20FF40;
            e3d_mat m = mat_basis(p, s->t, s->u, s->r, 1.f);
            draw_obj(e, g_lamp, &m, col, E3D_UNLIT | E3D_NO_SHADOW);
        }
    }
    /* ships */
    for (int i = 0; i < NSHIPS; i++) {
        const ship_t *s = &g_ship[i];
        if (in_view(s->pos, 3.f, 380.f, cp, fwd, right, tanx))
            draw_ship(e, s, cp);
    }
    /* rockets and mines */
    for (int i = 0; i < MAX_PROJ; i++) {
        const proj_t *p = &g_proj[i];
        if (p->life <= 0)
            continue;
        frame_t f;
        track_frame(p->u, &f);
        float pos[3];
        for (int q = 0; q < 3; q++)
            pos[q] = f.p[q] + f.r[q] * p->x + f.u[q] * (p->type == WPN_ROCKET ? 1.0f : 0.7f);
        if (!in_view(pos, 1.5f, 300.f, cp, fwd, right, tanx))
            continue;
        if (p->type == WPN_ROCKET) {
            e3d_mat m = mat_basis(pos, f.t, f.u, f.r, 1.f);
            draw_obj(e, m_rocket, &m, -1, E3D_UNLIT | E3D_NO_SHADOW);
            if (frand() < 0.5f)
                spawn_spark(pos, 0.3f, 2.f, 0xFFA040, 0.3f, 0.35f, NULL);
        } else {
            e3d_mat m = mat_at(pos[0], pos[1], pos[2], 0, p->life * 200.f, 0, 0.9f + 0.15f * sinf(p->life * 12.f));
            draw_obj(e, m_mine, &m, -1, E3D_UNLIT | E3D_NO_SHADOW);
        }
    }
    /* sparks and debris */
    for (int i = 0; i < MAX_SPARKS; i++) {
        const spark_t *s = &g_spark[i];
        if (s->life <= 0)
            continue;
        float t = s->life / s->life0;
        e3d_mat m = mat_at(s->p[0], s->p[1], s->p[2], t * 300.f, t * 500.f, 0, s->size * (0.4f + 0.6f * t));
        draw_obj(e, m_spark, &m, (int32_t)mix_rgb(0x402010, s->rgb, t), E3D_UNLIT | E3D_NO_SHADOW | E3D_FLAT);
    }
}

/* ── HUD ───────────────────────────────────────────────────────────────── */
static void text_at(uint16_t *px, int x, int y, const char *s, uint16_t col, int scale)
{
    e3d_text(px, W, W, H, x + scale, y + scale, s, 0x0000, scale);
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
    for (int j = y < 0 ? 0 : y; j < y + h && j < H; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < W; i++)
            px[j * W + i] = col;
}
static void shade_box(uint16_t *px, int x, int y, int w, int h)
{
    for (int j = y < 0 ? 0 : y; j < y + h && j < H; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < W; i++) {
            uint16_t c = px[j * W + i];
            px[j * W + i] = (uint16_t)((c >> 2) & 0x39E7);
        }
}
static void frame_box(uint16_t *px, int x, int y, int w, int h, uint16_t col)
{
    fill(px, x, y, w, 1, col);
    fill(px, x, y + h - 1, w, 1, col);
    fill(px, x, y, 1, h, col);
    fill(px, x + w - 1, y, 1, h, col);
}
static void tri_fill(uint16_t *px, float ax, float ay, float bx, float by, float cx, float cy, uint16_t col)
{
    int y0 = (int)fmaxf(0, floorf(fminf(ay, fminf(by, cy)))), y1 = (int)fminf(H - 1, ceilf(fmaxf(ay, fmaxf(by, cy))));
    for (int y = y0; y <= y1; y++) {
        float fy = (float)y + 0.5f, xl = 1e9f, xr = -1e9f;
        const float P[3][2] = { { ax, ay }, { bx, by }, { cx, cy } };
        for (int i = 0; i < 3; i++) {
            const float *p = P[i], *q = P[(i + 1) % 3];
            if ((p[1] <= fy && q[1] > fy) || (q[1] <= fy && p[1] > fy)) {
                float x = p[0] + (fy - p[1]) / (q[1] - p[1]) * (q[0] - p[0]);
                xl = fminf(xl, x);
                xr = fmaxf(xr, x);
            }
        }
        for (int x = (int)fmaxf(0, ceilf(xl - 0.5f)); x <= (int)fminf(W - 1, floorf(xr - 0.5f)); x++)
            px[y * W + x] = col;
    }
}
static const char *ordinal(int n)
{
    static const char *const o[] = { "", "1ST", "2ND", "3RD", "4TH", "5TH", "6TH" };
    return n >= 1 && n <= 6 ? o[n] : "";
}

/* the map: the track's plan, scaled into a box */
#define MAP_W 84
#define MAP_H 64
#define MAP_X (W - MAP_W - 6)
#define MAP_Y 46
static int16_t g_map[MAX_SAMP / 4][2];
static int g_nmap;
static float g_map_s, g_map_ox, g_map_oz;
static void map_xy(float x, float z, int *mx, int *my)
{
    *mx = MAP_X + (int)((x - g_map_ox) * g_map_s);
    *my = MAP_Y + (int)((z - g_map_oz) * g_map_s);
}
static void make_map(void)
{
    float sx = (MAP_W - 4) / (g_bx1 - g_bx0), sz = (MAP_H - 4) / (g_bz1 - g_bz0);
    g_map_s = fminf(sx, sz);
    g_map_ox = g_bx0 - ((MAP_W - (g_bx1 - g_bx0) * g_map_s) * 0.5f) / g_map_s;
    g_map_oz = g_bz0 - ((MAP_H - (g_bz1 - g_bz0) * g_map_s) * 0.5f) / g_map_s;
    g_nmap = 0;
    for (int k = 0; k < g_ns; k += 4) {
        int x, y;
        map_xy(g_tr[k].p[0], g_tr[k].p[2], &x, &y);
        g_map[g_nmap][0] = (int16_t)x;
        g_map[g_nmap][1] = (int16_t)y;
        g_nmap++;
    }
}
static void draw_map(uint16_t *px)
{
    uint16_t c = rgb565(0xB0B8C8), c2 = rgb565(0x404858);
    for (int i = 0; i < g_nmap; i++) {
        int x = g_map[i][0], y = g_map[i][1];
        if (x >= 1 && x < W - 1 && y >= 1 && y < H - 1) {
            px[(y + 1) * W + x + 1] = c2;
            px[y * W + x] = c;
        }
    }
    for (int i = NSHIPS - 1; i >= 0; i--) {
        const ship_t *s = &g_ship[i];
        if (s->dead)
            continue;
        int x, y;
        map_xy(s->pos[0], s->pos[2], &x, &y);
        bool me = i == 0 && g_state != ST_TITLE;
        uint16_t col = me ? ((int)(g_race_t * 6.f) & 1 ? 0xFFFF : rgb565(k_team[s->team].prim)) : rgb565(k_team[s->team].prim);
        fill(px, x - 1 - me, y - 1 - me, 3 + 2 * me, 3 + 2 * me, me ? 0xFFFF : 0);
        fill(px, x - me, y - me, 1 + 2 * me, 1 + 2 * me, col);
    }
}

static void draw_results(uint16_t *px)
{
    int bx = 70, by = 28, bw = W - 140, bh = 184;
    shade_box(px, bx, by, bw, bh);
    frame_box(px, bx, by, bw, bh, rgb565(g_td->neon));
    text_c(px, by + 8, "RESULTS", rgb565(g_td->neon), 2);
    char line[48], tm[16];
    for (int i = 0; i < NSHIPS; i++) {
        const ship_t *s = &g_ship[g_order[i]];
        if (s->finished)
            fmt_time(tm, sizeof(tm), s->finish_t);
        else
            snprintf(tm, sizeof(tm), "%s", s->dead ? "OUT" : "--");
        snprintf(line, sizeof(line), "%d  %-8s %8s", i + 1, k_team[s->team].name, tm);
        int y = by + 34 + i * 16;
        bool me = g_order[i] == 0;
        if (me)
            fill(px, bx + 6, y - 3, bw - 12, 13, rgb565(shade_rgb(g_td->neon, 0.35f)));
        fill(px, bx + 14, y, 6, 7, rgb565(k_team[s->team].prim));
        text_at(px, bx + 26, y, line, me ? 0xFFFF : rgb565(0xC0C8D8), 1);
    }
    const ship_t *p = PLAYER;
    fmt_time(tm, sizeof(tm), p->best_lap);
    snprintf(line, sizeof(line), "YOUR BEST LAP %s", tm);
    text_c(px, by + 136, line, rgb565(0xFFE060), 1);
    if (g_new_race_rec || g_new_lap_rec)
        text_c(px, by + 150, g_new_race_rec ? "NEW RACE RECORD" : "NEW LAP RECORD", (int)(g_state_t * 4.f) & 1 ? 0xFFFF : rgb565(0xFF60A0), 1);
    if ((int)(g_state_t * 2.f) & 1)
        text_c(px, by + 166, "PRESS A", 0xFFFF, 1);
}

static void draw_title(uint16_t *px)
{
    uint16_t neon = rgb565(g_td->neon), neon2 = rgb565(g_td->neon2);
    text_at(px, (W - text_w("SKYRIFT", 6)) / 2 + 2, 26 + 2, "SKYRIFT", neon2, 6);
    text_c(px, 26, "SKYRIFT", 0xFFFF, 6);
    text_c(px, 76, "ANTI-GRAVITY RACING LEAGUE", neon, 1);
    char line[64], a[16], b[16];
    shade_box(px, 0, H - 58, W, 58);
    snprintf(line, sizeof(line), "%s   %s CLASS   %d LAPS", g_td->name, g_cl->name, g_laps);
    text_c(px, H - 52, line, 0xFFFF, 1);
    fmt_time(a, sizeof(a), g_rec[g_track_i][g_class][0]);
    fmt_time(b, sizeof(b), g_rec[g_track_i][g_class][1]);
    snprintf(line, sizeof(line), "BEST LAP %s   BEST RACE %s", a, b);
    text_c(px, H - 38, line, rgb565(0xFFE060), 1);
    if ((int)(g_state_t * 2.f) & 1)
        text_c(px, H - 20, "PRESS A TO RACE", neon, 2);
}

static void draw_hud(uint16_t *px, float fps)
{
    if (g_state == ST_TITLE) {
        draw_title(px);
        return;
    }
    if (g_state == ST_RESULTS) {
        draw_results(px);
        return;
    }
    const ship_t *p = PLAYER;
    uint16_t white = 0xFFFF, neon = rgb565(g_td->neon), dim = rgb565(0xA0A8B8);
    char buf[48];
    /* lap and times, top left */
    int lap = p->lap + 1 > g_laps ? g_laps : p->lap + 1;
    text_at(px, 8, 6, "LAP", dim, 1);
    snprintf(buf, sizeof(buf), "%d/%d", lap, g_laps);
    text_at(px, 8, 16, buf, white, 3);
    char t[16];
    fmt_time(t, sizeof(t), g_state == ST_COUNT ? 0 : g_race_t - p->lap_start);
    if (p->finished)
        fmt_time(t, sizeof(t), p->last_lap);
    text_at(px, 8, 42, t, white, 1);
    if (p->best_lap > 0) {
        fmt_time(t, sizeof(t), p->best_lap);
        snprintf(buf, sizeof(buf), "BEST %s", t);
        text_at(px, 8, 52, buf, dim, 1);
    }
    /* place, top right */
    snprintf(buf, sizeof(buf), "%d", p->place);
    text_r(px, W - 30, 6, buf, white, 5);
    snprintf(buf, sizeof(buf), "/%d", NSHIPS);
    text_r(px, W - 6, 24, buf, dim, 2);
    draw_map(px);
    /* the weapon, top centre */
    if (p->weapon != WPN_NONE && !p->dead) {
        const char *n = wpn_name[p->weapon];
        int bw = 120;
        shade_box(px, (W - bw) / 2, 4, bw, 34);
        frame_box(px, (W - bw) / 2, 4, bw, 34, rgb565(0xFFA010));
        text_c(px, 9, n, rgb565(0xFFC040), 2);
        text_c(px, 27, s_touch_used ? "TAP FIRE" : "B FIRE  X ABSORB", dim, 1);
    }
    if (p->shield_t > 0)
        text_c(px, 42, "SHIELD", (int)(p->shield_t * 6.f) & 1 ? neon : white, 1);
    /* energy, bottom left */
    float en = p->energy / 100.f;
    uint16_t ec = en > 0.5f ? rgb565(0x40F070) : en > 0.25f ? rgb565(0xFFD030) : rgb565(0xFF3030);
    if (en <= 0.25f && ((int)(g_race_t * 6.f) & 1))
        ec = white;
    text_at(px, 8, H - 30, "ENERGY", dim, 1);
    fill(px, 7, H - 19, 122, 10, 0);
    fill(px, 8, H - 18, (int)(120.f * en), 8, ec);
    for (int i = 1; i < 10; i++)
        fill(px, 8 + i * 12, H - 18, 1, 8, 0);
    /* speed, bottom right */
    int kmh = (int)(fmaxf(p->va, 0) * SPEED_KMH);
    snprintf(buf, sizeof(buf), "%d", kmh);
    text_r(px, W - 40, H - 30, buf, white, 3);
    text_at(px, W - 34, H - 16, "KM/H", dim, 1);
    float sp = clampf(p->va / (g_cl->vmax * 1.3f), 0, 1);
    for (int i = 0; i < 16; i++) {
        uint16_t c = (float)i / 16.f < sp ? (i < 11 ? neon : rgb565(0xFF5050)) : rgb565(0x303440);
        fill(px, W - 140 + i * 6, H - 10 - i / 3, 4, 4 + i / 3, c);
    }
    if (p->boost_t > 0 && ((int)(p->boost_t * 10.f) & 1))
        text_r(px, W - 8, H - 44, p->turbo_t > 0 ? "TURBO" : "BOOST", rgb565(0x40F0FF), 1);
    /* touch hints */
    if (s_touch_used && (g_state == ST_RACE || g_state == ST_COUNT)) {
        uint16_t tc = rgb565(0x506070);
        tri_fill(px, 12, H - 80, 40, H - 96, 40, H - 64, tc);
        tri_fill(px, W - 12, H - 80, W - 40, H - 96, W - 40, H - 64, tc);
        text_c(px, H - 54, "FIRE", tc, 1);
    }
    /* the countdown and messages, centre */
    if (g_state == ST_COUNT) {
        int n = 3 - (int)g_state_t;
        snprintf(buf, sizeof(buf), "%d", n < 1 ? 1 : n);
        text_c(px, 72, buf, rgb565(0xFF4040), 7);
    } else if (g_msg_t > 0) {
        bool go = !strcmp(g_msg, "GO");
        text_c(px, go ? 72 : 80, g_msg, go ? rgb565(0x40FF60) : white, go ? 7 : 3);
    }
    if (g_state == ST_FINISH) {
        if (p->dead)
            text_c(px, 90, "ELIMINATED", rgb565(0xFF4040), 4);
        else {
            snprintf(buf, sizeof(buf), "FINISHED %s", ordinal(p->place));
            text_c(px, 90, buf, white, 3);
        }
    }
    if (g_show_fps) {
        snprintf(buf, sizeof(buf), "%.0f FPS", (double)fps);
        text_c(px, H - 10, buf, dim, 1);
    }
}

/* ── Building a track ──────────────────────────────────────────────────── */
static e3d_t *g_e;

static void apply_look(e3d_t *e)
{
    e3d_params *par = e3d_params_get(e);
    par->ambient = g_td->ambient;
    par->diffuse = 0.80f;
    par->specular = 0.45f;
    par->shininess = 16.f;
    par->shadows = false;
    par->fog_density = g_td->fog_density;
    par->fog_rgb565 = rgb565(g_td->fog);
    par->fog_keep_bg = true;
}

static bool track_build(void)
{
    int64_t t0 = papp_time_us();
    if (!b_init())
        return false;
    build_track();
    place_pads();
    place_buildings();
    bool ok = build_chunks();
    make_background();
    make_map();
    papp_log("skyrift: %s: %.0f m, %d samples, %d chunks, %d pads, %d buildings, built in %d ms\n", g_td->name,
             (double)g_len, g_ns, g_nchunks, g_npads, g_nbld, (int)((papp_time_us() - t0) / 1000));
    return ok;
}

static bool track_switch(int t)
{
    free_track();
    g_track_i = t;
    g_td = &k_track[t];
    bool ok = track_build();
    b_free();
    if (g_e)
        apply_look(g_e);
    return ok;
}

#ifndef SKY_HOST
/* ── Presenting: a thread hands one frame to the display while the next is drawn ── */
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

/* the surface, the two frames and the renderer, at the size g_res picks */
static uint16_t *g_fb[2];
static bool g_res_dirty;

static void video_close(void)
{
    if (g_e) {
        e3d_destroy(g_e);
        g_e = NULL;
    }
    for (int i = 0; i < 2; i++) {
        papp_free(g_fb[i]);
        g_fb[i] = NULL;
    }
    if (g_surf) {
        papp_disp_surface_destroy(g_surf);
        g_surf = NULL;
    }
}

static bool video_open(void)
{
    g_w = k_res[g_res][0];
    g_h = k_res[g_res][1];
    papp_surface_cfg_t cfg = { .width = (uint16_t)W, .height = (uint16_t)H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 1 };
    g_surf = papp_disp_surface_create(&cfg);
    for (int i = 0; i < 2; i++)
        g_fb[i] = papp_alloc(sizeof(uint16_t) * (size_t)(W * H), PAPP_MEM_LARGE | PAPP_MEM_DMA, 64);
    g_e = g_surf && g_fb[0] && g_fb[1] ? e3d_create(W, H, 2) : NULL;
    if (!g_e)
        return false;
    e3d_set_background_rows(g_e, bg_row, NULL);
    apply_look(g_e);
    papp_log("skyrift: drawing at %d x %d\n", W, H);
    return true;
}

/* open at g_res, falling back to the smallest size */
static bool video_open_any(void)
{
    if (video_open())
        return true;
    video_close();
    if (g_res == 0)
        return false;
    papp_log("skyrift: no memory for %d x %d, using %d x %d\n", W, H, k_res[0][0], k_res[0][1]);
    g_res = 0;
    return video_open();
}

static int menu(const char *title, const char *const *items, int n)
{
    papp_sem_take(g_pres_done, PAPP_WAIT_FOREVER);   /* nothing is being presented now */
    float ev = s_eng_vol;
    s_eng_vol = s_boost_vol = s_wind_vol = s_scrape_vol = 0;
    int r = papp_ui_menu(title, items, n, 0);
    papp_sem_give(g_pres_done);
    papp_input_flush();
    s_prev_btn = ~0u;          /* ignore the buttons still held */
    s_eng_vol = ev;
    return r;
}

/* returns false to exit */
static bool pause_menu(void)
{
    const char *const items[] = { "Resume", "Restart race", "Quit to title", "Exit game" };
    int r = menu("Paused", items, 4);
    if (r == 1)
        start_race();
    else if (r == 2)
        start_attract();
    return r != 3 && !papp_exit_requested();
}

/* the title menu; returns false to exit */
static bool title_menu(void)
{
    for (;;) {
        char trk[40], cls[32], laps[24], team[32];
        snprintf(trk, sizeof(trk), "Track: %s", g_td->name);
        snprintf(cls, sizeof(cls), "Class: %s", g_cl->name);
        snprintf(laps, sizeof(laps), "Laps: %d", g_laps);
        snprintf(team, sizeof(team), "Team: %s", k_team[g_team].name);
        const char *items[7] = { "Start race", trk, cls, laps, team, "Settings", "Exit game" };
        int r = menu("Skyrift", items, 7);
        if (r < 0)
            return !papp_exit_requested();
        if (r == 0) {
            start_race();
            return true;
        }
        if (r == 6)
            return false;
        if (r == 1) {
            const char *names[NTRACKS];
            for (int i = 0; i < NTRACKS; i++)
                names[i] = k_track[i].name;
            int c = menu("Track", names, NTRACKS);
            if (c >= 0 && c != g_track_i) {
                if (!track_switch(c)) {
                    papp_log("skyrift: out of memory building %s\n", k_track[c].name);
                    track_switch(0);
                }
                start_attract();
            }
        } else if (r == 2) {
            const char *names[NCLASSES];
            for (int i = 0; i < NCLASSES; i++)
                names[i] = k_class[i].name;
            int c = menu("Speed class", names, NCLASSES);
            if (c >= 0) {
                g_class = c;
                g_cl = &k_class[c];
            }
        } else if (r == 3) {
            static const char *const ln[] = { "1 lap", "2 laps", "3 laps", "4 laps", "5 laps" };
            int c = menu("Laps", ln, 5);
            if (c >= 0)
                g_laps = c + 1;
        } else if (r == 4) {
            const char *names[NSHIPS];
            for (int i = 0; i < NSHIPS; i++)
                names[i] = k_team[i].name;
            int c = menu("Team", names, NSHIPS);
            if (c >= 0)
                g_team = c;
        } else if (r == 5) {
            for (;;) {
                char ctl[40], thr[32], res[40];
                snprintf(ctl, sizeof(ctl), "Steering: %s", ctrl_name[g_ctrl]);
                snprintf(thr, sizeof(thr), "Thrust: %s", g_autothrust ? "automatic" : "A / R2 / UP");
                snprintf(res, sizeof(res), "Resolution: %d x %d", k_res[g_res][0], k_res[g_res][1]);
                const char *ci[4] = { ctl, thr, res, "Back" };
                int c = menu("Settings", ci, 4);
                if (c == 0)
                    g_ctrl = (g_ctrl + 1) % NCTRL;
                else if (c == 1)
                    g_autothrust = !g_autothrust;
                else if (c == 2) {
                    int q = menu("Resolution", k_res_name, NRES);
                    if (q >= 0 && q != g_res) {
                        g_res = q;
                        g_res_dirty = true;     /* applied when the menu closes */
                    }
                } else
                    break;
            }
        }
        settings_io(true);
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
        else if (!strcmp(argv[i], "track") && i + 1 < argc)
            g_track_i = (int)clampf((float)atoi(argv[++i]), 0, NTRACKS - 1);
        else if (!strcmp(argv[i], "class") && i + 1 < argc)
            g_class = (int)clampf((float)atoi(argv[++i]), 0, NCLASSES - 1);
        else if (!strcmp(argv[i], "laps") && i + 1 < argc)
            g_laps = (int)clampf((float)atoi(argv[++i]), 1, 9);
        else if (!strcmp(argv[i], "res") && i + 1 < argc)
            g_res = (int)clampf((float)atoi(argv[++i]), 0, NRES - 1);
    }
    g_td = &k_track[g_track_i];
    g_cl = &k_class[g_class];
    srand((unsigned)papp_time_us());
    if (g_ctrl < 0)
        g_ctrl = papp_input_analog(PAPP_ANALOG_PADDLE) >= 0 ? CTRL_ROLLER : CTRL_PAD;
    if (g_ctrl == CTRL_ROLLER)
        g_autothrust = true;

    g_pres_go = papp_sem_create(0, 1);
    g_pres_done = papp_sem_create(1, 1);
    bool vid = g_pres_go && g_pres_done && video_open_any();
    papp_thread_cfg_t pcfg = { .name = "sr_present", .stack_size = 4096, .prio = PAPP_PRIO_HIGH, .core = 0 };
    papp_thread_t *pres = vid ? papp_thread_create(presenter_main, NULL, &pcfg) : NULL;
    e3d_t *e = g_e;
    bool ok = pres && e && b_init() && make_meshes() && track_build();
    b_free();
    if (!ok) {
        papp_log("skyrift: out of memory\n");
        return 1;
    }
    records_io(false);

    g_snd_lock = papp_mutex_create(false);
    papp_audio_cfg_t acfg = { .rate = SR, .channels = 1, .format = PAPP_AUDIO_S16, .latency_ms = 60 };
    g_snd = g_snd_lock ? papp_audio_open(&acfg) : NULL;
    papp_thread_cfg_t acfg_t = { .name = "sr_audio", .stack_size = 4096, .prio = PAPP_PRIO_HIGH, .core = 1 };
    papp_thread_t *aud = g_snd ? papp_thread_create(audio_main, NULL, &acfg_t) : NULL;
    if (!aud && g_snd) {
        papp_audio_close(g_snd);
        g_snd = NULL;
    }

    if (g_auto)
        start_race();
    else
        start_attract();
    papp_log("skyrift: running%s, %s, %s class, %d laps\n", g_auto ? " (auto)" : "", g_td->name, g_cl->name, g_laps);

    int cur = 0, frames = 0;
    int64_t last = papp_time_us(), stat_t = last, log_t = last;
    float fps = 0;
    bool run = true;
    while (run && !papp_exit_requested()) {
        int64_t now = papp_time_us();
        float dt = (float)(now - last) * 1e-6f;
        last = now;
        if (dt > 0.05f)
            dt = 0.05f;
        input_t in = read_input(g_surf, dt);
        bool go = in.start || in.tap;
        if (g_state == ST_TITLE) {
            if (go || in.pause) {
                run = title_menu();
                if (g_res_dirty && run) {
                    /* a new frame size: nothing may be on its way to the display while it changes */
                    g_res_dirty = false;
                    papp_sem_take(g_pres_done, PAPP_WAIT_FOREVER);
                    video_close();
                    run = video_open_any();
                    papp_sem_give(g_pres_done);
                    e = g_e;
                    cur = 0;
                }
                last = papp_time_us();
                continue;
            }
        } else if (in.pause) {
            run = pause_menu();
            last = papp_time_us();
            continue;
        } else if (g_state == ST_RESULTS && go && g_state_t > 1.f) {
            if (g_auto)
                start_race();
            else
                start_attract();
        }
        if (g_auto && g_state == ST_RESULTS && g_state_t > 6.f)
            start_race();
        update(&in, dt);

        uint16_t *px = g_fb[cur];
        e3d_begin(e, px, W);
        draw_scene(e);
        e3d_end(e);
        draw_hud(px, fps);

        frames++;
        if (now - stat_t >= 1000000) {
            fps = (float)frames * 1e6f / (float)(now - stat_t);
            frames = 0;
            stat_t = now;
        }
        if (frames == 0 && g_state != ST_TITLE)
            view_adapt(fps);
        if (now - log_t >= 5000000) {
            const e3d_stats *st = e3d_get_stats(e);
            papp_log("skyrift: %.1f fps, state %d, lap %d, place %d, %.0f km/h, energy %.0f, objs %d, tris %d/%d, "
                     "geom %d us, raster %d us, view %.0f m\n",
                     (double)fps, g_state, PLAYER->lap + 1, PLAYER->place, (double)(PLAYER->va * SPEED_KMH),
                     (double)PLAYER->energy, st->objects, st->tris_drawn, st->tris_in, st->t_geom_us, st->t_raster_us, (double)g_view);
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
    settings_io(true);
    video_close();
    free_track();
    free_meshes();
    papp_sem_destroy(g_pres_go);
    papp_sem_destroy(g_pres_done);
    return 0;
}
#endif /* !SKY_HOST */
