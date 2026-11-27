/*
 * Skyguard — missile defence in the spirit of the 1980 arcade classic, on the
 * e3d engine: six cities and three missile bases on a plain under a night sky,
 * seen from a low camera that drifts slowly so the scene has depth.
 *
 * The game itself is played in one vertical plane (x = 0): enemy ICBMs fall
 * from the top towards your cities and bases, leaving trails. Put the
 * crosshair in their path and launch an anti-ballistic missile; it flies to
 * the crosshair and bursts into a fireball. Anything inside a fireball is
 * destroyed, and every kill bursts too, so chains clear whole salvos.
 *
 * Each base holds 10 missiles a wave (the stack on its mound) and is rebuilt
 * every wave. From wave 2 missiles can split (MIRV) and bombers or satellites
 * cross the sky dropping more; from wave 6 smart bombs dodge your fireballs.
 * Points multiply every two waves (up to 6X). Between waves, unused missiles
 * score 5 and surviving cities 100 (times the multiplier). Every 10000
 * points earns a bonus city. The game ends when no city is left.
 *
 * Controls:
 *   D-pad / left stick    move the crosshair   (roller boards: the roller moves it across)
 *   A                     fire from the nearest base with missiles
 *   X / L1                fire from the left base
 *   Y                     fire from the centre base
 *   B / R1                fire from the right base
 *   START / SELECT        pause
 *   Touch: tap where to fire (nearest base); the top-right corner pauses.
 *
 * Records (high score, best wave) in /sd/data/skyguard/records.txt.
 * Arguments: `demo` = the computer plays (testing), `fps` = show the frame rate,
 *            `wave N` = start at wave N.
 */
#include "papp/papp.h"
#include "e3d.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

PAPP_MANIFEST(.id = "michael.skyguard", .name = "Skyguard", .version = PAPP_APP_VERSION, .main_stack = 32768);

#define W 400
#define H 240
#define DEG (180.f / (float)M_PI)
#define RAD ((float)M_PI / 180.f)
#define NC(a) ((int)(sizeof(a) / sizeof(a[0])))

static float frand(void) { return (float)rand() / (float)RAND_MAX; }
static float frange(float a, float b) { return a + (b - a) * frand(); }
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

/* ── Sound: a few synth voices for the effects ─────────────────────────── */
#define SR 22050
enum { WAVE_SQUARE, WAVE_TRI, WAVE_NOISE, WAVE_SAW };
typedef struct {
    int   left, delay, total;
    float ph, f, fmul, vol;
    int   wave;
} voice_t;
#define NVOICES 10
static voice_t g_voice[NVOICES];
static papp_mutex_t *g_snd_lock;
static papp_audio_stream_t *g_snd;
static volatile bool g_snd_quit;
static uint32_t g_noise = 0x1234567u;

static void sfx_at(float f0, float f1, int ms, int wave, float vol, int delay_ms)
{
    if (!g_snd)
        return;
    papp_mutex_lock(g_snd_lock);
    int best = 0;
    for (int i = 0; i < NVOICES; i++) {
        if (g_voice[i].left == 0 && g_voice[i].delay == 0) {
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

static void snd_launch(void) { sfx(1800, 600, 220, WAVE_NOISE, 0.16f); sfx(700, 1400, 160, WAVE_SAW, 0.07f); }
static void snd_burst(void) { sfx(220, 40, 420, WAVE_NOISE, 0.22f); }
static void snd_kill(void) { sfx(500, 90, 300, WAVE_NOISE, 0.16f); sfx(900, 300, 90, WAVE_SQUARE, 0.05f); }
static void snd_city(void) { sfx(120, 25, 1400, WAVE_NOISE, 0.42f); sfx(70, 30, 900, WAVE_SQUARE, 0.18f); }
static void snd_base(void) { sfx(150, 30, 1000, WAVE_NOISE, 0.36f); sfx(90, 40, 600, WAVE_SQUARE, 0.14f); }
static void snd_empty(void) { sfx(140, 120, 90, WAVE_SQUARE, 0.10f); }
static void snd_tick(float f) { sfx(f, f, 45, WAVE_SQUARE, 0.08f); }
static void snd_flyer(void) { sfx(300, 300, 160, WAVE_TRI, 0.06f); }
static void snd_bonus(void)
{
    for (int i = 0; i < 6; i++)
        sfx_at(i & 1 ? 1320.f : 990.f, i & 1 ? 1320.f : 990.f, 110, WAVE_SQUARE, 0.10f, i * 120);
}
static void snd_siren(void)
{
    for (int i = 0; i < 3; i++) {
        sfx_at(400, 1000, 380, WAVE_TRI, 0.13f, i * 760);
        sfx_at(1000, 400, 380, WAVE_TRI, 0.13f, i * 760 + 380);
    }
}
static void snd_end(void) { sfx(90, 20, 3500, WAVE_NOISE, 0.55f); sfx(60, 25, 2500, WAVE_SQUARE, 0.2f); }

static int audio_main(void *arg)
{
    (void)arg;
    static int16_t buf[256];
    float lp = 0;
    while (!g_snd_quit && !papp_exit_requested()) {
        papp_mutex_lock(g_snd_lock);
        for (int i = 0; i < 256; i++) {
            g_noise = g_noise * 1664525u + 1013904223u;
            float nz = (float)(int32_t)g_noise * (1.f / 2147483648.f);
            float acc = 0;
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
                float s;
                if (v->wave == WAVE_NOISE) {
                    /* noise through a one-pole filter whose cutoff follows the "pitch" */
                    float k2 = clampf(v->f * (1.f / 2500.f), 0.01f, 1.f);
                    v->ph += (nz - v->ph) * k2;
                    s = v->ph * 1.6f;
                } else {
                    s = v->wave == WAVE_SQUARE ? (v->ph < 0.5f ? 1.f : -1.f)
                      : v->wave == WAVE_TRI    ? (v->ph < 0.5f ? 4.f * v->ph - 1.f : 3.f - 4.f * v->ph)
                                               : v->ph * 2.f - 1.f;
                    v->ph += v->f * (1.f / SR);
                    if (v->ph >= 1.f)
                        v->ph -= 1.f;
                }
                acc += s * v->vol * env;
                v->f *= v->fmul;
                v->left--;
            }
            lp += (acc - lp) * 0.6f;
            buf[i] = (int16_t)(clampf(lp, -1.f, 1.f) * 22000.f);
        }
        papp_mutex_unlock(g_snd_lock);
        papp_audio_write(g_snd, buf, 256, 100);
    }
    return 0;
}

/* ── Mesh building ───────────────────────────────────────────────────────────
 * A builder collects vertices and triangles, then bakes them into an e3d mesh.
 * Triangles are turned to face away from a given inside point. */
#define BMAX_V 6000
#define BMAX_T 6000
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
/* a quad facing along n (an open surface: the inside point is behind it) */
static void b_quad_n(int a, int b, int c, int d, uint32_t rgb, float nx, float ny, float nz)
{
    float ctr[3] = { (B.pos[a][0] + B.pos[c][0]) * 0.5f - nx * 10.f, (B.pos[a][1] + B.pos[c][1]) * 0.5f - ny * 10.f,
                     (B.pos[a][2] + B.pos[c][2]) * 0.5f - nz * 10.f };
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
        b_quad(v[0], v[1], v[5], v[4], shade_rgb(rgb, 0.85f), c);
    b_quad(v[2], v[3], v[7], v[6], shade_rgb(rgb, 1.10f), c);
    b_quad(v[0], v[1], v[3], v[2], rgb, c);
    b_quad(v[4], v[5], v[7], v[6], rgb, c);
    b_quad(v[0], v[2], v[6], v[4], shade_rgb(rgb, 0.92f), c);
    b_quad(v[1], v[3], v[7], v[5], shade_rgb(rgb, 0.92f), c);
}
/* a frustum: bottom rectangle (x +-bx, z +-bz) at y0, top (+-tx, +-tz) at y1, centred at (cx, cz) */
static void b_frustum(float cx, float cz, float y0, float y1, float bx, float bz, float tx, float tz, uint32_t rgb,
                      uint32_t top_rgb)
{
    int v[8];
    for (int i = 0; i < 8; i++) {
        bool top = i & 4;
        float sx = (i & 1) ? 1.f : -1.f, sz = (i & 2) ? 1.f : -1.f;
        v[i] = b_v(cx + sx * (top ? tx : bx), top ? y1 : y0, cz + sz * (top ? tz : bz));
    }
    float c[3] = { cx, (y0 + y1) * 0.5f, cz };
    b_quad(v[4], v[5], v[7], v[6], top_rgb, c);
    b_quad(v[0], v[1], v[5], v[4], rgb, c);
    b_quad(v[2], v[3], v[7], v[6], rgb, c);
    b_quad(v[0], v[2], v[6], v[4], shade_rgb(rgb, 0.9f), c);
    b_quad(v[1], v[3], v[7], v[5], shade_rgb(rgb, 0.9f), c);
}
/* an octahedron of radius r with an axis stretch */
static void b_octa(float cx, float cy, float cz, float rx, float ry, float rz, uint32_t rgb)
{
    int v[6] = { b_v(cx + rx, cy, cz), b_v(cx - rx, cy, cz), b_v(cx, cy + ry, cz),
                 b_v(cx, cy - ry, cz), b_v(cx, cy, cz + rz), b_v(cx, cy, cz - rz) };
    float c[3] = { cx, cy, cz };
    for (int i = 0; i < 8; i++)
        b_tri(v[(i & 1) ? 0 : 1], v[(i & 2) ? 2 : 3], v[(i & 4) ? 4 : 5], (i & 2) ? rgb : shade_rgb(rgb, 0.7f), c);
}

static void draw_obj(e3d_t *e, const e3d_mesh *mesh, const e3d_mat *m, int32_t rgb, uint32_t flags)
{
    if (!mesh)
        return;
    e3d_object o;
    e3d_object_init(&o, mesh);
    o.m = *m;
    o.color = rgb;
    o.flags = flags | E3D_NO_SHADOW;
    e3d_draw(e, &o);
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

/* ── The battlefield ───────────────────────────────────────────────────────
 * The game is played in the plane x = 0: z across the screen, y up. */
#define FIELD_Z   100.f
#define TOP_Y     122.f           /* enemies appear here, just above the screen */
#define BASE_TOP  7.f             /* the top of a base mound */
#define CUR_YMIN  14.f
#define CUR_YMAX  112.f
#define NCITY 6
#define CITY_S 1.3f              /* cities are drawn this much larger than built */
#define NBASE 3
#define NTGT  (NCITY + NBASE)
static const float k_city_z[NCITY] = { -67.f, -46.f, -25.f, 25.f, 46.f, 67.f };
static const float k_base_z[NBASE] = { -89.f, 0.f, 89.f };

/* colours per pair of waves, as the arcade changed them */
typedef struct {
    uint32_t sky_top, sky_hz, mtn, ground, enemy, abm, window;
} palette_t;
static const palette_t k_pal[] = {
    { 0x000008, 0x10184A, 0x0C1230, 0x8A7028, 0xFF3A30, 0x5A7CFF, 0xFFD860 },   /* ochre plain, red ICBMs */
    { 0x000000, 0x081030, 0x081028, 0x2E4AA0, 0xFFE040, 0x60FF70, 0x80F0FF },   /* blue ground, yellow */
    { 0x00080A, 0x0A3038, 0x08242A, 0x4A6A40, 0xFF50F0, 0xFFE040, 0xFFFFFF },   /* teal night, magenta */
    { 0x080000, 0x401010, 0x2A0A0A, 0x504040, 0x40F0FF, 0xFFFFFF, 0xFFA040 },   /* red sky, cyan */
    { 0x050010, 0x281050, 0x1C0A38, 0x2E6A30, 0xFF9020, 0x40E0FF, 0xFFE0A0 },   /* purple, orange */
    { 0x100800, 0x584018, 0x302010, 0x6A4A28, 0x50A0FF, 0x80FF60, 0xFFF080 },   /* amber dusk, blue */
};
#define NPAL NC(k_pal)
static const palette_t *g_pal = &k_pal[0];

/* meshes */
static e3d_mesh *m_icbm, *m_abm_fly, *m_flame;
static e3d_mesh *m_terrain, *m_mound, *m_cube, *m_sphere, *m_bomber, *m_sat, *m_smart, *m_arrow;
static e3d_mesh *m_city[NCITY], *m_cwin[NCITY], *m_rubble[NCITY];

static float terrain_h(float x, float z)
{
    if (x < 25.f)
        return 0;
    float k = clampf((x - 25.f) / 220.f, 0, 1);
    float h = 6.f + 5.f * sinf(z * 0.045f + x * 0.02f) + 4.f * sinf(z * 0.11f - x * 0.05f + 1.3f) +
              3.f * sinf(x * 0.09f + 0.7f);
    return fmaxf(0.f, h * k * (1.f + x * 0.004f));
}

/* a missile along +x with its nose at the origin: a six-sided body, a cone nose and four fins */
#define MSL_LEN 7.f
#define MSL_S   2.0f            /* ICBMs are drawn this size */
#define ABM_S   1.6f            /* our missiles */
static void b_missile(float len, float r, uint32_t body, uint32_t nose, uint32_t fin)
{
    const int N = 6;
    float nl = r * 2.6f, x0 = -len, x1 = -nl;
    int ring0 = B.nv;
    for (int i = 0; i < N; i++) {
        float a = (float)i * (2.f * (float)M_PI / N);
        b_v(x0, cosf(a) * r, sinf(a) * r);
    }
    int ring1 = B.nv;
    for (int i = 0; i < N; i++) {
        float a = (float)i * (2.f * (float)M_PI / N);
        b_v(x1, cosf(a) * r, sinf(a) * r);
    }
    int tip = b_v(0, 0, 0), tail = b_v(x0, 0, 0);
    for (int i = 0; i < N; i++) {
        int j = (i + 1) % N;
        float c[3] = { (x0 + x1) * 0.5f, 0, 0 };
        b_quad(ring0 + i, ring0 + j, ring1 + j, ring1 + i, (i & 1) ? body : shade_rgb(body, 0.9f), c);
        float cn[3] = { x1 - 0.5f, 0, 0 };
        b_tri(ring1 + i, ring1 + j, tip, nose, cn);
        float ct[3] = { x0 + 0.5f, 0, 0 };
        b_tri(ring0 + i, ring0 + j, tail, shade_rgb(body, 0.6f), ct);
    }
    /* fins: thin wedges at the tail, in four directions */
    for (int k = 0; k < 4; k++) {
        float a = (float)k * ((float)M_PI * 0.5f) + (float)M_PI / 6.f;
        float cy = cosf(a), cz = sinf(a);
        float span = r * 2.4f, th = 0.12f;
        float py = -cz * th, pz = cy * th;   /* the fin's thickness, perpendicular to its span */
        int v[6] = { b_v(x0, cy * r + py, cz * r + pz), b_v(x0 + len * 0.32f, cy * r + py, cz * r + pz),
                     b_v(x0 - 0.4f, cy * span + py, cz * span + pz),
                     b_v(x0, cy * r - py, cz * r - pz), b_v(x0 + len * 0.32f, cy * r - py, cz * r - pz),
                     b_v(x0 - 0.4f, cy * span - py, cz * span - pz) };
        float c[3] = { x0 + len * 0.1f, cy * r * 1.5f, cz * r * 1.5f };
        b_tri(v[0], v[1], v[2], fin, c);
        b_tri(v[3], v[4], v[5], fin, c);
        b_quad(v[1], v[2], v[5], v[4], shade_rgb(fin, 0.8f), c);
    }
}

static bool make_meshes(void)
{
    /* terrain: a trapezoid grid that widens with distance, flat along the battle line */
    b_reset();
    static const float xs[] = { -110, -70, -40, -15, 10, 35, 65, 100, 145, 200, 270, 360, 480, 650 };
    const int NX = NC(xs), NZ = 25;
    int base = B.nv;
    for (int i = 0; i < NX; i++) {
        float hw = 150.f + 0.85f * (xs[i] + 180.f);
        for (int j = 0; j < NZ; j++) {
            float z = ((float)j / (float)(NZ - 1) * 2.f - 1.f) * hw;
            b_v(xs[i], terrain_h(xs[i], z), z);
        }
    }
    for (int i = 0; i + 1 < NX; i++)
        for (int j = 0; j + 1 < NZ; j++) {
            int a = base + i * NZ + j;
            uint32_t c = ((i + j) & 1) ? 0xF0F0F0 : 0xE0E0E0;
            b_quad_n(a, a + 1, a + NZ + 1, a + NZ, c, 0, 1, 0);
        }
    m_terrain = b_bake(0);

    /* a base mound */
    b_reset();
    b_frustum(0, 0, -0.5f, BASE_TOP, 11.f, 15.f, 5.f, 7.f, 0xE0E0E0, 0xFFFFFF);
    m_mound = b_bake(0);


    /* a unit cube centred on the origin: trails (stretched) and missile heads */
    b_reset();
    b_box(-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f, 0xFFFFFF, false);
    m_cube = b_bake(0);

    m_sphere = e3d_mesh_sphere(1.f, 12, 7, 255, 255, 255);

    /* the falling ICBM: dark body, red warhead; ours: white with a blue nose */
    b_reset();
    b_missile(MSL_LEN, 0.62f, 0xC0C8D0, 0xFF3020, 0x5A6470);
    m_icbm = b_bake(0);
    b_reset();
    b_missile(MSL_LEN, 0.55f, 0xF0F0F4, 0x3070FF, 0x3070FF);
    m_abm_fly = b_bake(0);
    /* the exhaust: a cone pointing back from the tail, along -x */
    b_reset();
    {
        const int N = 6;
        int ring = B.nv;
        for (int i = 0; i < N; i++) {
            float a = (float)i * (2.f * (float)M_PI / N);
            b_v(-MSL_LEN, cosf(a) * 0.5f, sinf(a) * 0.5f);
        }
        int end = b_v(-MSL_LEN - 3.f, 0, 0);
        float c[3] = { -MSL_LEN - 1.f, 0, 0 };
        for (int i = 0; i < N; i++)
            b_tri(ring + i, ring + (i + 1) % N, end, 0xFFFFFF, c);
    }
    m_flame = b_bake(0);

    /* the bomber: fuselage along z, wings across x */
    b_reset();
    b_box(-0.9f, -0.7f, -4.f, 0.9f, 0.7f, 4.f, 0x9098A8, false);
    b_box(-5.5f, -0.2f, -1.2f, 5.5f, 0.2f, 1.4f, 0x707888, false);
    b_box(-2.2f, -0.15f, -4.2f, 2.2f, 0.15f, -3.2f, 0x707888, false);
    b_box(-0.15f, 0.f, -4.2f, 0.15f, 2.2f, -3.3f, 0x606878, false);
    b_box(-0.6f, 0.2f, 2.6f, 0.6f, 0.9f, 3.6f, 0x40C0FF, false);
    m_bomber = b_bake(0);

    /* the satellite: a body and two panels */
    b_reset();
    b_octa(0, 0, 0, 1.8f, 2.2f, 1.8f, 0xC0C0D0);
    b_box(-0.15f, -1.4f, 2.0f, 0.15f, 1.4f, 6.5f, 0x3050C0, false);
    b_box(-0.15f, -1.4f, -6.5f, 0.15f, 1.4f, -2.0f, 0x3050C0, false);
    m_sat = b_bake(0);

    /* a smart bomb: a diamond */
    b_reset();
    b_octa(0, 0, 0, 1.6f, 2.0f, 1.6f, 0xFFFFFF);
    m_smart = b_bake(0);

    /* a down arrow: points at the cities to defend */
    b_reset();
    {
        int v[3] = { b_v(-3.f, 0, 0), b_v(-3.f, 4.f, -2.2f), b_v(-3.f, 4.f, 2.2f) };
        float behind[3] = { 7.f, 2.f, 0 };      /* faces -x, towards the camera */
        b_tri(v[0], v[1], v[2], 0xFFFFFF, behind);
    }
    m_arrow = b_bake(0);

    /* cities: a cluster of towers each, with lit windows on the faces towards the camera */
    static const uint32_t k_tower[] = { 0x5A6478, 0x6A7088, 0x506070, 0x787C90, 0x4A5868, 0x687890 };
    for (int c = 0; c < NCITY; c++) {
        srand(1000 + c * 37);
        float tw[10][6];
        int nt = 6 + rand() % 3;
        b_reset();
        for (int t = 0; t < nt; t++) {
            float z = frange(-8.f, 8.f), x = frange(-5.f, 4.f);
            float hw = frange(1.3f, 2.4f), hd = frange(1.4f, 2.6f), hgt = frange(4.f, t < 2 ? 17.f : 11.f);
            uint32_t col = k_tower[rand() % NC(k_tower)];
            b_box(x - hd, 0, z - hw, x + hd, hgt, z + hw, col, true);
            if (t == 0)
                b_box(x - 0.15f, hgt, z - 0.15f, x + 0.15f, hgt + 4.f, z + 0.15f, 0xA0A0A0, true);
            tw[t][0] = x - hd;
            tw[t][1] = z - hw;
            tw[t][2] = z + hw;
            tw[t][3] = hgt;
        }
        m_city[c] = b_bake(0);
        /* windows */
        b_reset();
        for (int t = 0; t < nt; t++) {
            float x = tw[t][0] - 0.06f;
            for (float y = 1.2f; y + 1.0f < tw[t][3]; y += 2.0f)
                for (float z = tw[t][1] + 0.5f; z + 0.7f < tw[t][2]; z += 1.3f) {
                    if (frand() < 0.4f)
                        continue;
                    int a = b_v(x, y, z), b = b_v(x, y, z + 0.7f), cc = b_v(x, y + 0.9f, z + 0.7f), d = b_v(x, y + 0.9f, z);
                    b_quad_n(a, b, cc, d, 0xFFFFFF, -1, 0, 0);
                }
        }
        m_cwin[c] = b_bake(0);
        /* rubble */
        b_reset();
        for (int t = 0; t < 9; t++) {
            float z = frange(-9.f, 9.f), x = frange(-5.f, 4.f), s = frange(0.8f, 2.2f);
            b_box(x - s, -0.2f, z - s, x + s * 0.7f, frange(0.4f, 2.4f), z + s, t & 1 ? 0x302820 : 0x483C30, true);
        }
        m_rubble[c] = b_bake(0);
    }
    srand((unsigned)papp_time_us());
    bool ok = m_icbm && m_abm_fly && m_flame && m_terrain && m_mound && m_cube && m_sphere && m_bomber && m_sat && m_smart && m_arrow;
    for (int c = 0; c < NCITY; c++)
        ok = ok && m_city[c] && m_cwin[c] && m_rubble[c];
    return ok;
}

static void free_meshes(void)
{
    e3d_mesh *all[] = { m_icbm, m_abm_fly, m_flame, m_terrain, m_mound, m_cube, m_sphere, m_bomber, m_sat, m_smart, m_arrow };
    for (int i = 0; i < NC(all); i++)
        if (all[i])
            e3d_mesh_free(all[i]);
    for (int c = 0; c < NCITY; c++) {
        if (m_city[c]) e3d_mesh_free(m_city[c]);
        if (m_cwin[c]) e3d_mesh_free(m_cwin[c]);
        if (m_rubble[c]) e3d_mesh_free(m_rubble[c]);
    }
}

/* ── Game state ────────────────────────────────────────────────────────── */
enum { ST_INTRO, ST_PLAY, ST_TALLY, ST_OVER };
enum { E_MISSILE, E_SMART };
enum { F_BOMBER, F_SAT };

typedef struct {
    bool  on;
    int   type, target;
    float sy, sz;          /* where the trail starts */
    float y, z, vy, vz;    /* the head and its velocity */
    float speed, split_y;
    float t;
} enemy_t;
typedef struct {
    bool  on;
    int   base;
    float sy, sz, y, z, ty, tz, vy, vz, left;
} abm_t;
typedef struct {
    bool  on;
    float y, z, t, rmax;
    bool  big;
} expl_t;
typedef struct {
    bool  on;
    int   type;
    float y, z, dir, speed, drop_t, t;
} flyer_t;

#define MAX_EN   48
#define MAX_ABM  10
#define MAX_EXP  48
#define MAX_FLY  2
static enemy_t g_en[MAX_EN];
static abm_t   g_abm[MAX_ABM];
static expl_t  g_ex[MAX_EXP];
static flyer_t g_fly[MAX_FLY];

static bool  g_city[NCITY];
static bool  g_base_ok[NBASE];
static int   g_ammo[NBASE];
static int   g_wave, g_mult, g_budget, g_smart_left, g_fly_left;
static float g_espeed, g_mirv_p, g_salvo_t, g_fly_t;
static int   g_tgt[NTGT], g_ntgt;            /* this wave's targets */
static int   g_score, g_hiscore, g_best_wave, g_next_bonus, g_bonus_cities;
static int   g_kills;
static int   g_state = ST_INTRO;
static float g_state_t, g_time;
static int   g_tally_phase, g_tally_i, g_tally_abm, g_tally_city;
static float g_tally_t;
static bool  g_demo, g_show_fps;
static int   g_start_wave = 1;
static float g_cur_y = 60.f, g_cur_z = 0.f;
static float g_shake, g_flash;
static char  g_msg[32];
static float g_msg_t;
static bool  g_new_hi;

static int mult_for(int wave) { int m = (wave + 1) / 2; return m > 6 ? 6 : m; }

static float tgt_z(int t) { return t < NCITY ? k_city_z[t] : k_base_z[t - NCITY]; }
static float tgt_y(int t) { return t < NCITY ? 0.8f : BASE_TOP; }

static void note(const char *m, float t)
{
    snprintf(g_msg, sizeof g_msg, "%s", m);
    g_msg_t = t;
}

static int cities_alive(void)
{
    int n = 0;
    for (int i = 0; i < NCITY; i++)
        n += g_city[i];
    return n;
}

static void add_score(int pts)
{
    if (g_demo)
        return;
    g_score += pts * g_mult;
    while (g_score >= g_next_bonus) {
        g_bonus_cities++;
        g_next_bonus += 10000;
        note("BONUS CITY", 2.f);
        snd_bonus();
    }
}

static void explode(float y, float z, float rmax, bool big)
{
    int best = -1;
    float oldest = -1;
    for (int i = 0; i < MAX_EXP; i++) {
        if (!g_ex[i].on) {
            best = i;
            break;
        }
        if (g_ex[i].t > oldest) {
            oldest = g_ex[i].t;
            best = i;
        }
    }
    g_ex[best] = (expl_t){ .on = true, .y = y, .z = z, .t = 0, .rmax = rmax, .big = big };
}

/* a fireball grows, holds, then shrinks */
#define EX_GROW 0.55f
#define EX_HOLD 0.40f
#define EX_FADE 0.75f
static float expl_r(const expl_t *x)
{
    float t = x->t;
    if (t < EX_GROW) {
        float k = t / EX_GROW;
        return x->rmax * k * (2.f - k);
    }
    t -= EX_GROW;
    if (t < EX_HOLD)
        return x->rmax;
    t -= EX_HOLD;
    return x->rmax * fmaxf(0.f, 1.f - t / EX_FADE);
}

/* ── Waves ───────────────────────────────────────────────────────────── */
static void pick_targets(void)
{
    /* the arcade aims at three cities per wave, and at the bases */
    int alive[NCITY], na = 0;
    for (int i = 0; i < NCITY; i++)
        if (g_city[i])
            alive[na++] = i;
    for (int i = na - 1; i > 0; i--) {
        int j = rand() % (i + 1), s = alive[i];
        alive[i] = alive[j];
        alive[j] = s;
    }
    g_ntgt = 0;
    for (int i = 0; i < na && i < 3; i++)
        g_tgt[g_ntgt++] = alive[i];
    for (int b = 0; b < NBASE; b++)
        g_tgt[g_ntgt++] = NCITY + b;
}

static void wave_setup(int n)
{
    g_wave = n;
    g_mult = mult_for(n);
    g_pal = &k_pal[((n - 1) / 2) % NPAL];
    g_budget = 10 + 2 * n;
    if (g_budget > 34) g_budget = 34;
    g_espeed = fminf(6.5f + 1.35f * (float)(n - 1), 27.f);
    g_mirv_p = n >= 2 ? fminf(0.12f + 0.04f * (float)n, 0.5f) : 0.f;
    g_smart_left = n >= 6 ? (n - 4) / 2 : 0;
    if (g_smart_left > 4) g_smart_left = 4;
    g_fly_left = n >= 2 ? 1 + n / 3 : 0;
    if (g_fly_left > 4) g_fly_left = 4;
    g_salvo_t = 0.6f;
    g_fly_t = frange(4.f, 9.f);
    for (int b = 0; b < NBASE; b++) {
        g_base_ok[b] = true;
        g_ammo[b] = 10;
    }
    memset(g_en, 0, sizeof g_en);
    memset(g_abm, 0, sizeof g_abm);
    memset(g_ex, 0, sizeof g_ex);
    memset(g_fly, 0, sizeof g_fly);
    pick_targets();
    g_state = ST_INTRO;
    g_state_t = 0;
    if (!g_demo)
        snd_siren();
}

static void new_game(int wave, bool demo)
{
    g_demo = demo;
    g_score = 0;
    g_next_bonus = 10000;
    g_bonus_cities = 0;
    g_kills = 0;
    g_new_hi = false;
    for (int i = 0; i < NCITY; i++)
        g_city[i] = true;
    g_cur_y = 60.f;
    g_cur_z = 0.f;
    g_msg_t = 0;
    wave_setup(wave);
}

static enemy_t *spawn_enemy(int type, float y, float z, int target)
{
    for (int i = 0; i < MAX_EN; i++) {
        enemy_t *e = &g_en[i];
        if (e->on)
            continue;
        float tz = tgt_z(target) + frange(-3.f, 3.f), ty = tgt_y(target);
        float dy = ty - y, dz = tz - z, l = sqrtf(dy * dy + dz * dz);
        float sp = g_espeed * (type == E_SMART ? 0.85f : frange(0.9f, 1.1f));
        *e = (enemy_t){ .on = true, .type = type, .target = target, .sy = y, .sz = z, .y = y, .z = z,
                        .vy = dy / l * sp, .vz = dz / l * sp, .speed = sp, .t = 0 };
        if (type == E_MISSILE && frand() < g_mirv_p && y > 70.f)
            e->split_y = frange(45.f, 75.f);
        return e;
    }
    return NULL;
}

static int enemies_on(void)
{
    int n = 0;
    for (int i = 0; i < MAX_EN; i++)
        n += g_en[i].on;
    return n;
}

static void spawn_salvo(void)
{
    int k = 2 + rand() % (2 + g_wave / 3);
    for (int i = 0; i < k && g_budget > 0; i++) {
        int type = E_MISSILE;
        if (g_smart_left > 0 && frand() < 0.3f) {
            type = E_SMART;
            g_smart_left--;
        }
        int t = g_tgt[rand() % g_ntgt];
        if (spawn_enemy(type, TOP_Y + frange(0.f, 6.f), frange(-95.f, 95.f), t))
            g_budget--;
    }
}

/* ── Launching ───────────────────────────────────────────────────────── */
static int abms_in_flight(void)
{
    int n = 0;
    for (int i = 0; i < MAX_ABM; i++)
        n += g_abm[i].on;
    return n;
}
/* base: 0..2, or -1 for the nearest one with missiles; returns true if launched */
static bool launch(int base, float ty, float tz)
{
    if (g_state != ST_PLAY && g_state != ST_INTRO)
        return false;
    ty = clampf(ty, CUR_YMIN, CUR_YMAX);
    tz = clampf(tz, -FIELD_Z, FIELD_Z);
    if (base < 0) {
        float bd = 1e9f;
        for (int b = 0; b < NBASE; b++) {
            float d = fabsf(k_base_z[b] - tz);
            if (g_base_ok[b] && g_ammo[b] > 0 && d < bd) {
                bd = d;
                base = b;
            }
        }
        if (base < 0) {
            snd_empty();
            return false;
        }
    }
    if (!g_base_ok[base] || g_ammo[base] <= 0) {
        snd_empty();
        return false;
    }
    for (int i = 0; i < MAX_ABM; i++) {
        abm_t *a = &g_abm[i];
        if (a->on)
            continue;
        float sy = BASE_TOP + 3.f, sz = k_base_z[base];
        float dy = ty - sy, dz = tz - sz, l = sqrtf(dy * dy + dz * dz);
        if (l < 1.f)
            l = 1.f;
        /* the centre base's missiles are the fastest, as in the arcade */
        float sp = base == 1 ? 175.f : 125.f;
        *a = (abm_t){ .on = true, .base = base, .sy = sy, .sz = sz, .y = sy, .z = sz, .ty = ty, .tz = tz,
                      .vy = dy / l * sp, .vz = dz / l * sp, .left = l };
        g_ammo[base]--;
        snd_launch();
        return true;
    }
    return false;
}

/* ── The computer player (attract mode and testing) ────────────────────── */
static float s_ai_cool;
static bool ai_covered(float y, float z)
{
    for (int i = 0; i < MAX_ABM; i++)
        if (g_abm[i].on) {
            float dy = g_abm[i].ty - y, dz = g_abm[i].tz - z;
            if (dy * dy + dz * dz < 9.f * 9.f)
                return true;
        }
    for (int i = 0; i < MAX_EXP; i++)
        if (g_ex[i].on && g_ex[i].t < EX_GROW + EX_HOLD) {
            float dy = g_ex[i].y - y, dz = g_ex[i].z - z;
            if (dy * dy + dz * dz < g_ex[i].rmax * g_ex[i].rmax * 0.8f)
                return true;
        }
    return false;
}
/* returns true with an aim point and base when it wants to fire */
static bool ai_aim(float dt, int *base, float *ay, float *az)
{
    s_ai_cool -= dt;
    if (s_ai_cool > 0 || g_state != ST_PLAY)
        return false;
    float best = 1e9f;
    bool found = false;
    for (int i = 0; i < MAX_EN; i++) {
        const enemy_t *e = &g_en[i];
        if (!e->on || e->y > 110.f)
            continue;
        float tti = (e->y - tgt_y(e->target)) / fmaxf(1.f, -e->vy);
        if (tti > best)
            continue;
        /* the nearest base with missiles */
        int b = -1;
        float bd = 1e9f;
        for (int k = 0; k < NBASE; k++)
            if (g_base_ok[k] && g_ammo[k] > 0 && fabsf(k_base_z[k] - e->z) < bd) {
                bd = fabsf(k_base_z[k] - e->z);
                b = k;
            }
        if (b < 0)
            return false;
        /* lead the target: the missile's flight plus the fireball's growth */
        float t = 0, py = e->y, pz = e->z;
        float sp = b == 1 ? 175.f : 125.f;
        for (int it = 0; it < 4; it++) {
            float dy = py - (BASE_TOP + 3.f), dz = pz - k_base_z[b];
            t = sqrtf(dy * dy + dz * dz) / sp + 0.22f;
            py = e->y + e->vy * t;
            pz = e->z + e->vz * t;
        }
        if (py < CUR_YMIN + 2.f || ai_covered(py, pz))
            continue;
        best = tti;
        *base = b;
        *ay = py;
        *az = pz;
        found = true;
    }
    if (found)
        s_ai_cool = frange(0.18f, 0.4f);
    return found;
}

/* ── Update ─────────────────────────────────────────────────────────────── */
static void destroy_target(int t)
{
    if (t < NCITY) {
        if (g_city[t]) {
            g_city[t] = false;
            g_shake = fmaxf(g_shake, 1.f);
            g_flash = 1.f;
            snd_city();
        } else
            snd_burst();
    } else {
        int b = t - NCITY;
        if (g_base_ok[b]) {
            g_base_ok[b] = false;
            g_ammo[b] = 0;
            g_shake = fmaxf(g_shake, 0.7f);
            g_flash = 0.7f;
            snd_base();
        } else
            snd_burst();
    }
}

static void enemies_step(float dt)
{
    for (int i = 0; i < MAX_EN; i++) {
        enemy_t *e = &g_en[i];
        if (!e->on)
            continue;
        e->t += dt;
        if (e->type == E_SMART) {
            /* home on the target, sidestepping fireballs */
            float dy = tgt_y(e->target) - e->y, dz = tgt_z(e->target) - e->z, l = sqrtf(dy * dy + dz * dz);
            float vy = dy / fmaxf(l, 0.1f) * e->speed, vz = dz / fmaxf(l, 0.1f) * e->speed;
            for (int k = 0; k < MAX_EXP; k++) {
                const expl_t *x = &g_ex[k];
                if (!x->on)
                    continue;
                float ey = e->y - x->y, ez = e->z - x->z, d = sqrtf(ey * ey + ez * ez), r = x->rmax + 9.f;
                if (d < r && d > 0.01f) {
                    float push = (1.f - d / r) * 70.f;
                    vz += ez / d * push;
                    vy += fmaxf(0.f, ey / d) * push * 0.6f;
                }
            }
            e->vy = vy;
            e->vz = vz;
        }
        e->y += e->vy * dt;
        e->z = clampf(e->z + e->vz * dt, -FIELD_Z - 5.f, FIELD_Z + 5.f);
        /* MIRV: split into more warheads */
        if (e->split_y > 0 && e->y < e->split_y) {
            e->split_y = 0;
            int k = 1 + (frand() < 0.35f);
            for (int j = 0; j < k && g_budget > 0; j++) {
                enemy_t *c = spawn_enemy(E_MISSILE, e->y, e->z, g_tgt[rand() % g_ntgt]);
                if (c) {
                    c->split_y = 0;
                    g_budget--;
                }
            }
        }
        if (e->y <= tgt_y(e->target)) {
            e->on = false;
            explode(tgt_y(e->target) + 1.f, e->z, 11.f, false);
            if (fabsf(e->z - tgt_z(e->target)) < 14.f)
                destroy_target(e->target);
        }
    }
}

static void flyers_step(float dt)
{
    g_fly_t -= dt;
    if (g_fly_t <= 0 && g_fly_left > 0 && g_budget > 0) {
        for (int i = 0; i < MAX_FLY; i++)
            if (!g_fly[i].on) {
                flyer_t *f = &g_fly[i];
                float dir = frand() < 0.5f ? 1.f : -1.f;
                *f = (flyer_t){ .on = true, .type = (g_wave & 1) ? F_SAT : F_BOMBER, .y = frange(72.f, 92.f),
                                .z = -dir * (FIELD_Z + 15.f), .dir = dir, .speed = 13.f + (float)g_wave * 0.6f,
                                .drop_t = frange(1.5f, 3.f) };
                g_fly_left--;
                snd_flyer();
                break;
            }
        g_fly_t = frange(7.f, 14.f);
    }
    for (int i = 0; i < MAX_FLY; i++) {
        flyer_t *f = &g_fly[i];
        if (!f->on)
            continue;
        f->t += dt;
        f->z += f->dir * f->speed * dt;
        f->drop_t -= dt;
        if (f->drop_t <= 0 && fabsf(f->z) < FIELD_Z - 8.f && g_budget > 0) {
            int k = 1 + (g_wave >= 5 && frand() < 0.5f);
            for (int j = 0; j < k && g_budget > 0; j++)
                if (spawn_enemy(E_MISSILE, f->y - 2.f, f->z, g_tgt[rand() % g_ntgt]))
                    g_budget--;
            f->drop_t = frange(2.f, 3.5f);
        }
        if (fabsf(f->z) > FIELD_Z + 20.f)
            f->on = false;
    }
}

static void abms_step(float dt)
{
    for (int i = 0; i < MAX_ABM; i++) {
        abm_t *a = &g_abm[i];
        if (!a->on)
            continue;
        float sp = sqrtf(a->vy * a->vy + a->vz * a->vz) * dt;
        if (sp >= a->left) {
            a->on = false;
            explode(a->ty, a->tz, 13.f, false);
            snd_burst();
            continue;
        }
        a->left -= sp;
        a->y += a->vy * dt;
        a->z += a->vz * dt;
    }
}

static void explosions_step(float dt)
{
    for (int i = 0; i < MAX_EXP; i++) {
        expl_t *x = &g_ex[i];
        if (!x->on)
            continue;
        x->t += dt;
        if (x->t > EX_GROW + EX_HOLD + EX_FADE) {
            x->on = false;
            continue;
        }
        float r = expl_r(x);
        if (r < 1.f)
            continue;
        float r2 = r * r;
        for (int k = 0; k < MAX_EN; k++) {
            enemy_t *e = &g_en[k];
            if (!e->on)
                continue;
            float dy = e->y - x->y, dz = e->z - x->z;
            if (dy * dy + dz * dz < r2) {
                e->on = false;
                add_score(e->type == E_SMART ? 125 : 25);
                g_kills++;
                explode(e->y, e->z, 10.f, false);
                snd_kill();
            }
        }
        for (int k = 0; k < MAX_FLY; k++) {
            flyer_t *f = &g_fly[k];
            if (!f->on)
                continue;
            float dy = f->y - x->y, dz = f->z - x->z;
            if (dy * dy + dz * dz < (r + 5.f) * (r + 5.f)) {
                f->on = false;
                add_score(100);
                g_kills++;
                explode(f->y, f->z, 12.f, false);
                snd_kill();
            }
        }
    }
}

static bool field_clear(void)
{
    if (g_budget > 0 || enemies_on() || abms_in_flight())
        return false;
    for (int i = 0; i < MAX_FLY; i++)
        if (g_fly[i].on)
            return false;
    for (int i = 0; i < MAX_EXP; i++)
        if (g_ex[i].on)
            return false;
    return true;
}

static void records_io(bool save);

static void game_over(void)
{
    g_state = ST_OVER;
    g_state_t = 0;
    memset(g_ex, 0, sizeof g_ex);
    if (!g_demo) {
        snd_end();
        if (g_score > g_hiscore) {
            g_hiscore = g_score;
            g_new_hi = true;
        }
        if (g_wave > g_best_wave)
            g_best_wave = g_wave;
        records_io(true);
    }
}

static void tally_step(float dt)
{
    g_tally_t -= dt;
    if (g_tally_t > 0)
        return;
    switch (g_tally_phase) {
    case 0:   /* unused missiles, one by one */
        for (; g_tally_i < NBASE && g_ammo[g_tally_i] == 0; g_tally_i++) {}
        if (g_tally_i < NBASE) {
            g_ammo[g_tally_i]--;
            g_tally_abm++;
            add_score(5);
            snd_tick(1400.f);
            g_tally_t = 0.06f;
        } else {
            g_tally_phase = 1;
            g_tally_i = 0;
            g_tally_t = 0.4f;
        }
        break;
    case 1:   /* surviving cities */
        for (; g_tally_i < NCITY && !g_city[g_tally_i]; g_tally_i++) {}
        if (g_tally_i < NCITY) {
            g_tally_city++;
            add_score(100);
            snd_tick(500.f);
            g_tally_i++;
            g_tally_t = 0.3f;
        } else {
            g_tally_phase = 2;
            g_tally_t = 0.5f;
        }
        break;
    case 2:   /* bonus cities rebuild the ruins */
        if (g_bonus_cities > 0 && cities_alive() < NCITY) {
            int dead[NCITY], n = 0;
            for (int i = 0; i < NCITY; i++)
                if (!g_city[i])
                    dead[n++] = i;
            g_city[dead[rand() % n]] = true;
            g_bonus_cities--;
            note("BONUS CITY", 1.5f);
            snd_bonus();
            g_tally_t = 1.0f;
        } else {
            g_tally_phase = 3;
            g_tally_t = 1.6f;
        }
        break;
    default:
        if (cities_alive() == 0)
            game_over();
        else
            wave_setup(g_wave + 1);
        break;
    }
}

typedef struct {
    bool pause, start, tap;
    int  fire;               /* -2 none, -1 nearest base, 0..2 that base */
    float my, mz;            /* crosshair motion, units/s */
    bool abs_set;            /* touch / roller: put the crosshair at ay / az */
    float ay, az;
    bool abs_z_only;
} input_t;

static void update(const input_t *in, float dt)
{
    g_time += dt;
    g_state_t += dt;
    g_shake = fmaxf(0.f, g_shake - dt * 1.6f);
    g_flash = fmaxf(0.f, g_flash - dt * 2.5f);
    if (g_msg_t > 0)
        g_msg_t -= dt;

    /* the crosshair */
    if (in->abs_set) {
        g_cur_z = in->az;
        if (!in->abs_z_only)
            g_cur_y = in->ay;
    }
    g_cur_y = clampf(g_cur_y + in->my * dt, CUR_YMIN, CUR_YMAX);
    g_cur_z = clampf(g_cur_z + in->mz * dt, -FIELD_Z, FIELD_Z);
    if (in->fire > -2 && g_state == ST_PLAY)
        launch(in->fire, g_cur_y, g_cur_z);

    switch (g_state) {
    case ST_INTRO:
        if (g_state_t > 2.6f) {
            g_state = ST_PLAY;
            g_state_t = 0;
        }
        break;
    case ST_PLAY:
        g_salvo_t -= dt;
        if (g_budget > 0 && (g_salvo_t <= 0 || enemies_on() == 0) && enemies_on() < 6 + g_wave) {
            spawn_salvo();
            g_salvo_t = fmaxf(1.8f, 4.8f - 0.22f * (float)g_wave) * frange(0.8f, 1.2f);
        }
        enemies_step(dt);
        flyers_step(dt);
        abms_step(dt);
        explosions_step(dt);
        if (field_clear() && g_state_t > 1.f) {
            g_state = ST_TALLY;
            g_state_t = 0;
            g_tally_phase = 0;
            g_tally_i = 0;
            g_tally_abm = g_tally_city = 0;
            g_tally_t = 1.0f;
        }
        break;
    case ST_TALLY:
        explosions_step(dt);
        tally_step(dt);
        break;
    case ST_OVER:
        if (g_demo && g_state_t > 5.f)
            new_game(1, true);
        break;
    }
}

/* ── Camera ────────────────────────────────────────────────────────────────
 * The camera stands on the ground just in front of the cities, looking up at
 * the sky the missiles fall from, and swings slowly round a
 * point on it, so the cities, mounds and hills show their depth. */
#define CAM_D     80.f            /* in front of the battle line */
#define CAM_Y     4.f             /* eye height above the ground */
#define CAM_PITCH 21.f            /* looking up */
#define FOCAL     152.f           /* a wide lens: the whole line fits */
static float g_cam_p[3], g_cam_h, g_cam_pitch;
static float g_cf[3], g_cr[3], g_cu[3];      /* forward, right, up */

static void update_camera(void)
{
    float swing = g_demo ? 10.f * sinf(g_time * 0.07f) : 3.f * sinf(g_time * 0.11f);
    float lift = g_demo ? 2.f * sinf(g_time * 0.05f + 1.f) : 0.f;
    float a = swing * RAD;
    float d = CAM_D + (g_demo ? 8.f : 0.f);
    g_cam_p[0] = -d * cosf(a);
    g_cam_p[1] = CAM_Y + lift;
    g_cam_p[2] = -d * sinf(a);
    g_cam_h = swing;
    g_cam_pitch = CAM_PITCH - lift * 0.5f;
    if (g_shake > 0) {
        float k = g_shake * g_shake;
        g_cam_p[1] += frange(-1.f, 1.f) * 2.f * k;
        g_cam_p[2] += frange(-1.f, 1.f) * 2.f * k;
        g_cam_pitch += frange(-1.f, 1.f) * 0.6f * k;
    }
    float h = g_cam_h * RAD, p = g_cam_pitch * RAD;
    g_cf[0] = cosf(p) * cosf(h);
    g_cf[1] = sinf(p);
    g_cf[2] = cosf(p) * sinf(h);
    g_cr[0] = -sinf(h);
    g_cr[1] = 0;
    g_cr[2] = cosf(h);
    /* up = right x forward */
    g_cu[0] = g_cr[1] * g_cf[2] - g_cr[2] * g_cf[1];
    g_cu[1] = g_cr[2] * g_cf[0] - g_cr[0] * g_cf[2];
    g_cu[2] = g_cr[0] * g_cf[1] - g_cr[1] * g_cf[0];
}

/* world -> screen; false behind the camera */
static bool project(float x, float y, float z, float *sx, float *sy)
{
    float d[3] = { x - g_cam_p[0], y - g_cam_p[1], z - g_cam_p[2] };
    float f = d[0] * g_cf[0] + d[1] * g_cf[1] + d[2] * g_cf[2];
    if (f < 1.f)
        return false;
    *sx = (float)W * 0.5f + FOCAL * (d[0] * g_cr[0] + d[1] * g_cr[1] + d[2] * g_cr[2]) / f;
    *sy = (float)H * 0.5f - FOCAL * (d[0] * g_cu[0] + d[1] * g_cu[1] + d[2] * g_cu[2]) / f;
    return true;
}
/* screen -> the battle plane (x = 0) */
static bool unproject(float sx, float sy, float *y, float *z)
{
    float u = (sx - (float)W * 0.5f) / FOCAL, v = -(sy - (float)H * 0.5f) / FOCAL;
    float r[3];
    for (int q = 0; q < 3; q++)
        r[q] = g_cf[q] + g_cr[q] * u + g_cu[q] * v;
    if (r[0] < 1e-3f)
        return false;
    float t = -g_cam_p[0] / r[0];
    *y = g_cam_p[1] + r[1] * t;
    *z = g_cam_p[2] + r[2] * t;
    return true;
}

/* ── Background: sky, stars, far mountains, haze below the horizon ──────── */
#define SKYN 1024
static uint8_t  s_mtn[SKYN];
static uint16_t s_sky_tab[256];
static uint16_t s_col[W];
static int      s_hz;
static uint16_t s_c_mtn, s_c_haze, s_c_ground;
static const palette_t *s_bg_pal;

static void background_frame(void)
{
    if (s_bg_pal != g_pal) {
        s_bg_pal = g_pal;
        for (int up = 0; up < 256; up++)
            s_sky_tab[up] = rgb565(mix_rgb(g_pal->sky_hz, g_pal->sky_top, powf((float)up / 160.f, 0.7f)));
        s_c_mtn = rgb565(g_pal->mtn);
        s_c_haze = rgb565(mix_rgb(g_pal->sky_hz, g_pal->ground, 0.25f));
        s_c_ground = rgb565(shade_rgb(g_pal->ground, 0.35f));
        static bool made;
        if (!made) {
            made = true;
            for (int i = 0; i < SKYN; i++) {
                float a = (float)i * (2.f * (float)M_PI / SKYN);
                float h = 10.f + 6.f * sinf(a * 3.f + 0.4f) + 4.f * sinf(a * 7.f + 1.1f) + 2.5f * sinf(a * 17.f + 2.f) +
                          1.2f * sinf(a * 41.f);
                s_mtn[i] = (uint8_t)clampf(h, 1.f, 40.f);
            }
        }
    }
    s_hz = (int)floorf((float)H * 0.5f + FOCAL * tanf(g_cam_pitch * RAD) + 0.5f);
    for (int x = 0; x < W; x++) {
        float a = g_cam_h + atanf(((float)x + 0.5f - W * 0.5f) / FOCAL) * DEG;
        int i = (int)floorf(a * (SKYN / 360.f) * 4.f) % SKYN;   /* the range repeats every 90 degrees */
        if (i < 0)
            i += SKYN;
        s_col[x] = (uint16_t)i;
    }
}

static void bg_row(uint16_t *row, int y, int w, void *ctx)
{
    (void)ctx;
    int up = s_hz - y;
    if (up <= 0) {
        uint16_t c = up > -3 ? s_c_haze : s_c_ground;
        for (int x = 0; x < w; x++)
            row[x] = c;
        return;
    }
    uint16_t sky = s_sky_tab[up < 256 ? up : 255], mtn = s_c_mtn;
    for (int x = 0; x < w; x++) {
        int i = s_col[x];
        if (up <= s_mtn[i]) {
            row[x] = mtn;
            continue;
        }
        uint32_t h = (uint32_t)i * 73856093u ^ (uint32_t)up * 19349663u;
        h ^= h >> 13;
        h *= 0x5bd1e995u;
        h ^= h >> 15;
        if ((h & 1023u) < 3u && up > s_mtn[i] + 6) {
            int b = 120 + (int)((h >> 12) & 127);
            row[x] = PAPP_RGB565(b, b, b + 20 > 255 ? 255 : b + 20);
        } else
            row[x] = sky;
    }
}

/* ── Drawing the world ─────────────────────────────────────────────────── */
static int g_frame;
static const uint32_t k_fire[] = { 0xFFFFFF, 0xFFF060, 0xFF9020, 0xFF3020, 0xFF40E0, 0x40E0FF, 0x80FF60 };

/* a thin box from (y0, z0) to (y1, z1) in the battle plane */
static void draw_beam(e3d_t *e, float y0, float z0, float y1, float z1, float w, uint32_t rgb)
{
    float dy = y1 - y0, dz = z1 - z0, l = sqrtf(dy * dy + dz * dz);
    if (l < 0.05f)
        return;
    e3d_mat m;
    m.m[0][0] = 0;  m.m[0][1] = 0;              m.m[0][2] = w; m.m[0][3] = 0;
    m.m[1][0] = dy; m.m[1][1] = -dz / l * w;    m.m[1][2] = 0; m.m[1][3] = (y0 + y1) * 0.5f;
    m.m[2][0] = dz; m.m[2][1] = dy / l * w;     m.m[2][2] = 0; m.m[2][3] = (z0 + z1) * 0.5f;
    m.m[3][0] = m.m[3][1] = m.m[3][2] = 0;
    m.m[3][3] = 1;
    /* the cube's x axis runs along the beam: swap so x -> beam, y -> width, z -> depth */
    draw_obj(e, m_cube, &m, (int32_t)rgb, E3D_UNLIT | E3D_DOUBLE_SIDED);
}

/* a missile with its nose at (y, z), pointing along (dy, dz) in the battle plane, rolling by roll degrees */
static void draw_missile(e3d_t *e, const e3d_mesh *mesh, float y, float z, float dy, float dz, float s, float roll,
                         bool flame)
{
    float l = sqrtf(dy * dy + dz * dz);
    if (l < 1e-4f)
        return;
    dy /= l;
    dz /= l;
    float cr = cosf(roll * RAD), sr = sinf(roll * RAD);
    /* basis: along = (0, dy, dz), across = (0, -dz, dy), depth = (1, 0, 0); roll turns across and depth */
    float ac[3] = { sr, -dz * cr, dy * cr }, dp[3] = { cr, dz * sr, -dy * sr };
    e3d_mat m;
    m.m[0][0] = 0;      m.m[0][1] = ac[0] * s; m.m[0][2] = dp[0] * s; m.m[0][3] = 0;
    m.m[1][0] = dy * s; m.m[1][1] = ac[1] * s; m.m[1][2] = dp[1] * s; m.m[1][3] = y;
    m.m[2][0] = dz * s; m.m[2][1] = ac[2] * s; m.m[2][2] = dp[2] * s; m.m[2][3] = z;
    m.m[3][0] = m.m[3][1] = m.m[3][2] = 0;
    m.m[3][3] = 1;
    draw_obj(e, mesh, &m, -1, 0);
    if (flame) {
        float k = s * frange(0.75f, 1.3f);
        e3d_mat f = m;
        /* stretch the flame along the missile, keep its width */
        for (int i = 0; i < 3; i++)
            f.m[i][0] *= k / s;
        float ox = MSL_LEN * s * (1.f - k / s);   /* keep it attached to the tail */
        f.m[1][3] -= dy * ox;
        f.m[2][3] -= dz * ox;
        draw_obj(e, m_flame, &f, (g_frame & 1) ? 0xFFF080 : 0xFF8020, E3D_UNLIT | E3D_FLAT);
    }
}

static void apply_look(e3d_t *e)
{
    e3d_params *par = e3d_params_get(e);
    par->ambient = 0.34f + g_flash * 0.45f;
    par->diffuse = 0.85f;
    par->specular = 0.25f;
    par->shininess = 12.f;
    par->shadows = false;
    par->fog_density = 0.f;
    par->fog_keep_bg = true;
}

/* set the camera, light and look, then start the frame (e3d_begin builds the view from the camera) */
static void scene_begin(e3d_t *e, uint16_t *px)
{
    update_camera();
    e3d_camera *cam = e3d_cam(e);
    *cam = (e3d_camera){ .pos = { g_cam_p[0], g_cam_p[1], g_cam_p[2] }, .heading = g_cam_h, .pitch = g_cam_pitch };
    e3d_set_focal(e, FOCAL);
    float fl = g_flash;
    *e3d_light_get(e) = (e3d_light){ .pos = { -160.f, 240.f, -150.f }, .r = 200.f + 55.f * fl, .g = 190.f + 60.f * fl,
                                     .b = 230.f + 25.f * fl };
    apply_look(e);
    background_frame();
    g_frame++;
    e3d_begin(e, px, W);
}

static void draw_scene(e3d_t *e)
{
    e3d_mat id;
    e3d_identity(&id);

    /* the ground and the mounds */
    draw_obj(e, m_terrain, &id, (int32_t)g_pal->ground, E3D_FLAT);
    for (int b = 0; b < NBASE; b++) {
        e3d_mat m = mat_at(0, 0, k_base_z[b], 0, 0, 0, 1.f);
        uint32_t c = g_base_ok[b] ? shade_rgb(g_pal->ground, 1.15f) : shade_rgb(g_pal->ground, 0.45f);
        draw_obj(e, m_mound, &m, (int32_t)c, E3D_FLAT);
        /* the missile stack: rows of 4, 3, 2, 1 */
        int n = 0;
        for (int r = 0; r < 4 && n < g_ammo[b]; r++)
            for (int k = 0; k < 4 - r && n < g_ammo[b]; k++, n++) {
                float z = k_base_z[b] + ((float)k - (float)(3 - r) * 0.5f) * 2.3f;
                draw_missile(e, m_abm_fly, BASE_TOP + (float)r * 2.4f + MSL_LEN * 0.4f, z, 1.f, 0.f, 0.4f, 30.f, false);
            }
    }
    /* cities */
    for (int c = 0; c < NCITY; c++) {
        e3d_mat m = mat_at(0, 0, k_city_z[c], 0, 0, 0, CITY_S);
        if (g_city[c]) {
            draw_obj(e, m_city[c], &m, -1, E3D_FLAT);
            draw_obj(e, m_cwin[c], &m, (int32_t)g_pal->window, E3D_UNLIT | E3D_FLAT);
        } else
            draw_obj(e, m_rubble[c], &m, -1, E3D_FLAT);
    }
    /* "defend cities" arrows during the intro */
    if (g_state == ST_INTRO && !g_demo && ((int)(g_state_t * 4.f) & 1))
        for (int c = 0; c < NCITY; c++)
            if (g_city[c]) {
                e3d_mat m = mat_at(0, 22.f, k_city_z[c] , 0, 0, 0, 1.2f);
                draw_obj(e, m_arrow, &m, 0x60A0FF, E3D_UNLIT | E3D_DOUBLE_SIDED);
            }
    /* enemy trails and warheads */
    uint32_t trail = g_pal->enemy;
    for (int i = 0; i < MAX_EN; i++) {
        const enemy_t *en = &g_en[i];
        if (!en->on)
            continue;
        if (en->type == E_SMART) {
            e3d_mat m = mat_at(0, en->y, en->z, 0, en->t * 360.f, 0, 1.f);
            draw_obj(e, m_smart, &m, (int32_t)k_fire[(g_frame >> 1) % NC(k_fire)], E3D_UNLIT);
            continue;
        }
        /* the trail runs from the launch point to the missile's tail */
        float sp = sqrtf(en->vy * en->vy + en->vz * en->vz), k = (MSL_LEN + 3.f) * MSL_S / fmaxf(sp, 1e-3f);   /* behind the flame */
        float ty = en->y - en->vy * k, tz = en->z - en->vz * k;
        if ((ty - en->sy) * en->vy + (tz - en->sz) * en->vz > 0)
            draw_beam(e, en->sy, en->sz, ty, tz, 0.9f, trail);
        draw_missile(e, m_icbm, en->y, en->z, en->vy, en->vz, MSL_S, en->t * 90.f, true);
    }
    /* our missiles */
    for (int i = 0; i < MAX_ABM; i++) {
        const abm_t *a = &g_abm[i];
        if (!a->on)
            continue;
        float sp = sqrtf(a->vy * a->vy + a->vz * a->vz), k = (MSL_LEN + 3.f) * ABM_S / fmaxf(sp, 1e-3f);
        float ty = a->y - a->vy * k, tz = a->z - a->vz * k;
        if ((ty - a->sy) * a->vy + (tz - a->sz) * a->vz > 0)
            draw_beam(e, a->sy, a->sz, ty, tz, 0.8f, g_pal->abm);
        draw_missile(e, m_abm_fly, a->y, a->z, a->vy, a->vz, ABM_S, (float)g_frame * 25.f + (float)i * 60.f, true);
    }
    /* bombers and satellites */
    for (int i = 0; i < MAX_FLY; i++) {
        const flyer_t *f = &g_fly[i];
        if (!f->on)
            continue;
        if (f->type == F_BOMBER) {
            e3d_mat m = mat_at(0, f->y + sinf(f->t * 1.7f) * 0.8f, f->z, 0, f->dir > 0 ? 0.f : 180.f, sinf(f->t * 1.3f) * 6.f, 1.7f);
            draw_obj(e, m_bomber, &m, -1, 0);
        } else {
            e3d_mat m = mat_at(0, f->y, f->z, 0, f->t * 120.f, 20.f, 1.4f);
            draw_obj(e, m_sat, &m, -1, 0);
        }
    }
    /* fireballs: a flickering ball with a hot core towards the camera */
    for (int i = 0; i < MAX_EXP; i++) {
        const expl_t *x = &g_ex[i];
        if (!x->on)
            continue;
        float r = expl_r(x);
        if (r < 0.4f)
            continue;
        int ci = (g_frame + i) % NC(k_fire);
        e3d_mat m = mat_at(0, x->y, x->z, 0, (float)i * 37.f, 0, r);
        draw_obj(e, m_sphere, &m, (int32_t)k_fire[ci], E3D_UNLIT | E3D_FLAT);
        if (r > 3.f) {
            /* the core sits towards the camera so it shows in front of the ball */
            float d[3] = { g_cam_p[0], g_cam_p[1] - x->y, g_cam_p[2] - x->z };
            float k = r * 0.75f / sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            e3d_mat c = mat_at(d[0] * k, x->y + d[1] * k, x->z + d[2] * k, 0, 0, 0, r * 0.45f);
            draw_obj(e, m_sphere, &c, (int32_t)k_fire[(ci + 3) % NC(k_fire)], E3D_UNLIT | E3D_FLAT);
        }
    }
    /* the end */
    if (g_state == ST_OVER) {
        float t = g_state_t, r = t < 1.2f ? 95.f * (t / 1.2f) : t < 3.2f ? 95.f : 95.f * fmaxf(0.f, 1.f - (t - 3.2f) / 1.2f);
        if (r > 0.5f) {
            e3d_mat m = mat_at(0, 55.f, 0, 0, t * 20.f, 0, r);
            draw_obj(e, m_sphere, &m, (int32_t)k_fire[(g_frame >> 1) % NC(k_fire)], E3D_UNLIT | E3D_FLAT);
        }
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
static void shade_box(uint16_t *px, int x, int y, int w, int h)
{
    for (int j = y < 0 ? 0 : y; j < y + h && j < H; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < W; i++) {
            uint16_t c = px[j * W + i];
            px[j * W + i] = (uint16_t)((c >> 2) & 0x39E7);
        }
}
static void plot(uint16_t *px, int x, int y, uint16_t c)
{
    if (x >= 0 && x < W && y >= 0 && y < H)
        px[y * W + x] = c;
}
static void crosshair(uint16_t *px, int x, int y, uint16_t col)
{
    for (int pass = 0; pass < 2; pass++) {
        int o = pass ? 0 : 1;
        uint16_t c = pass ? col : 0x0000;
        for (int k = 3; k <= 8; k++) {
            plot(px, x + k + o, y + o, c);
            plot(px, x - k + o, y + o, c);
            plot(px, x + o, y + k + o, c);
            plot(px, x + o, y - k + o, c);
        }
        plot(px, x + o, y + o, c);
    }
}
static void xmark(uint16_t *px, int x, int y, uint16_t col)
{
    for (int k = -3; k <= 3; k++) {
        plot(px, x + k, y + k, col);
        plot(px, x + k, y - k, col);
    }
}

static void draw_hud(uint16_t *px, float fps)
{
    char buf[48];
    uint16_t white = 0xFFFF, yel = rgb565(0xFFE040), red = rgb565(0xFF4030), dim = rgb565(0xA0A8C0);
    uint16_t abm_c = rgb565(g_pal->abm);
    bool blink = (g_frame >> 3) & 1;

    /* X marks where our missiles will burst, and the crosshair */
    for (int i = 0; i < MAX_ABM; i++)
        if (g_abm[i].on) {
            float sx, sy;
            if (project(0, g_abm[i].ty, g_abm[i].tz, &sx, &sy))
                xmark(px, (int)sx, (int)sy, abm_c);
        }
    if (g_state != ST_OVER) {
        float sx, sy;
        if (project(0, g_cur_y, g_cur_z, &sx, &sy))
            crosshair(px, (int)sx, (int)sy, g_demo ? dim : white);
    }

    if (g_demo) {
        shade_box(px, W / 2 - 112, 52, 224, 72);
        text_c(px, 60, "SKYGUARD", rgb565(0xFF5030), 4);
        if (blink)
            text_c(px, 98, "PRESS A OR TAP TO PLAY", white, 1);
        snprintf(buf, sizeof buf, "HIGH SCORE %d   BEST WAVE %d", g_hiscore, g_best_wave);
        text_c(px, 112, buf, yel, 1);
        text_c(px, H - 10, "DEFEND THE CITIES", dim, 1);
        return;
    }

    /* the score line */
    snprintf(buf, sizeof buf, "%d", g_score);
    text_at(px, 6, 4, buf, red, 2);
    snprintf(buf, sizeof buf, "HI %d", g_hiscore > g_score ? g_hiscore : g_score);
    text_c(px, 6, buf, yel, 1);
    snprintf(buf, sizeof buf, "WAVE %d  %dX", g_wave, g_mult);
    text_r(px, W - 6, 6, buf, white, 1);

    /* LOW / OUT under the bases */
    for (int b = 0; b < NBASE; b++) {
        float sx, sy;
        if (!project(-12.f, 0, k_base_z[b], &sx, &sy))
            continue;
        const char *s = NULL;
        if (g_state == ST_PLAY) {
            if (!g_base_ok[b] || g_ammo[b] == 0)
                s = "OUT";
            else if (g_ammo[b] <= 3)
                s = "LOW";
        }
        if (s && blink)
            text_at(px, (int)sx - text_w(s, 1) / 2, (int)fminf(sy, H - 9), s, red, 1);
    }

    if (g_state == ST_INTRO) {
        shade_box(px, W / 2 - 90, 70, 180, 64);
        snprintf(buf, sizeof buf, "WAVE %d", g_wave);
        text_c(px, 78, buf, white, 3);
        snprintf(buf, sizeof buf, "%d X POINTS", g_mult);
        text_c(px, 106, buf, yel, 2);
        if (blink)
            text_c(px, 124, "DEFEND CITIES", rgb565(0x60A0FF), 1);
    } else if (g_state == ST_TALLY) {
        shade_box(px, W / 2 - 110, 64, 220, 76);
        text_c(px, 72, "BONUS POINTS", white, 2);
        snprintf(buf, sizeof buf, "MISSILES %2d  %5d", g_tally_abm, g_tally_abm * 5 * g_mult);
        text_c(px, 98, buf, abm_c, 1);
        snprintf(buf, sizeof buf, "CITIES   %2d  %5d", g_tally_city, g_tally_city * 100 * g_mult);
        text_c(px, 112, buf, rgb565(g_pal->window), 1);
        if (g_tally_phase >= 3 && cities_alive() > 0)
            text_c(px, 126, "GET READY", white, 1);
    } else if (g_state == ST_OVER) {
        if (g_state_t > 0.8f) {
            /* dark on the fireball, light once it has gone */
            bool on_ball = g_state_t < 3.8f;
            text_c(px, 92, "THE END", on_ball ? rgb565(0x100818) : white, 4);
            if (g_new_hi && blink)
                text_c(px, 136, "NEW HIGH SCORE", on_ball ? rgb565(0x400010) : yel, 2);
            if (g_state_t > 4.5f)
                text_c(px, 160, "PRESS A OR TAP", dim, 1);
        }
    }
    if (g_msg_t > 0 && blink)
        text_c(px, 40, g_msg, yel, 2);
    /* pause button */
    text_r(px, W - 6, H - 11, "II", dim, 1);
    if (g_show_fps) {
        snprintf(buf, sizeof buf, "%.0f FPS", (double)fps);
        text_at(px, 6, H - 11, buf, dim, 1);
    }
}

/* ── Records and settings ──────────────────────────────────────────────── */
#define DATA_DIR "/sd/data/skyguard"
#define REC_PATH DATA_DIR "/records.txt"
#define SET_PATH DATA_DIR "/settings.txt"
enum { CTRL_PAD, CTRL_ROLLER, NCTRL };
static const char *const ctrl_name[NCTRL] = { "Pad / stick", "Roller" };
static int g_ctrl = -1;
static int g_speed_i = 1;
static const char *const speed_name[] = { "Slow", "Normal", "Fast" };
static const float k_speed[] = { 75.f, 115.f, 165.f };

static void records_io(bool save)
{
    if (save) {
        mkdir("/sd/data", 0777);
        mkdir(DATA_DIR, 0777);
        FILE *f = fopen(REC_PATH, "w");
        if (!f)
            return;
        fprintf(f, "%d %d\n", g_hiscore, g_best_wave);
        fclose(f);
    } else {
        FILE *f = fopen(REC_PATH, "r");
        if (!f)
            return;
        if (fscanf(f, "%d %d", &g_hiscore, &g_best_wave) != 2)
            g_hiscore = g_best_wave = 0;
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
            fprintf(f, "wave=%d\ncontrols=%d\nspeed=%d\n", g_start_wave, g_ctrl, g_speed_i);
            fclose(f);
        }
    } else {
        FILE *f = fopen(SET_PATH, "r");
        if (f) {
            int v;
            if (fscanf(f, "wave=%d ", &v) == 1 && v >= 1 && v <= 99) g_start_wave = v;
            if (fscanf(f, "controls=%d ", &v) == 1 && v >= 0 && v < NCTRL) g_ctrl = v;
            if (fscanf(f, "speed=%d ", &v) == 1 && v >= 0 && v < 3) g_speed_i = v;
            fclose(f);
        }
    }
}

/* ── Input ─────────────────────────────────────────────────────────────── */
static uint32_t s_prev_btn = ~0u;
static int s_touch_n;
static float s_pot_f = -1.f;
#define POT_MID   32768.f
#define POT_RANGE 26000.f

static float stick(int raw)
{
    const float dz = 0.15f;
    float v = clampf((float)raw / 30000.f, -1.f, 1.f), m = fabsf(v);
    if (m <= dz)
        return 0;
    float u = (m - dz) / (1.f - dz);
    u = u * (0.3f + 0.7f * u);
    return v < 0 ? -u : u;
}

static input_t read_input(papp_surface_t *surf, float dt)
{
    input_t a = { .fire = -2 };
    papp_gamepad_t pad = { 0 };
    uint32_t held = papp_input_gamepad(0, &pad) ? pad.buttons : 0;
    uint32_t press = held & ~s_prev_btn;
    s_prev_btn = held;
    if (press & (PAPP_BTN_A | PAPP_BTN_START))
        a.start = true;
    if (press & (PAPP_BTN_START | PAPP_BTN_SELECT))
        a.pause = true;
    if (press & PAPP_BTN_A)
        a.fire = -1;
    else if (press & (PAPP_BTN_X | PAPP_BTN_L1))
        a.fire = 0;
    else if (press & PAPP_BTN_Y)
        a.fire = 1;
    else if (press & (PAPP_BTN_B | PAPP_BTN_R1))
        a.fire = 2;
    float sp = k_speed[g_speed_i];
    float mx = 0, my = 0;
    if (held & PAPP_BTN_LEFT) mx -= 1.f;
    if (held & PAPP_BTN_RIGHT) mx += 1.f;
    if (held & PAPP_BTN_UP) my += 1.f;
    if (held & PAPP_BTN_DOWN) my -= 1.f;
    if (mx == 0) mx = stick(pad.lx);
    if (my == 0) my = -stick(pad.ly);
    a.mz = mx * sp;
    a.my = my * sp;
    /* the roller moves the crosshair across */
    if (g_ctrl == CTRL_ROLLER) {
        int pv = papp_input_analog(PAPP_ANALOG_PADDLE);
        if (pv >= 0) {
            if (s_pot_f < 0) s_pot_f = (float)pv;
            float prev = s_pot_f;
            s_pot_f += ((float)pv - s_pot_f) * fminf(1.f, dt * 30.f);
            if (fabsf(s_pot_f - prev) > 1.f || s_touch_n == 0) {
                a.abs_set = true;
                a.abs_z_only = true;
                a.az = clampf((s_pot_f - POT_MID) / POT_RANGE, -1.f, 1.f) * FIELD_Z;
            }
        }
    }
    /* touch: tap to fire at that point; drag moves the crosshair; the bottom-right corner pauses */
    papp_touch_point_t tp[2];
    int n = papp_disp_touch(surf, tp, 2);
    if (n > 0) {
        bool first = s_touch_n == 0;
        int x = tp[0].x, y = tp[0].y;
        if (x > W - 40 && y > H - 30) {
            if (first) a.pause = true;
        } else if (g_demo || g_state == ST_OVER) {
            if (first) a.tap = true;
        } else {
            float py, pz;
            if (unproject((float)x, (float)y, &py, &pz)) {
                a.abs_set = true;
                a.abs_z_only = false;
                a.ay = clampf(py, CUR_YMIN, CUR_YMAX);
                a.az = clampf(pz, -FIELD_Z, FIELD_Z);
                if (first)
                    a.fire = -1;
            }
        }
    }
    s_touch_n = n > 0 ? n : 0;
    return a;
}

/* the computer's input: glide the crosshair to its aim and fire */
static input_t ai_input(float dt)
{
    input_t a = { .fire = -2 };
    static float ty = 60.f, tz = 0.f;
    static int pending = -2;
    int b;
    float y, z;
    if (pending == -2 && ai_aim(dt, &b, &y, &z)) {
        pending = b;
        ty = y;
        tz = z;
    }
    float dy = ty - g_cur_y, dz = tz - g_cur_z, d = sqrtf(dy * dy + dz * dz);
    float sp = 600.f;
    if (d < sp * dt || pending == -2) {
        if (pending != -2) {
            a.abs_set = true;
            a.ay = ty;
            a.az = tz;
            a.fire = pending;
            pending = -2;
        }
    } else {
        a.my = dy / d * sp;
        a.mz = dz / d * sp;
    }
    return a;
}

static void start_demo(void) { new_game(1, true); }
static void start_game(void) { new_game(g_start_wave, false); }

#ifndef SG_HOST
/* ── Presenting: a thread hands one frame to the display while the next is drawn ── */
static papp_surface_t *g_surf;
static papp_sem_t *g_pres_go, *g_pres_done;
static const uint16_t *volatile g_pres_buf;

static int presenter_main(void *arg)
{
    (void)arg;
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
    int r = papp_ui_menu(title, items, n, 0);
    papp_sem_give(g_pres_done);
    papp_input_flush();
    s_prev_btn = ~0u;          /* ignore the buttons still held */
    s_touch_n = 1;             /* and the touch that chose the item */
    return r;
}

/* returns false to exit */
static bool pause_menu(void)
{
    const char *const items[] = { "Resume", "Restart game", "Quit to title", "Exit game" };
    int r = menu("Paused", items, 4);
    if (r == 1)
        start_game();
    else if (r == 2) {
        if (g_score > g_hiscore) {
            g_hiscore = g_score;
            records_io(true);
        }
        start_demo();
    }
    return r != 3 && !papp_exit_requested();
}

/* the title menu; returns false to exit */
static bool title_menu(void)
{
    for (;;) {
        char wv[32], ctl[40], spd[32];
        snprintf(wv, sizeof wv, "Start wave: %d", g_start_wave);
        snprintf(ctl, sizeof ctl, "Controls: %s", ctrl_name[g_ctrl]);
        snprintf(spd, sizeof spd, "Crosshair: %s", speed_name[g_speed_i]);
        const char *items[5] = { "Start game", wv, ctl, spd, "Exit game" };
        int r = menu("Skyguard", items, 5);
        if (r < 0)
            return !papp_exit_requested();
        if (r == 0) {
            settings_io(true);
            start_game();
            return true;
        }
        if (r == 4)
            return false;
        if (r == 1) {
            static const char *const wn[] = { "Wave 1", "Wave 3", "Wave 5", "Wave 7", "Wave 9", "Wave 11" };
            int c = menu("Start wave", wn, 6);
            if (c >= 0)
                g_start_wave = 1 + c * 2;
        } else if (r == 2)
            g_ctrl = (g_ctrl + 1) % NCTRL;
        else if (r == 3)
            g_speed_i = (g_speed_i + 1) % 3;
        settings_io(true);
    }
}

int main(int argc, char **argv)
{
    settings_io(false);
    bool demo_only = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "demo"))
            demo_only = true;
        else if (!strcmp(argv[i], "fps"))
            g_show_fps = true;
        else if (!strcmp(argv[i], "wave") && i + 1 < argc)
            g_start_wave = (int)clampf((float)atoi(argv[++i]), 1, 99);
    }
    if (g_ctrl < 0)
        g_ctrl = papp_input_analog(PAPP_ANALOG_PADDLE) >= 0 ? CTRL_ROLLER : CTRL_PAD;

    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 1 };
    g_surf = papp_disp_surface_create(&cfg);
    uint16_t *fb[2];
    for (int i = 0; i < 2; i++)
        fb[i] = papp_alloc(sizeof(uint16_t) * W * H, PAPP_MEM_LARGE | PAPP_MEM_DMA, 64);
    g_pres_go = papp_sem_create(0, 1);
    g_pres_done = papp_sem_create(1, 1);
    papp_thread_cfg_t pcfg = { .name = "sg_present", .stack_size = 4096, .prio = PAPP_PRIO_HIGH, .core = 0 };
    papp_thread_t *pres = (g_surf && fb[0] && fb[1] && g_pres_go && g_pres_done)
                              ? papp_thread_create(presenter_main, NULL, &pcfg) : NULL;
    e3d_t *e = e3d_create(W, H, 2);
    bool ok = pres && e && b_init() && make_meshes();
    b_free();
    if (!ok) {
        papp_log("skyguard: out of memory\n");
        return 1;
    }
    e3d_set_background_rows(e, bg_row, NULL);
    records_io(false);

    g_snd_lock = papp_mutex_create(false);
    papp_audio_cfg_t acfg = { .rate = SR, .channels = 1, .format = PAPP_AUDIO_S16, .latency_ms = 60 };
    g_snd = g_snd_lock ? papp_audio_open(&acfg) : NULL;
    papp_thread_cfg_t acfg_t = { .name = "sg_audio", .stack_size = 4096, .prio = PAPP_PRIO_HIGH, .core = 1 };
    papp_thread_t *aud = g_snd ? papp_thread_create(audio_main, NULL, &acfg_t) : NULL;
    if (!aud && g_snd) {
        papp_audio_close(g_snd);
        g_snd = NULL;
    }

    start_demo();
    papp_log("skyguard: running\n");

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
        if (g_demo && !demo_only) {
            if (go || in.pause) {
                run = title_menu();
                last = papp_time_us();
                continue;
            }
        } else if (in.pause) {
            run = pause_menu();
            last = papp_time_us();
            continue;
        } else if (g_state == ST_OVER && g_state_t > 4.5f && go) {
            start_demo();
            continue;
        } else if (g_state == ST_OVER && g_state_t > 30.f)
            start_demo();
        if (g_demo) {
            input_t ai = ai_input(dt);
            update(&ai, dt);
        } else
            update(&in, dt);

        uint16_t *px = fb[cur];
        scene_begin(e, px);
        draw_scene(e);
        e3d_end(e);
        draw_hud(px, fps);

        frames++;
        if (now - stat_t >= 1000000) {
            fps = (float)frames * 1e6f / (float)(now - stat_t);
            frames = 0;
            stat_t = now;
        }
        if (now - log_t >= 5000000) {
            const e3d_stats *st = e3d_get_stats(e);
            papp_log("skyguard: %.1f fps, wave %d, state %d, score %d, cities %d, objs %d, tris %d/%d, geom %d us, raster %d us\n",
                     (double)fps, g_wave, g_state, g_score, cities_alive(), st->objects, st->tris_drawn, st->tris_in,
                     st->t_geom_us, st->t_raster_us);
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
    if (!g_demo && g_score > g_hiscore) {
        g_hiscore = g_score;
        records_io(true);
    }
    settings_io(true);
    e3d_destroy(e);
    free_meshes();
    papp_sem_destroy(g_pres_go);
    papp_sem_destroy(g_pres_done);
    papp_free(fb[0]);
    papp_free(fb[1]);
    papp_disp_surface_destroy(g_surf);
    return 0;
}
#endif /* !SG_HOST */
