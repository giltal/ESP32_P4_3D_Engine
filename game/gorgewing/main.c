/*
 * Gorgewing — a jet fighter run through a desert canyon, on the e3d engine.
 *
 * The jet flies low through a winding canyon of red rock that goes on and on:
 * a loop of 6 km, cut into chunks of 32 m, each a floor mesh (the floor layer,
 * so the jet's shadow falls on it) and a wall mesh, both in two levels of
 * detail. Stone arches span it and rock spires stand in its wider parts.
 *
 * Enemies along the way: turrets on the walls and the floor, SAM launchers
 * (their missiles home in), drones (they dive at the jet), helicopters (they fly
 * ahead of the jet and fire bursts), enemy jets (head on), floating mines and
 * barrage balloons on cables. Fuel drains all the time; the green and yellow
 * gas balloons refuel the jet (fly into them; shooting one wastes it). The run
 * ends when the jet runs out of fuel or the last life is lost.
 *
 * Controls: D-pad (or the left stick) steers, UP climbs (Pitch: inverted in the
 * menu swaps it); X = more throttle, Y = less (or R2 / L2); A = cannon (held),
 * B = missile (homes in on the locked target); START / SELECT = pause menu.
 * Touch: drag in the view to steer (a stick round where the finger lands);
 * right bar: the throttle slider, GUN, MSL; the top-right corner pauses.
 *
 * Records: /sd/data/gorgewing/records.txt (high score, longest flight),
 * settings in settings.txt there.
 *
 * Arguments: `auto` = the computer flies (no records), `god` = no damage and no
 * fuel use, `fps` = show the frame rate, `secs N` = exit after N s, `icon` = a
 * still close-up for the icon.
 */
#include "papp/papp.h"
#include "e3d.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

PAPP_MANIFEST(.id = "giltal.gorgewing", .name = "Gorgewing", .version = PAPP_APP_VERSION, .main_stack = 16384);

#define W 400
#define H 240
/* the 3D view is 320x240 in the middle of the surface; the 40-column bars beside it hold the
   fuel (left) and the throttle, GUN and MSL (right) */
#define VX 40
#define VW 320
#define VR (VX + VW)
#define PI_F 3.14159265f
#define PI2  6.2831853f
#define DEG (180.f / PI_F)
#define RAD (PI_F / 180.f)

static float frand(void) { return (float)rand() / (float)RAND_MAX; }
static float frand2(void) { return frand() * 2.f - 1.f; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float wrapf(float a)
{
    while (a > PI_F)
        a -= PI2;
    while (a < -PI_F)
        a += PI2;
    return a;
}

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

/* ── Sound: the jet's engine, guns, missiles, explosions, pick-ups ────────── */
#define SR 22050
enum { WAVE_SQUARE, WAVE_TRI, WAVE_NOISE, WAVE_BOOM };
typedef struct {
    int   left, delay, total;
    float ph, f, fmul, vol, lp;
    int   wave;
} voice_t;
#define NVOICES 10
static voice_t g_voice[NVOICES];
static papp_mutex_t *g_snd_lock;
static papp_audio_stream_t *g_snd;
static volatile bool g_snd_quit;
static uint32_t g_noise = 0x1234567u;
/* continuous sounds, set by the game each frame */
static volatile float s_eng_f, s_eng_vol, s_roar_vol;
static bool g_sound_on = true;

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
    v->lp = 0;
    v->f = f0;
    v->fmul = powf(f1 / f0, 1.f / (float)v->total);
    v->vol = vol;
    v->wave = wave;
    papp_mutex_unlock(g_snd_lock);
}
static void sfx(float f0, float f1, int ms, int wave, float vol) { sfx_at(f0, f1, ms, wave, vol, 0); }

static int audio_main(void *arg)
{
    (void)arg;
    static int16_t buf[256];
    float eph = 0, ef = 300.f, ev = 0, rv = 0, lp1 = 0, lp2 = 0, hp = 0;
    while (!g_snd_quit && !papp_exit_requested()) {
        papp_mutex_lock(g_snd_lock);
        float tf = s_eng_f, tv = s_eng_vol, tr = s_roar_vol;
        for (int i = 0; i < 256; i++) {
            ef += (tf - ef) * 0.0015f;
            ev += (tv - ev) * 0.002f;
            rv += (tr - rv) * 0.002f;
            g_noise = g_noise * 1664525u + 1013904223u;
            float nz = (float)(int32_t)g_noise * (1.f / 2147483648.f);
            /* the turbine: a soft whine and a roar (two-pole low-passed noise) */
            eph += ef * (1.f / SR);
            if (eph >= 1.f)
                eph -= 1.f;
            float tri = eph < 0.5f ? 4.f * eph - 1.f : 3.f - 4.f * eph;
            float acc = tri * ev * 0.35f;
            lp1 += (nz - lp1) * 0.10f;
            lp2 += (lp1 - lp2) * 0.10f;
            acc += lp2 * rv * 2.6f;
            hp += (nz - hp) * 0.5f;
            acc += (nz - hp) * rv * 0.10f;
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
                if (v->wave == WAVE_SQUARE)
                    s = v->ph < 0.5f ? 1.f : -1.f;
                else if (v->wave == WAVE_TRI)
                    s = v->ph < 0.5f ? 4.f * v->ph - 1.f : 3.f - 4.f * v->ph;
                else if (v->wave == WAVE_NOISE)
                    s = nz;
                else {
                    /* a rumble: noise through a low-pass that closes as f falls */
                    v->lp += (nz - v->lp) * clampf(v->f * (PI2 / SR), 0.002f, 1.f);
                    s = v->lp * 2.4f;
                }
                acc += s * v->vol * env;
                v->ph += v->f * (1.f / SR);
                if (v->ph >= 1.f)
                    v->ph -= 1.f;
                v->f *= v->fmul;
                v->left--;
            }
            acc = clampf(acc, -1.f, 1.f);
            buf[i] = g_sound_on ? (int16_t)(acc * 22000.f) : 0;
        }
        papp_mutex_unlock(g_snd_lock);
        papp_audio_write(g_snd, buf, 256, 100);
    }
    return 0;
}

/* ── Mesh building ───────────────────────────────────────────────────────────
 * A builder collects vertices and triangles, then bakes them into an e3d mesh.
 * Triangles are turned to face away from a given inside point. An optional
 * transform moves what is built (b_tf). */
#define BMAX_V 6000
#define BMAX_T 6000
typedef struct {
    int nv, nt;
    float (*pos)[3];
    struct { uint16_t v[3]; uint32_t rgb; } *tri;
    bool use_tf;
    e3d_mat tf;
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
    B.pos = NULL;
    B.tri = NULL;
}
static void b_reset(void)
{
    B.nv = B.nt = 0;
    B.use_tf = false;
}
static void b_tf(const e3d_mat *m)
{
    if (m) {
        B.tf = *m;
        B.use_tf = true;
    } else
        B.use_tf = false;
}

static int b_v(float x, float y, float z)
{
    if (B.nv >= BMAX_V)
        return BMAX_V - 1;
    float v[3] = { x, y, z };
    if (B.use_tf)
        e3d_apply(&B.tf, v);
    B.pos[B.nv][0] = v[0];
    B.pos[B.nv][1] = v[1];
    B.pos[B.nv][2] = v[2];
    return B.nv++;
}

/* outward from ctr: (v0 - v1) x (v2 - v1) points away from it */
static void b_tri(int a, int b, int c, uint32_t rgb, const float ctr_in[3])
{
    if (B.nt >= BMAX_T)
        return;
    float ctr[3] = { ctr_in[0], ctr_in[1], ctr_in[2] };
    if (B.use_tf)
        e3d_apply(&B.tf, ctr);
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
/* a quad facing along (ox, oy, oz); vertices already built (world coordinates, no transform) */
static void b_quad_out(int a, int b, int c, int d, uint32_t rgb, float ox, float oy, float oz)
{
    bool tf = B.use_tf;
    B.use_tf = false;
    float ctr[3] = { (B.pos[a][0] + B.pos[c][0]) * 0.5f - ox * 3.f, (B.pos[a][1] + B.pos[c][1]) * 0.5f - oy * 3.f,
                     (B.pos[a][2] + B.pos[c][2]) * 0.5f - oz * 3.f };
    b_quad(a, b, c, d, rgb, ctr);
    B.use_tf = tf;
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
    b_quad(v[0], v[1], v[5], v[4], shade_rgb(rgb, 0.9f), c);
    b_quad(v[2], v[3], v[7], v[6], shade_rgb(rgb, 1.08f), c);
    b_quad(v[0], v[2], v[6], v[4], rgb, c);
    b_quad(v[1], v[3], v[7], v[5], rgb, c);
}

/* a cone standing at (x, y, z): base radius r, height h, seg sides, no base */
static void b_cone(float x, float y, float z, float r, float h, int seg, uint32_t rgb, float rot)
{
    int base = B.nv;
    for (int i = 0; i < seg; i++) {
        float a = rot + PI2 * (float)i / (float)seg;
        b_v(x + cosf(a) * r, y, z + sinf(a) * r);
    }
    int tip = b_v(x, y + h, z);
    float c[3] = { x, y + h * 0.3f, z };
    for (int i = 0; i < seg; i++)
        b_tri(base + i, base + (i + 1) % seg, tip, shade_rgb(rgb, 0.9f + 0.2f * (float)(i & 1)), c);
}

/* a prism: a convex side profile (x, y), extruded across z0..z1; side_rgb per profile edge */
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

/* a slab: a convex outline (x, z), extruded from y0 to y1 (wings, tail planes) */
static void b_slab(const float (*p)[2], int n, float y0, float y1, uint32_t side, uint32_t top, uint32_t bot)
{
    int base = B.nv;
    float cx = 0, cz = 0;
    for (int i = 0; i < n; i++) {
        b_v(p[i][0], y0, p[i][1]);
        b_v(p[i][0], y1, p[i][1]);
        cx += p[i][0];
        cz += p[i][1];
    }
    float c[3] = { cx / (float)n, (y0 + y1) * 0.5f, cz / (float)n };
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        b_quad(base + 2 * i, base + 2 * i + 1, base + 2 * j + 1, base + 2 * j, side, c);
    }
    for (int i = 1; i + 1 < n; i++) {
        b_tri(base, base + 2 * i, base + 2 * (i + 1), bot, c);
        b_tri(base + 1, base + 2 * i + 1, base + 2 * (i + 1) + 1, top, c);
    }
}

/* a loft along x: ns stations, each a convex cross-section of k (z, y) points, joined by quads
   coloured per segment and edge (rgb[s * k + j], 0 = no face); cap0 / cap1 close the ends (0 = open) */
static void b_loft(const float *xs, int ns, const float (*sec)[2], int k, const uint32_t *rgb, uint32_t cap0,
                   uint32_t cap1)
{
    int base = B.nv;
    for (int st = 0; st < ns; st++)
        for (int j = 0; j < k; j++)
            b_v(xs[st], sec[st * k + j][1], sec[st * k + j][0]);
    for (int st = 0; st + 1 < ns; st++) {
        float cy = 0;
        for (int j = 0; j < k; j++)
            cy += sec[st * k + j][1] + sec[(st + 1) * k + j][1];
        float c[3] = { (xs[st] + xs[st + 1]) * 0.5f, cy / (float)(2 * k), 0 };
        for (int j = 0; j < k; j++) {
            uint32_t col = rgb[st * k + j];
            if (!col)
                continue;
            int j1 = (j + 1) % k;
            b_quad(base + st * k + j, base + st * k + j1, base + (st + 1) * k + j1, base + (st + 1) * k + j, col, c);
        }
    }
    for (int e = 0; e < 2; e++) {
        uint32_t col = e ? cap1 : cap0;
        if (!col)
            continue;
        int st = e ? ns - 1 : 0, o = base + st * k;
        float cy = 0;
        for (int j = 0; j < k; j++)
            cy += sec[st * k + j][1];
        float c[3] = { xs[st] + (e ? -1.f : 1.f) * 0.5f, cy / (float)k, 0 };
        for (int j = 1; j + 1 < k; j++)
            b_tri(o, o + j, o + j + 1, col, c);
    }
}

/* a round loft along x: radius rs and centre height yc at each station, k sides; the upper half
   in top, the lower in bot; nose: the first segment's colour (0 = as the rest) */
#define RMAX_S 8
#define RMAX_K 10
static void b_round(const float *xs, const float *rs, const float *yc, int ns, int k, float squash, uint32_t top,
                    uint32_t bot, uint32_t nose, uint32_t cap0, uint32_t cap1)
{
    float sec[RMAX_S * RMAX_K][2];
    uint32_t rgb[RMAX_S * RMAX_K];
    for (int st = 0; st < ns; st++)
        for (int j = 0; j < k; j++) {
            float a = PI2 * (float)j / (float)k;
            sec[st * k + j][0] = rs[st] * cosf(a);
            sec[st * k + j][1] = yc[st] + rs[st] * sinf(a) * squash;
        }
    for (int st = 0; st + 1 < ns; st++)
        for (int j = 0; j < k; j++) {
            float mid = PI2 * ((float)j + 0.5f) / (float)k;
            rgb[st * k + j] = (st == 0 && nose) ? nose : sinf(mid) >= 0 ? top : bot;
        }
    b_loft(xs, ns, sec, k, rgb, cap0, cap1);
}

/* a sphere at (cx, cy, cz): slices round y alternate between colours a and b */
static void b_sphere(float cx, float cy, float cz, float r, int sl, int st, uint32_t a, uint32_t b)
{
    int top = b_v(cx, cy + r, cz), bot = b_v(cx, cy - r, cz);
    int ring0 = B.nv;
    for (int i = 1; i < st; i++) {
        float ph = PI_F * (float)i / (float)st, y = cosf(ph) * r, rr = sinf(ph) * r;
        for (int j = 0; j < sl; j++) {
            float t = PI2 * (float)j / (float)sl;
            b_v(cx + cosf(t) * rr, cy + y, cz + sinf(t) * rr);
        }
    }
    float c[3] = { cx, cy, cz };
    for (int j = 0; j < sl; j++) {
        int j1 = (j + 1) % sl;
        uint32_t col = (j & 1) ? a : b;
        b_tri(top, ring0 + j, ring0 + j1, col, c);
        for (int i = 0; i + 2 < st; i++)
            b_quad(ring0 + i * sl + j, ring0 + i * sl + j1, ring0 + (i + 1) * sl + j1, ring0 + (i + 1) * sl + j, col, c);
        b_tri(bot, ring0 + (st - 2) * sl + j, ring0 + (st - 2) * sl + j1, col, c);
    }
}

/* a four-sided spike along an axis (0 x, 1 y, 2 z), from d0 out to d0 + len */
static void b_spike(int axis, float sgn, float d0, float len, float r, uint32_t rgb)
{
    int u = (axis + 1) % 3, w = (axis + 2) % 3;
    static const int ring[4] = { 0, 1, 3, 2 };
    int v[4];
    for (int k = 0; k < 4; k++) {
        float p[3] = { 0, 0, 0 };
        p[axis] = sgn * d0;
        p[u] = (ring[k] & 1) ? r : -r;
        p[w] = (ring[k] & 2) ? r : -r;
        v[k] = b_v(p[0], p[1], p[2]);
    }
    float tp[3] = { 0, 0, 0 };
    tp[axis] = sgn * (d0 + len);
    int tip = b_v(tp[0], tp[1], tp[2]);
    float c[3] = { 0, 0, 0 };
    for (int k = 0; k < 4; k++)
        b_tri(v[k], v[(k + 1) % 4], tip, rgb, c);
}

/* model -> world: move, then heading (around y), pitch (around z, nose up), roll (around x) */
static e3d_mat mat_orient(float x, float y, float z, float h, float p, float roll, float s)
{
    e3d_mat m, r;
    e3d_move(&m, x, y, z);
    e3d_rotate(&r, 0, h * DEG, 0);
    e3d_mul(&m, &r);
    if (p != 0.f || roll != 0.f) {
        e3d_rotate(&r, roll * DEG, 0, p * DEG);
        e3d_mul(&m, &r);
    }
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

/* ── The canyon ──────────────────────────────────────────────────────────────
 * Its centre line z(x) and half width are sums of sines that repeat every
 * LOOP_L metres, so the canyon loops without a seam. The walls lean out as they
 * rise; their rock is the smooth profile plus noise per station and row. */
#define LOOP_L   6144.f
#define CH_LEN   32.f
#define NCH      192
#define STA      4.f                    /* metres between wall stations */
#define SEG      8                      /* stations per chunk */
#define NSTA     (NCH * SEG)
#define LEAN     0.22f                  /* the walls lean out this much per metre of height */
#define NROWS    7
#define CEIL     55.f                   /* the highest the jet flies (the rim is at 64) */
#define JET_HALF 3.4f                   /* how close the jet's centre gets to a wall */
#define JET_R    2.6f                   /* the jet's radius against things in the air */
static const float k_rows[NROWS] = { 0, 7, 16, 27, 39, 51, 64 };
static const uint32_t k_band[NROWS - 1] = { 0x7A3420, 0x9C4628, 0xBC6438, 0xCF8450, 0xB45C36, 0xD8A068 };

static void canyon(float x, float *zc, float *hw, float *nx, float *nz)
{
    const float w = PI2 / LOOP_L;
    float a1 = 3.f * w * x + 0.4f, a2 = 7.f * w * x + 1.9f, a3 = 13.f * w * x + 4.0f, a4 = 29.f * w * x + 2.2f;
    if (zc)
        *zc = 80.f * sinf(a1) + 38.f * sinf(a2) + 16.f * sinf(a3) + 4.f * sinf(a4);
    if (hw)
        *hw = 32.f + 9.f * sinf(11.f * w * x + 0.7f) + 5.f * sinf(23.f * w * x + 2.9f);
    if (nx || nz) {
        float s = w * (240.f * cosf(a1) + 266.f * cosf(a2) + 208.f * cosf(a3) + 116.f * cosf(a4));
        float q = 1.f / sqrtf(1.f + s * s);
        if (nx)
            *nx = -s * q;
        if (nz)
            *nz = q;
    }
}
/* the canyon's direction at x (radians, like a heading) */
static float canyon_dir(float x)
{
    float nx, nz;
    canyon(x, NULL, NULL, &nx, &nz);
    return atan2f(-nx, nz);
}
/* a point lat metres across the canyon (right = +), at x: its z */
static float z_of(float x, float lat)
{
    float zc, nz;
    canyon(x, &zc, NULL, NULL, &nz);
    return zc + lat / nz;
}
static float lat_of(float x, float z, float *hw)
{
    float zc, nz;
    canyon(x, &zc, hw, NULL, &nz);
    return (z - zc) * nz;
}
/* how far from the centre the jet's centre may be at height y */
static float wall_lim(float hw, float y) { return hw + y * LEAN - JET_HALF; }
static float wrapx(float dx) { return dx - LOOP_L * floorf(dx / LOOP_L + 0.5f); }

static float hn(int j, int r, int s)
{
    uint32_t u = (uint32_t)(((j % NSTA) + NSTA) % NSTA) * 73856093u ^ (uint32_t)r * 19349663u ^ (uint32_t)(s + 2) * 83492791u;
    u ^= u >> 13;
    u *= 0x5bd1e995u;
    u ^= u >> 15;
    return (float)(u & 0xFFFF) / 32767.5f - 1.f;
}

static void wall_pt(int j, int r, int s, float out[3])
{
    if (j % SEG == 0 && r % 3 != 0) {
        /* a chunk's first station: the rows between the far detail's (0, 27, 64) lie on its
           lines, so a near chunk and a far one meet without cracks */
        int r0 = r < 3 ? 0 : 3, r1 = r0 + 3;
        float a[3], b[3];
        wall_pt(j, r0, s, a);
        wall_pt(j, r1, s, b);
        float t = (k_rows[r] - k_rows[r0]) / (k_rows[r1] - k_rows[r0]);
        for (int k = 0; k < 3; k++)
            out[k] = a[k] + (b[k] - a[k]) * t;
        return;
    }
    float x = (float)j * STA, zc, hw, nx, nz;
    canyon(x, &zc, &hw, &nx, &nz);
    float y = k_rows[r];
    float bulge = r ? 1.6f * sinf(PI2 * 50.f * (float)j / (float)NSTA + (float)r * 0.9f + (float)s * 2.1f) : 0.f;
    float d = (float)s * (hw + y * LEAN + hn(j, r, s) * (r ? 1.8f : 0.5f) + bulge);
    out[0] = x + nx * d;
    out[1] = y;
    out[2] = zc + nz * d;
}

/* stone arches across the canyon and rock spires on its floor; fixed, at the same places each loop */
#define ARCH_T 7.f
typedef struct { float x, z, nx, nz, hw; int chunk; } arch_t;
typedef struct { float x, z, r, h; int chunk; } pillar_t;
#define MAX_ARCH 12
#define MAX_PIL  64
static arch_t   g_arch[MAX_ARCH];
static pillar_t g_pil[MAX_PIL];
static int      g_narch, g_npil;
static float arch_yb(float u) { return 19.f + 13.f * (1.f - u * u); }
static float arch_yt(float u) { return 39.f + 4.f * (1.f - u * u); }
static float arch_span(float hw) { return hw + 30.f * LEAN; }

static uint32_t s_rng = 0xC0FFEEu;
static float rnd(void)
{
    s_rng = s_rng * 1664525u + 1013904223u;
    return (float)(s_rng >> 8) * (1.f / 16777216.f);
}

static void place_features(void)
{
    g_narch = g_npil = 0;
    s_rng = 0xC0FFEEu;
    for (int c = 0; c < NCH; c++) {
        if (c % 24 == 13 && g_narch < MAX_ARCH) {
            arch_t *a = &g_arch[g_narch++];
            a->x = ((float)c + 0.5f) * CH_LEN;
            canyon(a->x, &a->z, &a->hw, &a->nx, &a->nz);
            a->chunk = c;
            continue;
        }
        if (c < 4 || c % 24 == 12 || c % 24 == 14)
            continue;
        float x = ((float)c + 0.25f + rnd() * 0.5f) * CH_LEN, hw;
        canyon(x, NULL, &hw, NULL, NULL);
        if (hw > 33.f && rnd() < 0.42f && g_npil < MAX_PIL) {
            pillar_t *p = &g_pil[g_npil++];
            float lat = (rnd() < 0.5f ? -1.f : 1.f) * (0.30f + rnd() * 0.25f) * hw;
            p->x = x;
            p->z = z_of(x, lat);
            p->r = 3.f + rnd() * 1.6f;
            p->h = 30.f + rnd() * 26.f;
            p->chunk = c;
        }
    }
}

static void b_pillar(const pillar_t *p, bool far)
{
    int sides = far ? 4 : 6, nr = far ? 2 : 4;
    static const float fy[4] = { 0, 0.45f, 0.8f, 1.f }, fr[4] = { 1.f, 0.82f, 0.9f, 0.62f };
    static const float fy2[2] = { 0, 1.f }, fr2[2] = { 1.f, 0.7f };
    const float *ys = far ? fy2 : fy, *rs = far ? fr2 : fr;
    int base = B.nv;
    for (int r = 0; r < nr; r++)
        for (int i = 0; i < sides; i++) {
            float a = PI2 * (float)i / (float)sides + (float)r * 0.3f;
            b_v(p->x + cosf(a) * p->r * rs[r], ys[r] * p->h, p->z + sinf(a) * p->r * rs[r]);
        }
    int top = b_v(p->x, p->h + 1.f, p->z);
    float c[3] = { p->x, p->h * 0.5f, p->z };
    for (int r = 0; r + 1 < nr; r++)
        for (int i = 0; i < sides; i++) {
            int i1 = (i + 1) % sides;
            uint32_t col = shade_rgb(k_band[(r * 5) / nr + 1], (i & 1) ? 0.92f : 1.04f);
            b_quad(base + r * sides + i, base + r * sides + i1, base + (r + 1) * sides + i1, base + (r + 1) * sides + i,
                   col, c);
        }
    for (int i = 0; i < sides; i++)
        b_tri(base + (nr - 1) * sides + i, base + (nr - 1) * sides + (i + 1) % sides, top, 0xD8A068, c);
}

static void b_arch(const arch_t *a, int K)
{
    float span = arch_span(a->hw), tx = a->nz, tz = -a->nx;
    int base = B.nv;
    for (int k = 0; k <= K; k++) {
        float u = -1.2f + 2.4f * (float)k / (float)K, d = u * span;
        float px = a->x + a->nx * d, pz = a->z + a->nz * d;
        for (int f = -1; f <= 1; f += 2) {
            float ox = tx * (float)f * ARCH_T * 0.5f, oz = tz * (float)f * ARCH_T * 0.5f;
            b_v(px + ox, arch_yb(u), pz + oz);
            b_v(px + ox, arch_yt(u), pz + oz);
        }
    }
#define AV(k, f, t) (base + (k) * 4 + (f) * 2 + (t))
    for (int k = 0; k < K; k++) {
        uint32_t side = (k & 1) ? 0xB0603A : 0xA45834;
        b_quad_out(AV(k, 0, 0), AV(k, 1, 0), AV(k + 1, 1, 0), AV(k + 1, 0, 0), 0x8E4428, 0, -1, 0);
        b_quad_out(AV(k, 0, 1), AV(k, 1, 1), AV(k + 1, 1, 1), AV(k + 1, 0, 1), 0xC88A58, 0, 1, 0);
        b_quad_out(AV(k, 0, 0), AV(k, 0, 1), AV(k + 1, 0, 1), AV(k + 1, 0, 0), side, -tx, 0, -tz);
        b_quad_out(AV(k, 1, 0), AV(k, 1, 1), AV(k + 1, 1, 1), AV(k + 1, 1, 0), side, tx, 0, tz);
    }
#undef AV
}

/* the walls (and the rocks, spires and arches) of chunk c */
static void b_walls(int c, int lod)
{
    static const int rows_near[NROWS] = { 0, 1, 2, 3, 4, 5, 6 }, rows_far[3] = { 0, 3, 6 };
    const int *rows = lod ? rows_far : rows_near;
    int nr = lod ? 3 : NROWS, step = lod ? 2 : 1, ns = SEG / step + 1, j0 = c * SEG;
    for (int s = -1; s <= 1; s += 2) {
        int base = B.nv;
        for (int i = 0; i < ns; i++)
            for (int k = 0; k < nr; k++) {
                float p[3];
                wall_pt(j0 + i * step, rows[k], s, p);
                b_v(p[0], p[1], p[2]);
            }
        for (int i = 0; i + 1 < ns; i++)
            for (int k = 0; k + 1 < nr; k++) {
                int a = base + i * nr + k, b = a + nr;
                int band = (rows[k] + rows[k + 1]) / 2;
                if (band > NROWS - 2)
                    band = NROWS - 2;
                float vary = 0.92f + 0.12f * (hn(j0 + i * step, rows[k] + 20, s) * 0.5f + 0.5f);
                b_quad_out(a, b, b + 1, a + 1, shade_rgb(k_band[band], vary), 0, 0, -(float)s);
            }
    }
    if (!lod) {
        /* boulders at the foot of the walls */
        s_rng = 0x9E3779B9u ^ (uint32_t)c * 2654435761u;
        for (int i = 0; i < 3; i++) {
            float x = ((float)c + rnd()) * CH_LEN, hw;
            canyon(x, NULL, &hw, NULL, NULL);
            float lat = (rnd() < 0.5f ? -1.f : 1.f) * (hw - 1.5f - rnd() * 4.f);
            b_cone(x, 0, z_of(x, lat), 1.2f + rnd() * 1.6f, 1.f + rnd() * 1.6f, 5, 0x8A4A2C, rnd() * 3.f);
        }
    }
    for (int i = 0; i < g_npil; i++)
        if (g_pil[i].chunk == c)
            b_pillar(&g_pil[i], lod != 0);
    for (int i = 0; i < g_narch; i++)
        if (g_arch[i].chunk == c)
            b_arch(&g_arch[i], lod ? 4 : 10);
}

/* the floor of chunk c: scree at the wall feet, sand, a river winding down the middle */
static void b_floor(int c, int lod)
{
    int step = lod ? 2 : 1, ns = SEG / step + 1, j0 = c * SEG, np = lod ? 4 : 6;
    int base = B.nv;
    for (int i = 0; i < ns; i++) {
        int j = j0 + i * step;
        float x = (float)j * STA, zc, hw, nx, nz;
        canyon(x, &zc, &hw, &nx, &nz);
        float rc = 0.3f * hw * sinf(PI2 * 17.f * (float)j / (float)NSTA), rv = 3.5f + 1.5f * sinf(PI2 * 41.f * (float)j / (float)NSTA);
        float dn[6] = { -(hw + 5.f), -0.62f * hw, rc - rv, rc + rv, 0.62f * hw, hw + 5.f };
        float df[4] = { -(hw + 5.f), rc - rv, rc + rv, hw + 5.f };
        const float *d = lod ? df : dn;
        for (int k = 0; k < np; k++)
            b_v(x + nx * d[k], 0, zc + nz * d[k]);
    }
    static const uint32_t near_c[5][2] = { { 0xA46A44, 0x9C6440 }, { 0xCCA070, 0xC29466 }, { 0x3E7C9A, 0x4886A4 },
                                           { 0xCCA070, 0xC29466 }, { 0xA46A44, 0x9C6440 } };
    static const uint32_t far_c[3][2] = { { 0xC09468, 0xB88C60 }, { 0x3E7C9A, 0x4886A4 }, { 0xC09468, 0xB88C60 } };
    for (int i = 0; i + 1 < ns; i++) {
        int par = ((j0 / step) + i) & 1;
        for (int k = 0; k + 1 < np; k++) {
            int a = base + i * np + k, b = a + np;
            b_quad_out(a, b, b + 1, a + 1, lod ? far_c[k][par] : near_c[k][par], 0, 1, 0);
        }
    }
}

typedef struct {
    e3d_mesh *wall[2], *floor[2];
    float cx;
} chunk_t;
static chunk_t g_ch[NCH];

static bool build_canyon(void)
{
    place_features();
    for (int c = 0; c < NCH; c++) {
        chunk_t *ch = &g_ch[c];
        ch->cx = ((float)c + 0.5f) * CH_LEN;
        for (int lod = 0; lod < 2; lod++) {
            b_reset();
            b_walls(c, lod);
            ch->wall[lod] = b_bake(48.f);
            b_reset();
            b_floor(c, lod);
            ch->floor[lod] = b_bake(1.f);
            if (!ch->wall[lod] || !ch->floor[lod])
                return false;
        }
    }
    return true;
}

static void free_canyon(void)
{
    for (int c = 0; c < NCH; c++)
        for (int l = 0; l < 2; l++) {
            e3d_mesh_free(g_ch[c].wall[l]);
            e3d_mesh_free(g_ch[c].floor[l]);
            g_ch[c].wall[l] = g_ch[c].floor[l] = NULL;
        }
}

/* ── Models ──────────────────────────────────────────────────────────────────
 * Nose along +x, y up, z to the right. */
static e3d_mesh *m_jet, *m_ejet, *m_flame, *m_turret, *m_barrel, *m_sam, *m_drone, *m_heli, *m_rotor, *m_mine,
    *m_blimp, *m_cable, *m_fuel, *m_shell, *m_bullet, *m_missile, *m_part;

static e3d_mesh *make_jet(uint32_t body, uint32_t dark, uint32_t accent, uint32_t glass, uint32_t nose)
{
    b_reset();
    {
        static const float xs[6] = { 4.3f, 3.2f, 1.6f, -0.5f, -2.8f, -4.0f };
        static const float rs[6] = { 0.04f, 0.32f, 0.55f, 0.62f, 0.55f, 0.45f };
        static const float yc[6] = { 0.05f, 0.08f, 0.10f, 0.10f, 0.10f, 0.10f };
        b_round(xs, rs, yc, 6, 8, 0.85f, body, dark, nose, 0, 0x2A2420);
    }
    {
        static const float xs[4] = { 2.7f, 1.9f, 0.6f, -0.3f };
        static const float rs[4] = { 0.04f, 0.32f, 0.34f, 0.08f };
        static const float yc[4] = { 0.38f, 0.48f, 0.52f, 0.44f };
        b_round(xs, rs, yc, 4, 6, 0.9f, glass, glass, 0, 0, glass);
    }
    {
        static const float wing[5][2] = { { 1.4f, 0 }, { -2.6f, 3.3f }, { -3.3f, 3.3f }, { -3.3f, -3.3f }, { -2.6f, -3.3f } };
        b_slab(wing, 5, -0.02f, 0.10f, dark, body, dark);
        static const float stab[5][2] = { { -2.9f, 0 }, { -3.9f, 1.7f }, { -4.3f, 1.7f }, { -4.3f, -1.7f }, { -3.9f, -1.7f } };
        b_slab(stab, 5, 0.12f, 0.18f, dark, body, dark);
        static const float fin[4][2] = { { -1.8f, 0.4f }, { -3.6f, 2.3f }, { -4.2f, 2.3f }, { -4.1f, 0.4f } };
        const uint32_t fc[4] = { body, accent, body, body };
        b_prism(fin, 4, -0.07f, 0.07f, fc, body);
        /* wing tip stripes and the intakes */
        b_box(-3.3f, 0.10f, 3.0f, -2.75f, 0.13f, 3.3f, accent);
        b_box(-3.3f, 0.10f, -3.3f, -2.75f, 0.13f, -3.0f, accent);
        b_box(-1.0f, -0.35f, 0.45f, 1.0f, 0.15f, 0.85f, dark);
        b_box(-1.0f, -0.35f, -0.85f, 1.0f, 0.15f, -0.45f, dark);
    }
    return b_bake(35.f);
}

static bool make_models(void)
{
    m_jet = make_jet(0x8C9CAC, 0x4E5A66, 0xE8A020, 0x203048, 0x3A3E44);
    m_ejet = make_jet(0x7A3030, 0x3A2222, 0xE0E0E0, 0x101418, 0x202020);

    b_reset();
    {
        static const float xs[3] = { 0, -1.4f, -3.2f };
        static const float rs[3] = { 0.42f, 0.30f, 0.03f };
        static const float yc[3] = { 0.1f, 0.1f, 0.1f };
        b_round(xs, rs, yc, 3, 6, 1.f, 0xFFFFFF, 0xFFFFFF, 0, 0, 0);
    }
    m_flame = b_bake(80.f);

    b_reset();
    b_box(-1.8f, 0, -1.8f, 1.8f, 0.6f, 1.8f, 0x4A4A50);
    b_cone(0, 0.6f, 0, 1.45f, 1.5f, 6, 0x6A6E74, 0);
    m_turret = b_bake(30.f);
    b_reset();
    b_box(0, -0.22f, -0.22f, 3.2f, 0.22f, 0.22f, 0x2A2A2E);
    b_box(-0.8f, -0.45f, -0.45f, 0.4f, 0.45f, 0.45f, 0x505058);
    m_barrel = b_bake(30.f);

    b_reset();
    {
        b_box(-2.2f, 0, -1.5f, 2.2f, 1.0f, 1.5f, 0x6A6A40);
        b_box(-1.6f, 1.0f, -0.6f, 1.0f, 1.5f, 0.6f, 0x56562E);
        for (int s = -1; s <= 1; s += 2) {
            e3d_mat m = mat_orient(0, 1.7f, 0.55f * (float)s, 0, 35.f * RAD, 0, 1.f);
            b_tf(&m);
            b_box(-1.8f, -0.32f, -0.32f, 1.8f, 0.32f, 0.32f, 0x8A8A60);
            b_box(1.8f, -0.2f, -0.2f, 2.3f, 0.2f, 0.2f, 0xC02818);
            b_tf(NULL);
        }
    }
    m_sam = b_bake(30.f);

    b_reset();
    {
        b_box(-0.6f, -0.25f, -0.6f, 0.6f, 0.25f, 0.6f, 0x303034);
        b_box(0.4f, -0.12f, -0.25f, 0.75f, 0.12f, 0.25f, 0xE02010);
        for (int k = 0; k < 2; k++) {
            e3d_mat m = mat_orient(0, 0, 0, (45.f + 90.f * (float)k) * RAD, 0, 0, 1.f);
            b_tf(&m);
            b_box(-1.6f, -0.08f, -0.12f, 1.6f, 0.08f, 0.12f, 0x505058);
            b_box(1.15f, 0.12f, -0.45f, 2.05f, 0.18f, 0.45f, 0xC83020);
            b_box(-2.05f, 0.12f, -0.45f, -1.15f, 0.18f, 0.45f, 0xC83020);
            b_tf(NULL);
        }
    }
    m_drone = b_bake(30.f);

    b_reset();
    {
        static const float xs[5] = { 2.6f, 1.8f, 0.4f, -1.2f, -1.8f };
        static const float rs[5] = { 0.3f, 0.9f, 1.1f, 0.9f, 0.4f };
        static const float yc[5] = { 0.0f, 0.1f, 0.2f, 0.3f, 0.4f };
        b_round(xs, rs, yc, 5, 8, 1.f, 0x56603A, 0x3A4228, 0x1A2430, 0x1A2430, 0x3A4228);
        static const float bx[2] = { -1.8f, -5.6f }, br[2] = { 0.35f, 0.14f }, by[2] = { 0.4f, 0.7f };
        b_round(bx, br, by, 2, 6, 1.f, 0x56603A, 0x3A4228, 0, 0, 0x3A4228);
        static const float fin[4][2] = { { -5.0f, 0.6f }, { -5.8f, 2.0f }, { -6.2f, 2.0f }, { -6.0f, 0.6f } };
        const uint32_t fc[4] = { 0x56603A, 0x56603A, 0x56603A, 0x56603A };
        b_prism(fin, 4, -0.06f, 0.06f, fc, 0x56603A);
        for (int s = -1; s <= 1; s += 2) {
            float z = 0.95f * (float)s;
            b_box(-1.5f, -1.35f, z - 0.06f, 1.6f, -1.25f, z + 0.06f, 0x202020);
            b_box(-0.9f, -1.25f, z - 0.05f, -0.8f, -0.6f, z + 0.05f, 0x202020);
            b_box(0.8f, -1.25f, z - 0.05f, 0.9f, -0.6f, z + 0.05f, 0x202020);
        }
        b_box(-0.15f, 1.1f, -0.15f, 0.15f, 1.6f, 0.15f, 0x303030);
    }
    m_heli = b_bake(35.f);
    b_reset();
    b_box(-5.5f, -0.04f, -0.25f, 5.5f, 0.04f, 0.25f, 0x202020);
    b_box(-0.25f, -0.04f, -5.5f, 0.25f, 0.04f, 5.5f, 0x202020);
    m_rotor = b_bake(30.f);

    b_reset();
    b_sphere(0, 0, 0, 1.4f, 8, 6, 0x40404A, 0x4A4A56);
    for (int a = 0; a < 3; a++) {
        b_spike(a, 1.f, 1.1f, 1.3f, 0.28f, 0x9A9AA4);
        b_spike(a, -1.f, 1.1f, 1.3f, 0.28f, 0x9A9AA4);
    }
    m_mine = b_bake(40.f);

    b_reset();
    {
        static const float xs[6] = { 4.5f, 3.6f, 2.0f, -0.5f, -3.0f, -4.6f };
        static const float rs[6] = { 0.2f, 1.6f, 2.3f, 2.4f, 1.7f, 0.5f };
        static const float yc[6] = { 0, 0, 0, 0, 0, 0 };
        b_round(xs, rs, yc, 6, 8, 1.f, 0xB8BCC0, 0x8C9094, 0, 0, 0x8C9094);
        static const float fin[4][2] = { { -2.4f, 1.2f }, { -4.7f, 3.3f }, { -5.3f, 3.3f }, { -4.9f, 0.3f } };
        const uint32_t fc[4] = { 0xA0A4A8, 0xD03020, 0xA0A4A8, 0xA0A4A8 };
        b_prism(fin, 4, -0.08f, 0.08f, fc, 0xA0A4A8);
        static const float hf[5][2] = { { -2.4f, 0 }, { -4.8f, 3.0f }, { -5.3f, 3.0f }, { -5.3f, -3.0f }, { -4.8f, -3.0f } };
        b_slab(hf, 5, -0.08f, 0.08f, 0xA0A4A8, 0xA0A4A8, 0x8C9094);
    }
    m_blimp = b_bake(35.f);
    b_reset();
    b_box(-0.09f, 0, -0.09f, 0.09f, 1.f, 0.09f, 0x302C28);
    m_cable = b_bake(1.f);

    b_reset();
    b_sphere(0, 0, 0, 2.8f, 10, 7, 0x30C040, 0xF0D020);
    b_box(-0.05f, -3.6f, -0.05f, 0.05f, -2.6f, 0.05f, 0x403020);
    b_box(-0.6f, -4.7f, -0.6f, 0.6f, -3.5f, 0.6f, 0xE04020);
    m_fuel = b_bake(40.f);

    b_reset();
    b_box(-0.45f, -0.45f, -0.45f, 0.45f, 0.45f, 0.45f, 0xFFFFFF);
    m_shell = b_bake(1.f);
    b_reset();
    b_box(-1.6f, -0.12f, -0.12f, 1.6f, 0.12f, 0.12f, 0xFFFFFF);
    m_bullet = b_bake(1.f);
    b_reset();
    {
        static const float xs[3] = { 1.0f, 0.6f, -1.0f };
        static const float rs[3] = { 0.03f, 0.17f, 0.17f };
        static const float yc[3] = { 0, 0, 0 };
        b_round(xs, rs, yc, 3, 6, 1.f, 0xE8E8E8, 0xC8C8C8, 0xD02818, 0, 0x404040);
        static const float fin[4][2] = { { -0.5f, 0 }, { -1.0f, 0.45f }, { -1.0f, -0.45f }, { -0.9f, 0 } };
        b_slab(fin, 3, -0.03f, 0.03f, 0x808080, 0x909090, 0x808080);
    }
    m_missile = b_bake(40.f);
    b_reset();
    b_box(-0.5f, -0.5f, -0.5f, 0.5f, 0.5f, 0.5f, 0xFFFFFF);
    m_part = b_bake(1.f);

    return m_jet && m_ejet && m_flame && m_turret && m_barrel && m_sam && m_drone && m_heli && m_rotor && m_mine &&
           m_blimp && m_cable && m_fuel && m_shell && m_bullet && m_missile && m_part;
}

static void free_models(void)
{
    e3d_mesh *all[] = { m_jet, m_ejet, m_flame, m_turret, m_barrel, m_sam, m_drone, m_heli, m_rotor, m_mine,
                        m_blimp, m_cable, m_fuel, m_shell, m_bullet, m_missile, m_part };
    for (size_t i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        e3d_mesh_free(all[i]);
}

/* ── The game ────────────────────────────────────────────────────────────── */
#define V_MIN     38.f                  /* m/s at no throttle */
#define V_MAX     105.f                 /* at full throttle */
#define YAW_MAX   1.25f                 /* rad/s */
#define PITCH_MAX 0.45f                 /* rad */
#define MSL_MAX   9
#define START_MSL 6

typedef struct {
    float x, y, z, h, p, roll, yr, v, thr;
    float fuel, hull, invul, gun_t, wall_t;
    int   lives, msl, gun_side;
    bool  alive;
} jet_t;
static jet_t g_jet;

enum { ST_TITLE, ST_PLAY, ST_DOWN, ST_OVER };
static int   g_state;
static float g_state_t, g_t;
static int   g_score, g_hi, g_best_dist, g_sector, g_runs;
static float g_dist, g_dist_pts;
static bool  g_new_hi, g_over_fuel, g_flameout;
static bool  g_auto, g_god, g_icon, g_show_fps, g_invert, g_haze, g_keys;
static float g_msg_t, g_hurt_t, g_shake, g_alarm_t;
static char  g_msg[40];
static uint32_t g_msg_col;
static float g_cam_h, g_cam_y;

static void note(const char *msg, uint32_t col)
{
    snprintf(g_msg, sizeof(g_msg), "%s", msg);
    g_msg_t = 1.8f;
    g_msg_col = col;
}
static float diff(void) { return clampf(g_dist / 15000.f, 0.f, 1.f); }
static bool demo(void) { return g_state == ST_TITLE || g_auto; }

/* records and settings */
#define DATA_DIR "/sd/data/gorgewing"
#define REC_PATH DATA_DIR "/records.txt"
#define SET_PATH DATA_DIR "/settings.txt"
static void settings_io(bool save)
{
    if (save) {
        mkdir("/sd/data", 0777);
        mkdir(DATA_DIR, 0777);
        FILE *f = fopen(SET_PATH, "w");
        if (f) {
            fprintf(f, "invert=%d\nsound=%d\nfps=%d\nhaze=%d\n", g_invert ? 1 : 0, g_sound_on ? 1 : 0,
                    g_show_fps ? 1 : 0, g_haze ? 1 : 0);
            fclose(f);
        }
    } else {
        FILE *f = fopen(SET_PATH, "r");
        if (f) {
            int v;
            if (fscanf(f, "invert=%d ", &v) == 1)
                g_invert = v != 0;
            if (fscanf(f, "sound=%d ", &v) == 1)
                g_sound_on = v != 0;
            if (fscanf(f, "fps=%d ", &v) == 1)
                g_show_fps = v != 0;
            if (fscanf(f, "haze=%d", &v) == 1)
                g_haze = v != 0;
            fclose(f);
        }
    }
}
static void records_io(bool save)
{
    if (save) {
        mkdir("/sd/data", 0777);
        mkdir(DATA_DIR, 0777);
        FILE *f = fopen(REC_PATH, "w");
        if (f) {
            fprintf(f, "GORGEWING 1 score %d dist %d\n", g_hi, g_best_dist);
            fclose(f);
        }
    } else {
        FILE *f = fopen(REC_PATH, "r");
        if (f) {
            int s, d;
            if (fscanf(f, "GORGEWING 1 score %d dist %d", &s, &d) == 2 && s >= 0 && d >= 0) {
                g_hi = s;
                g_best_dist = d;
            }
            fclose(f);
        }
    }
}

/* ── Things in the canyon ────────────────────────────────────────────────── */
enum { E_TURRET, E_SAM, E_DRONE, E_HELI, E_JET, E_MINE, E_BLIMP, E_FUEL, E_EMSL, NE_TYPES };
static const float k_erad[NE_TYPES]  = { 2.6f, 3.0f, 2.0f, 4.0f, 3.8f, 2.4f, 4.6f, 3.4f, 1.2f };
static const int   k_ehp[NE_TYPES]   = { 3, 4, 1, 8, 3, 1, 3, 1, 1 };
static const int   k_escore[NE_TYPES] = { 100, 200, 150, 400, 500, 50, 75, 0, 50 };

typedef struct {
    uint8_t  type, on, mode, burst;
    int8_t   side;                      /* turrets: -1 left wall, +1 right wall, 0 on the floor */
    uint16_t serial;
    float x, y, z, lat, blat, by;
    float vx, vy, vz;
    float hp, t, fire, ph, aim_h, aim_p, hit_t;
} ent_t;
#define MAX_ENT 72
static ent_t    g_ent[MAX_ENT];
static uint16_t g_serial;
static float    g_spawn_x, g_fuel_x;

typedef struct { float p[3], v[3], life; float dmg; } shot_t;          /* enemy shells */
typedef struct { float p[3], v[3], life; } bullet_t;                   /* the jet's cannon */
typedef struct { float p[3], v[3], life, smoke; int target; uint16_t serial; } missile_t;
#define MAX_SHOT 48
#define MAX_BUL  40
#define MAX_MSL  4
static shot_t    g_shot[MAX_SHOT];
static bullet_t  g_bul[MAX_BUL];
static missile_t g_msl[MAX_MSL];
static int       g_lock = -1;            /* the missile target, an index in g_ent */
static float     g_lock_t;

typedef struct { float p[3], v[3], life, life0, size, grav; uint32_t c0, c1; } part_t;
#define MAX_PT 80
static part_t g_pt[MAX_PT];

typedef struct { float p[3]; float t; int val; } popup_t;
#define MAX_POP 6
static popup_t g_pop[MAX_POP];

static void spawn_part(const float p[3], float vx, float vy, float vz, float life, float size, uint32_t c0, uint32_t c1,
                       float grav)
{
    int best = -1;
    float least = 1e9f;
    for (int i = 0; i < MAX_PT; i++) {
        if (g_pt[i].life <= 0) {
            best = i;
            break;
        }
        if (g_pt[i].life < least) {
            least = g_pt[i].life;
            best = i;
        }
    }
    part_t *q = &g_pt[best];
    memcpy(q->p, p, sizeof(q->p));
    q->v[0] = vx;
    q->v[1] = vy;
    q->v[2] = vz;
    q->life = q->life0 = life;
    q->size = size;
    q->c0 = c0;
    q->c1 = c1;
    q->grav = grav;
}

static void boom(const float p[3], float size)
{
    int n = 5 + (int)(size * 4.f);
    for (int i = 0; i < n; i++) {
        float s = 6.f + size * 7.f;
        uint32_t c = i % 3 == 0 ? 0xFFF0A0 : i % 3 == 1 ? 0xFFA030 : 0xE05020;
        spawn_part(p, frand2() * s, frand2() * s + 3.f, frand2() * s, 0.4f + frand() * 0.5f, 0.8f + size * 0.6f * frand(),
                   c, 0x402A20, 4.f);
    }
    for (int i = 0; i < 2 + (int)size; i++)
        spawn_part(p, frand2() * 3.f, 2.f + frand() * 3.f, frand2() * 3.f, 1.0f + frand() * 0.6f, 1.0f + size * 0.4f,
                   0x806858, 0x4A4440, -1.f);
    float f = 180.f / (1.f + size * 0.5f);
    sfx(f, f * 0.25f, 300 + (int)(size * 220.f), WAVE_BOOM, 0.45f + size * 0.12f);
}

static void popup(const float p[3], int val)
{
    for (int i = 0; i < MAX_POP; i++)
        if (g_pop[i].t <= 0 || i == MAX_POP - 1) {
            memcpy(g_pop[i].p, p, sizeof(g_pop[i].p));
            g_pop[i].t = 1.0f;
            g_pop[i].val = val;
            return;
        }
}

static void add_score(int pts)
{
    if (g_state != ST_PLAY)
        return;
    g_score += pts;
}

static ent_t *spawn_ent(int type, float x, float lat, float y)
{
    for (int i = 0; i < MAX_ENT; i++) {
        ent_t *e = &g_ent[i];
        if (e->on)
            continue;
        memset(e, 0, sizeof(*e));
        e->on = 1;
        e->type = (uint8_t)type;
        e->serial = ++g_serial;
        e->x = x;
        e->lat = e->blat = lat;
        e->y = e->by = y;
        e->z = z_of(x, lat);
        e->hp = (float)k_ehp[type];
        e->fire = 0.6f + frand();
        e->ph = frand() * PI2;
        return e;
    }
    return NULL;
}

/* is (x, lat) next to a spire or an arch? */
static bool near_pillar(float x, float lat, float margin)
{
    float z = z_of(x, lat);
    for (int i = 0; i < g_npil; i++) {
        float dx = wrapx(g_pil[i].x - x), dz = g_pil[i].z - z;
        if (dx * dx + dz * dz < (g_pil[i].r + margin) * (g_pil[i].r + margin))
            return true;
    }
    return false;
}
static bool near_arch(float x, float margin)
{
    for (int i = 0; i < g_narch; i++)
        if (fabsf(wrapx(g_arch[i].x - x)) < margin)
            return true;
    return false;
}

static float free_lat(float x, float lat)
{
    if (near_pillar(x, lat, 6.f))
        lat = -lat;
    if (near_pillar(x, lat, 6.f))
        lat = 0;
    return lat;
}

static void spawn_group(float x)
{
    float d = diff(), hw;
    canyon(x, NULL, &hw, NULL, NULL);
    float wt[7] = { 22.f, 4.f + 10.f * d, 16.f, 4.f + 8.f * d, 2.f + 9.f * d, 12.f, 8.f };
    float sum = 0;
    for (int i = 0; i < 7; i++)
        sum += wt[i];
    float r = frand() * sum;
    int type = 0;
    while (type < 6 && r > wt[type])
        r -= wt[type++];
    bool arch = near_arch(x, 30.f);
    if (arch && (type == E_BLIMP || type == E_HELI))
        type = E_TURRET;
    switch (type) {
    case E_TURRET: {
        int n = 1 + (frand() < 0.4f + 0.4f * d);
        for (int i = 0; i < n; i++) {
            float xx = x + (float)i * 22.f;
            if (frand() < 0.65f) {
                int side = frand() < 0.5f ? -1 : 1;
                float y = 8.f + frand() * 28.f, hw2;
                canyon(xx, NULL, &hw2, NULL, NULL);
                ent_t *e = spawn_ent(E_TURRET, xx, (float)side * (hw2 + y * LEAN - 0.6f), y);
                if (e)
                    e->side = (int8_t)side;
            } else
                spawn_ent(E_TURRET, xx, free_lat(xx, frand2() * 0.6f * hw), 0);
        }
        break;
    }
    case E_SAM:
        spawn_ent(E_SAM, x, free_lat(x, frand2() * 0.6f * hw), 0);
        break;
    case E_DRONE: {
        float lat = frand2() * 0.4f * hw, y = 12.f + frand() * 24.f;
        int n = 2 + (frand() < 0.5f + 0.5f * d);
        for (int i = 0; i < n; i++)
            spawn_ent(E_DRONE, x + (float)i * 8.f, lat + (float)(i - 1) * 7.f, y + (float)(i & 1) * 3.f);
        break;
    }
    case E_HELI:
        spawn_ent(E_HELI, x, frand2() * 0.3f * hw, 18.f + frand() * 16.f);
        break;
    case E_JET: {
        int n = 1 + (frand() < 0.3f + 0.4f * d);
        for (int i = 0; i < n; i++)
            spawn_ent(E_JET, x + 40.f + (float)i * 30.f, frand2() * 0.4f * hw, 14.f + frand() * 26.f);
        break;
    }
    case E_MINE: {
        int n = 3 + (int)(frand() * (3.f + 3.f * d));
        for (int i = 0; i < n; i++) {
            float xx = x + frand() * 40.f, lat = frand2() * 0.75f * hw;
            float y = arch ? 5.f + frand() * 12.f : 6.f + frand() * 40.f;
            spawn_ent(E_MINE, xx, free_lat(xx, lat), y);
        }
        break;
    }
    default: {
        int n = 2 + (hw > 34.f);
        for (int i = 0; i < n; i++) {
            float u = n == 2 ? (i ? 0.35f : -0.35f) : (float)(i - 1) * 0.55f;
            float lat = free_lat(x, u * hw);
            spawn_ent(E_BLIMP, x + frand() * 8.f, lat, 34.f + frand() * 12.f);
        }
        break;
    }
    }
}

static void spawner(void)
{
    float ahead = g_jet.x + 440.f, d = diff();
    while (g_spawn_x < ahead) {
        spawn_group(g_spawn_x);
        g_spawn_x += (95.f - 45.f * d) + frand() * 45.f;
    }
    while (g_fuel_x < ahead) {
        float hw;
        canyon(g_fuel_x, NULL, &hw, NULL, NULL);
        float y = near_arch(g_fuel_x, 20.f) ? 10.f + frand() * 8.f : 10.f + frand() * 32.f;
        spawn_ent(E_FUEL, g_fuel_x, free_lat(g_fuel_x, frand2() * 0.6f * hw), y);
        g_fuel_x += 400.f + 260.f * d + frand() * 140.f;
    }
}

static void clear_ahead(float range)
{
    for (int i = 0; i < MAX_ENT; i++) {
        ent_t *e = &g_ent[i];
        if (e->on && e->type != E_FUEL && (e->type == E_EMSL || e->x - g_jet.x < range))
            e->on = 0;
    }
    memset(g_shot, 0, sizeof(g_shot));
}

/* everything moves back by a loop when the jet passes its end (the canyon repeats) */
static void shift_all(float dx)
{
    g_jet.x += dx;
    g_spawn_x += dx;
    g_fuel_x += dx;
    for (int i = 0; i < MAX_ENT; i++)
        g_ent[i].x += dx;
    for (int i = 0; i < MAX_SHOT; i++)
        g_shot[i].p[0] += dx;
    for (int i = 0; i < MAX_BUL; i++)
        g_bul[i].p[0] += dx;
    for (int i = 0; i < MAX_MSL; i++)
        g_msl[i].p[0] += dx;
    for (int i = 0; i < MAX_PT; i++)
        g_pt[i].p[0] += dx;
    for (int i = 0; i < MAX_POP; i++)
        g_pop[i].p[0] += dx;
}

/* ── The jet ─────────────────────────────────────────────────────────────── */
static void crash(bool fuel);

static void hurt(float dmg)
{
    jet_t *j = &g_jet;
    if (!j->alive || j->invul > 0 || g_god || g_state != ST_PLAY)
        return;
    j->hull -= dmg;
    g_hurt_t = 0.3f;
    g_shake = fmaxf(g_shake, dmg * 0.05f);
    sfx(160.f, 50.f, 180, WAVE_SQUARE, 0.25f);
    sfx(400.f, 100.f, 120, WAVE_BOOM, 0.3f);
    if (j->hull <= 0) {
        j->hull = 0;
        crash(false);
    }
}

static void crash(bool fuel)
{
    jet_t *j = &g_jet;
    if (!j->alive)
        return;
    float p[3] = { j->x, j->y, j->z };
    boom(p, 3.f);
    boom(p, 2.f);
    j->alive = false;
    j->lives--;
    g_over_fuel = fuel;
    g_state = ST_DOWN;
    g_state_t = 0;
    g_shake = 1.f;
}

static void reset_jet(float x)
{
    jet_t *j = &g_jet;
    j->x = x;
    j->z = z_of(x, 0);
    j->y = 22.f;
    j->h = canyon_dir(x);
    j->p = j->roll = j->yr = 0;
    j->v = 65.f;
    j->thr = 0.4f;
    j->alive = true;
    j->invul = 0;
    j->wall_t = 0;
}

static void new_game(void)
{
    jet_t *j = &g_jet;
    float x = j->x;
    reset_jet(x);
    j->fuel = 100.f;
    j->hull = 100.f;
    j->lives = 3;
    j->msl = START_MSL;
    j->invul = 2.f;
    g_score = 0;
    g_dist = g_dist_pts = 0;
    g_sector = 1;
    g_new_hi = false;
    g_flameout = false;
    clear_ahead(500.f);
    g_spawn_x = x + 260.f;
    g_fuel_x = x + 320.f;
    g_state = ST_PLAY;
    g_state_t = 0;
    g_runs++;
    note("GET READY", 0xFFE0);
}

static void game_over(void)
{
    g_state = ST_OVER;
    g_state_t = 0;
    if (!g_auto) {
        bool save = false;
        if (g_score > g_hi) {
            g_hi = g_score;
            g_new_hi = true;
            save = true;
        }
        if ((int)g_dist > g_best_dist) {
            g_best_dist = (int)g_dist;
            save = true;
        }
        if (save)
            records_io(true);
    }
}

typedef struct {
    float sx, sy, dthr, thr_set;
    bool gun, msl, start, pause, tap;
} input_t;

static void sparks(float x, float y, float z, int n)
{
    float p[3] = { x, y, z };
    for (int i = 0; i < n; i++)
        spawn_part(p, frand2() * 10.f, frand() * 8.f, frand2() * 10.f, 0.25f + frand() * 0.25f, 0.35f, 0xFFF0B0,
                   0xFF6020, 12.f);
}

static void jet_step(const input_t *in, float dt)
{
    jet_t *j = &g_jet;
    float sx = in->sx, sy = in->sy;
    if (j->fuel <= 0) {
        j->thr = 0;
        sy = -0.5f;
        sx *= 0.4f;
    } else if (in->thr_set >= 0)
        j->thr = in->thr_set;
    else
        j->thr = clampf(j->thr + in->dthr * dt, 0, 1);
    float vt = j->fuel > 0 ? V_MIN + (V_MAX - V_MIN) * j->thr : 34.f;
    j->v += (vt - j->v) * fminf(1.f, 1.2f * dt);
    j->yr += (sx * YAW_MAX - j->yr) * fminf(1.f, 5.f * dt);
    j->h += j->yr * dt;
    float hc = canyon_dir(j->x), dh = wrapf(j->h - hc);
    if (dh > 1.15f) {
        j->h = hc + 1.15f;
        j->yr = fminf(j->yr, 0);
    } else if (dh < -1.15f) {
        j->h = hc - 1.15f;
        j->yr = fmaxf(j->yr, 0);
    }
    j->h = wrapf(j->h);
    float roll_t = -(j->yr / YAW_MAX) * 62.f * RAD;
    j->roll += (roll_t - j->roll) * fminf(1.f, 6.f * dt);
    j->p += (sy * PITCH_MAX - j->p) * fminf(1.f, 3.5f * dt);
    float cp = cosf(j->p);
    j->x += j->v * cp * cosf(j->h) * dt;
    j->z += j->v * cp * sinf(j->h) * dt;
    j->y += j->v * sinf(j->p) * dt;
    if (j->y > CEIL) {
        j->y = CEIL;
        if (j->p > 0)
            j->p *= 0.5f;
    }
    if (j->invul > 0)
        j->invul -= dt;
    if (j->wall_t > 0)
        j->wall_t -= dt;
    if (j->alive && g_state == ST_PLAY && !g_god) {
        float use = (1.0f + 1.8f * j->thr) * dt;
        if (j->fuel > 0 && j->fuel - use <= 0) {
            g_flameout = true;
            note("OUT OF FUEL", 0xF800);
        }
        j->fuel = fmaxf(0, j->fuel - use);
    }

    /* the floor, the walls, the spires and the arches */
    bool solid = j->alive;
    if (j->y < 2.4f) {
        if (solid && j->fuel <= 0 && g_state == ST_PLAY) {
            crash(true);
            return;
        }
        if (solid && j->wall_t <= 0) {
            hurt(15.f);
            j->wall_t = 0.4f;
            sparks(j->x, 0.3f, j->z, 8);
        }
        j->y = 2.4f;
        j->p = fmaxf(j->p, 0.25f);
    }
    float hw, lat = lat_of(j->x, j->z, &hw), lim = wall_lim(hw, j->y);
    if (fabsf(lat) > lim) {
        float s = lat > 0 ? 1.f : -1.f;
        lat = s * (lim - 0.6f);
        j->z = z_of(j->x, lat);
        j->h = hc - s * 0.18f;
        j->yr = 0;
        if (solid && j->wall_t <= 0) {
            hurt(12.f);
            j->wall_t = 0.35f;
            sparks(j->x, j->y, z_of(j->x, s * (lim + JET_HALF - 0.5f)), 10);
            g_shake = fmaxf(g_shake, 0.8f);
        }
    }
    if (!solid)
        return;
    for (int i = 0; i < g_npil; i++) {
        const pillar_t *p = &g_pil[i];
        float dx = wrapx(p->x - j->x);
        if (fabsf(dx) > 12.f || j->y > p->h + 1.f)
            continue;
        float dz = p->z - j->z, d = sqrtf(dx * dx + dz * dz), r = p->r + 2.6f;
        if (d < r && d > 0.01f) {
            j->x -= dx / d * (r - d + 0.3f);
            j->z -= dz / d * (r - d + 0.3f);
            if (j->wall_t <= 0) {
                hurt(25.f);
                j->wall_t = 0.5f;
                sparks(j->x + dx * 0.5f, j->y, j->z + dz * 0.5f, 10);
            }
        }
    }
    for (int i = 0; i < g_narch; i++) {
        const arch_t *a = &g_arch[i];
        float dx = wrapx(a->x - j->x);
        if (fabsf(dx) > 20.f)
            continue;
        float tx = a->nz, tz = -a->nx;
        float along = -dx * tx + (j->z - a->z) * tz;
        if (fabsf(along) > ARCH_T * 0.5f + 2.2f)
            continue;
        float u = lat / arch_span(a->hw), yb = arch_yb(u), yt = arch_yt(u);
        if (j->y > yb - 2.0f && j->y < yt + 2.0f) {
            j->y = j->y < (yb + yt) * 0.5f ? yb - 2.2f : yt + 2.2f;
            if (j->wall_t <= 0) {
                hurt(34.f);
                j->wall_t = 0.6f;
                sparks(j->x, j->y, j->z, 14);
                g_shake = 1.2f;
            }
        }
    }
}

/* ── Weapons ─────────────────────────────────────────────────────────────── */
static void jet_fwd(float out[3])
{
    float cp = cosf(g_jet.p);
    out[0] = cp * cosf(g_jet.h);
    out[1] = sinf(g_jet.p);
    out[2] = cp * sinf(g_jet.h);
}

/* the best missile target: in a cone ahead of the nose, the nearest to its line */
static int find_lock(void)
{
    const jet_t *j = &g_jet;
    float f[3];
    jet_fwd(f);
    int best = -1;
    float bs = 1e9f;
    for (int i = 0; i < MAX_ENT; i++) {
        const ent_t *e = &g_ent[i];
        if (!e->on || e->type == E_FUEL || e->type == E_EMSL)
            continue;
        float d[3] = { e->x - j->x, e->y - j->y, e->z - j->z };
        float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (dist < 25.f || dist > 420.f)
            continue;
        float c = (d[0] * f[0] + d[1] * f[1] + d[2] * f[2]) / dist;
        if (c < 0.95f)                    /* 18 degrees */
            continue;
        float s = (1.f - c) * 3000.f + dist * 0.2f;
        if (s < bs) {
            bs = s;
            best = i;
        }
    }
    return best;
}

/* the segment p0..p1 against a sphere */
static bool seg_hit(const float p0[3], const float p1[3], const float c[3], float r)
{
    float d[3] = { p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2] };
    float m[3] = { c[0] - p0[0], c[1] - p0[1], c[2] - p0[2] };
    float dd = d[0] * d[0] + d[1] * d[1] + d[2] * d[2];
    float t = dd > 0 ? clampf((m[0] * d[0] + m[1] * d[1] + m[2] * d[2]) / dd, 0, 1) : 0;
    float q[3] = { p0[0] + d[0] * t - c[0], p0[1] + d[1] * t - c[1], p0[2] + d[2] * t - c[2] };
    return q[0] * q[0] + q[1] * q[1] + q[2] * q[2] < r * r;
}

static void kill_ent(ent_t *e, bool by_player)
{
    float p[3] = { e->x, e->y, e->z };
    if (e->type == E_FUEL) {
        sfx(500.f, 200.f, 200, WAVE_TRI, 0.3f);
        for (int i = 0; i < 8; i++)
            spawn_part(p, frand2() * 8.f, frand2() * 8.f, frand2() * 8.f, 0.5f, 0.9f, 0x40E050, 0xF0D020, 2.f);
        if (by_player && g_state == ST_PLAY)
            note("FUEL LOST", 0xFD20);
        e->on = 0;
        return;
    }
    boom(p, e->type == E_HELI || e->type == E_JET ? 2.2f : e->type == E_DRONE || e->type == E_MINE || e->type == E_EMSL ? 0.8f : 1.4f);
    if (by_player && k_escore[e->type]) {
        add_score(k_escore[e->type]);
        if (g_state == ST_PLAY)
            popup(p, k_escore[e->type]);
    }
    e->on = 0;
}

static void damage_ent(ent_t *e, float dmg)
{
    e->hp -= dmg;
    e->hit_t = 0.07f;
    if (e->hp <= 0)
        kill_ent(e, true);
}

static void fire_gun(void)
{
    jet_t *j = &g_jet;
    float f[3];
    jet_fwd(f);
    float rx = -sinf(j->h), rz = cosf(j->h), side = j->gun_side ? 1.f : -1.f;
    j->gun_side ^= 1;
    for (int i = 0; i < MAX_BUL; i++) {
        bullet_t *b = &g_bul[i];
        if (b->life > 0)
            continue;
        b->p[0] = j->x + f[0] * 2.f + rx * side * 1.6f;
        b->p[1] = j->y + f[1] * 2.f;
        b->p[2] = j->z + f[2] * 2.f + rz * side * 1.6f;
        float s = j->v + 380.f;
        b->v[0] = f[0] * s;
        b->v[1] = f[1] * s;
        b->v[2] = f[2] * s;
        b->life = 0.95f;
        break;
    }
    sfx(900.f, 300.f, 45, WAVE_NOISE, 0.13f);
}

static void fire_missile(void)
{
    jet_t *j = &g_jet;
    for (int i = 0; i < MAX_MSL; i++) {
        missile_t *m = &g_msl[i];
        if (m->life > 0)
            continue;
        float f[3];
        jet_fwd(f);
        m->p[0] = j->x + f[0];
        m->p[1] = j->y - 0.8f;
        m->p[2] = j->z + f[2];
        float s = j->v + 15.f;
        m->v[0] = f[0] * s;
        m->v[1] = f[1] * s - 2.f;
        m->v[2] = f[2] * s;
        m->life = 3.2f;
        m->smoke = 0;
        m->target = g_lock;
        m->serial = g_lock >= 0 ? g_ent[g_lock].serial : 0;
        j->msl--;
        sfx(300.f, 900.f, 500, WAVE_NOISE, 0.22f);
        sfx(200.f, 60.f, 400, WAVE_BOOM, 0.25f);
        return;
    }
}

/* turns the velocity v towards the direction d (unit), at most max radians */
static void steer_vec(float v[3], const float d[3], float speed, float max)
{
    float s = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (s < 0.01f)
        s = 0.01f;
    float u[3] = { v[0] / s, v[1] / s, v[2] / s };
    float c = clampf(u[0] * d[0] + u[1] * d[1] + u[2] * d[2], -1.f, 1.f), a = acosf(c);
    float k = a > max ? max / a : 1.f;
    for (int i = 0; i < 3; i++)
        u[i] += (d[i] - u[i]) * k;
    float n = sqrtf(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (int i = 0; i < 3; i++)
        v[i] = u[i] / n * speed;
}

static void weapons_step(float dt)
{
    /* the cannon */
    for (int i = 0; i < MAX_BUL; i++) {
        bullet_t *b = &g_bul[i];
        if (b->life <= 0)
            continue;
        float p0[3] = { b->p[0], b->p[1], b->p[2] };
        for (int k = 0; k < 3; k++)
            b->p[k] += b->v[k] * dt;
        b->life -= dt;
        float hw, lat = lat_of(b->p[0], b->p[2], &hw);
        if (b->p[1] < 0 || fabsf(lat) > hw + b->p[1] * LEAN) {
            if (b->p[1] < 70.f)
                sparks(b->p[0], fmaxf(b->p[1], 0.2f), b->p[2], 2);
            b->life = 0;
            continue;
        }
        for (int e = 0; e < MAX_ENT; e++) {
            ent_t *en = &g_ent[e];
            if (!en->on)
                continue;
            float c[3] = { en->x, en->y, en->z };
            if (fabsf(c[0] - b->p[0]) > 30.f)
                continue;
            if (seg_hit(p0, b->p, c, k_erad[en->type])) {
                damage_ent(en, 1.f);
                sparks(b->p[0], b->p[1], b->p[2], 2);
                b->life = 0;
                break;
            }
        }
    }
    /* missiles */
    for (int i = 0; i < MAX_MSL; i++) {
        missile_t *m = &g_msl[i];
        if (m->life <= 0)
            continue;
        m->life -= dt;
        float speed = fminf(200.f, sqrtf(m->v[0] * m->v[0] + m->v[1] * m->v[1] + m->v[2] * m->v[2]) + 140.f * dt);
        ent_t *t = m->target >= 0 && g_ent[m->target].on && g_ent[m->target].serial == m->serial ? &g_ent[m->target] : NULL;
        if (t) {
            float d[3] = { t->x - m->p[0], t->y - m->p[1], t->z - m->p[2] };
            float n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
            if (n > 0.01f) {
                d[0] /= n;
                d[1] /= n;
                d[2] /= n;
                steer_vec(m->v, d, speed, 4.f * dt);
            }
        } else {
            float s = sqrtf(m->v[0] * m->v[0] + m->v[1] * m->v[1] + m->v[2] * m->v[2]);
            for (int k = 0; k < 3; k++)
                m->v[k] *= speed / s;
        }
        float p0[3] = { m->p[0], m->p[1], m->p[2] };
        for (int k = 0; k < 3; k++)
            m->p[k] += m->v[k] * dt;
        m->smoke -= dt;
        if (m->smoke <= 0) {
            spawn_part(m->p, frand2(), frand2() + 1.f, frand2(), 0.5f, 0.55f, 0xF0E8E0, 0x908880, -0.5f);
            m->smoke = 0.03f;
        }
        bool hit = false;
        float hw, lat = lat_of(m->p[0], m->p[2], &hw);
        if (m->p[1] < 0 || fabsf(lat) > hw + m->p[1] * LEAN)
            hit = true;
        for (int e = 0; e < MAX_ENT && !hit; e++) {
            ent_t *en = &g_ent[e];
            if (!en->on || en->type == E_FUEL)
                continue;
            float c[3] = { en->x, en->y, en->z };
            if (fabsf(c[0] - m->p[0]) < 30.f && seg_hit(p0, m->p, c, k_erad[en->type] + 1.5f))
                hit = true;
        }
        if (hit || m->life <= 0) {
            boom(m->p, 1.2f);
            /* the blast: 5 to what it hit, 2 to anything within 10 m */
            for (int e = 0; e < MAX_ENT; e++) {
                ent_t *en = &g_ent[e];
                if (!en->on || en->type == E_FUEL)
                    continue;
                float d[3] = { en->x - m->p[0], en->y - m->p[1], en->z - m->p[2] };
                float d2 = d[0] * d[0] + d[1] * d[1] + d[2] * d[2], r = k_erad[en->type] + 2.f;
                if (d2 < r * r)
                    damage_ent(en, 5.f);
                else if (d2 < 100.f)
                    damage_ent(en, 2.f);
            }
            m->life = 0;
        }
    }
}

/* an enemy fires a shell at where the jet will be */
static void enemy_fire(const float from[3], float speed, float dmg, float spread)
{
    const jet_t *j = &g_jet;
    float d[3] = { j->x - from[0], j->y - from[1], j->z - from[2] };
    float t = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) / speed;
    float f[3];
    jet_fwd(f);
    float aim[3] = { j->x + f[0] * j->v * t * 0.85f + frand2() * spread, j->y + f[1] * j->v * t * 0.85f + frand2() * spread,
                     j->z + f[2] * j->v * t * 0.85f + frand2() * spread };
    float a[3] = { aim[0] - from[0], aim[1] - from[1], aim[2] - from[2] };
    float n = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
    if (n < 1.f)
        return;
    for (int i = 0; i < MAX_SHOT; i++) {
        shot_t *s = &g_shot[i];
        if (s->life > 0)
            continue;
        memcpy(s->p, from, sizeof(s->p));
        for (int k = 0; k < 3; k++)
            s->v[k] = a[k] / n * speed;
        s->life = 3.f;
        s->dmg = dmg;
        float dist = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        sfx(320.f, 180.f, 70, WAVE_SQUARE, clampf(0.12f * 120.f / (dist + 40.f), 0.02f, 0.12f));
        return;
    }
}

static void shots_step(float dt)
{
    const jet_t *j = &g_jet;
    float jp[3] = { j->x, j->y, j->z };
    for (int i = 0; i < MAX_SHOT; i++) {
        shot_t *s = &g_shot[i];
        if (s->life <= 0)
            continue;
        float p0[3] = { s->p[0], s->p[1], s->p[2] };
        for (int k = 0; k < 3; k++)
            s->p[k] += s->v[k] * dt;
        s->life -= dt;
        if (s->p[1] < 0)
            s->life = 0;
        if (j->alive && seg_hit(p0, s->p, jp, JET_R)) {
            hurt(s->dmg);
            sparks(s->p[0], s->p[1], s->p[2], 4);
            s->life = 0;
        }
    }
}

/* ── Enemies ─────────────────────────────────────────────────────────────── */
static void ents_step(float dt)
{
    jet_t *j = &g_jet;
    float jp[3] = { j->x, j->y, j->z }, d = diff();
    bool can_fire = j->alive && g_state != ST_OVER;
    for (int i = 0; i < MAX_ENT; i++) {
        ent_t *e = &g_ent[i];
        if (!e->on)
            continue;
        e->t += dt;
        if (e->hit_t > 0)
            e->hit_t -= dt;
        float dx = e->x - j->x;
        bool bound = true;               /* placed by lat (follows the canyon) */
        switch (e->type) {
        case E_TURRET: {
            float up = e->side ? 0 : 1.4f;
            float pv[3] = { e->x, e->y + up, e->side ? z_of(e->x, e->lat - (float)e->side * 1.4f) : e->z };
            float a[3] = { j->x - pv[0], j->y - pv[1], j->z - pv[2] };
            e->aim_h = atan2f(a[2], a[0]);
            e->aim_p = atan2f(a[1], sqrtf(a[0] * a[0] + a[2] * a[2]));
            if (can_fire && dx > 35.f && dx < 330.f) {
                e->fire -= dt;
                if (e->fire <= 0) {
                    float c = cosf(e->aim_p);
                    float mz[3] = { pv[0] + c * cosf(e->aim_h) * 3.2f, pv[1] + sinf(e->aim_p) * 3.2f,
                                    pv[2] + c * sinf(e->aim_h) * 3.2f };
                    enemy_fire(mz, 150.f, 9.f, 2.5f);
                    e->fire = 1.7f - 0.6f * d + frand() * 0.6f;
                }
            }
            break;
        }
        case E_SAM:
            if (can_fire && dx > 60.f && dx < 320.f) {
                e->fire -= dt;
                if (e->fire <= 0) {
                    ent_t *m = spawn_ent(E_EMSL, e->x + 1.f, e->lat, 3.f);
                    if (m) {
                        m->vx = -30.f;
                        m->vy = 45.f;
                        m->vz = 0;
                        sfx(250.f, 700.f, 450, WAVE_NOISE, 0.18f);
                    }
                    e->fire = 5.5f - 1.5f * d;
                }
            }
            break;
        case E_DRONE:
            if (e->mode == 0) {
                e->y = e->by + sinf(e->t * 2.f + e->ph) * 1.5f;
                e->lat = e->blat + sinf(e->t * 1.3f + e->ph) * 5.f;
                if (can_fire && dx > 0 && dx < 140.f) {
                    e->mode = 1;
                    e->z = z_of(e->x, e->lat);
                    float a[3] = { jp[0] - e->x, jp[1] - e->y, jp[2] - e->z };
                    float n = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
                    e->vx = a[0] / n * 45.f;
                    e->vy = a[1] / n * 45.f;
                    e->vz = a[2] / n * 45.f;
                    sfx(1200.f, 600.f, 250, WAVE_SQUARE, 0.06f);
                }
            } else {
                bound = false;
                if (dx > 4.f && j->alive) {
                    float a[3] = { jp[0] - e->x, jp[1] - e->y, jp[2] - e->z };
                    float n = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
                    float v[3] = { e->vx, e->vy, e->vz }, dd[3] = { a[0] / n, a[1] / n, a[2] / n };
                    steer_vec(v, dd, 48.f, 1.6f * dt);
                    e->vx = v[0];
                    e->vy = v[1];
                    e->vz = v[2];
                }
            }
            break;
        case E_HELI:
            e->y = e->by + sinf(e->t * 0.9f + e->ph) * 2.f;
            if (e->mode == 0 && dx < 230.f)
                e->mode = 1;
            if (e->mode >= 1) {
                float lim = e->mode == 2 ? j->v * 1.4f : j->v * 0.8f;
                e->vx += (lim - e->vx) * fminf(1.f, dt * 1.5f);
                e->x += e->vx * dt;
                float jl = lat_of(j->x, j->z, NULL);
                e->lat += clampf(jl - e->lat, -7.f * dt, 7.f * dt) + cosf(e->t * 0.8f + e->ph) * 6.f * dt;
                if (e->t > 14.f)
                    e->mode = 2;
                if (can_fire && e->mode == 1 && dx > 25.f && dx < 250.f) {
                    e->fire -= dt;
                    if (e->fire <= 0) {
                        float mz[3] = { e->x - 2.5f, e->y - 0.5f, z_of(e->x, e->lat) };
                        enemy_fire(mz, 180.f, 7.f, 3.f);
                        if (++e->burst >= 3) {
                            e->burst = 0;
                            e->fire = 1.9f - 0.5f * d;
                        } else
                            e->fire = 0.13f;
                    }
                }
            }
            break;
        case E_JET:
            e->x -= 95.f * dt;
            if (dx > 60.f) {
                e->lat += clampf(lat_of(j->x, j->z, NULL) - e->lat, -4.f * dt, 4.f * dt);
                e->y += clampf(j->y - e->y, -5.f * dt, 5.f * dt);
            }
            if (can_fire && e->mode == 0 && dx > 110.f && dx < 280.f) {
                e->fire -= dt;
                if (e->fire <= 0) {
                    float mz[3] = { e->x - 4.f, e->y, z_of(e->x, e->lat) };
                    enemy_fire(mz, 230.f, 9.f, 2.f);
                    if (++e->burst >= 4) {
                        e->mode = 1;
                    } else
                        e->fire = 0.11f;
                }
            }
            break;
        case E_MINE:
            e->y = e->by + sinf(e->t * 1.5f + e->ph) * 1.2f;
            break;
        case E_BLIMP:
            e->y = e->by + sinf(e->t * 0.6f + e->ph) * 0.6f;
            break;
        case E_FUEL:
            e->y = e->by + sinf(e->t * 1.1f + e->ph) * 1.0f;
            break;
        case E_EMSL: {
            bound = false;
            if (j->alive && e->t > 0.5f) {
                float a[3] = { jp[0] - e->x, jp[1] - e->y, jp[2] - e->z };
                float n = sqrtf(a[0] * a[0] + a[1] * a[1] + a[2] * a[2]);
                float v[3] = { e->vx, e->vy, e->vz }, dd[3] = { a[0] / n, a[1] / n, a[2] / n };
                steer_vec(v, dd, 100.f + 10.f * d, 1.5f * dt);
                e->vx = v[0];
                e->vy = v[1];
                e->vz = v[2];
            }
            e->fire -= dt;
            if (e->fire <= 0) {
                float p[3] = { e->x, e->y, e->z };
                spawn_part(p, frand2(), frand2() + 1.f, frand2(), 0.6f, 0.6f, 0xE8E0D8, 0x807870, -0.5f);
                e->fire = 0.04f;
            }
            float hw, lat = lat_of(e->x, e->z, &hw);
            if (e->t > 6.f || e->y < 0.5f || fabsf(lat) > hw + e->y * LEAN) {
                kill_ent(e, false);
                continue;
            }
            break;
        }
        default:
            break;
        }
        if (bound) {
            e->z = z_of(e->x, e->lat);
        } else {
            e->x += e->vx * dt;
            e->y += e->vy * dt;
            e->z += e->vz * dt;
            if (e->type == E_DRONE && (e->y < 1.f || fabsf(lat_of(e->x, e->z, NULL)) > 60.f)) {
                kill_ent(e, false);
                continue;
            }
        }
        /* gone behind (or the helicopter flew off) */
        if (dx < -70.f && !(e->type == E_EMSL && dx > -250.f)) {
            e->on = 0;
            continue;
        }
        if (e->type == E_HELI && dx > 700.f) {
            e->on = 0;
            continue;
        }
        /* against the jet */
        if (!j->alive)
            continue;
        float q[3] = { e->x - jp[0], e->y - jp[1], e->z - jp[2] };
        float d2 = q[0] * q[0] + q[1] * q[1] + q[2] * q[2];
        float r = k_erad[e->type] + JET_R + (e->type == E_FUEL ? 2.f : 0.f);
        if (d2 < r * r) {
            if (e->type == E_FUEL) {
                if (g_state == ST_PLAY) {
                    j->fuel = fminf(100.f, j->fuel + 22.f);
                    if (j->msl < MSL_MAX)
                        j->msl++;
                    add_score(25);
                    note("FUEL +22  MISSILE +1", 0x07E0);
                }
                sfx(660.f, 990.f, 110, WAVE_TRI, 0.28f);
                sfx_at(990.f, 1480.f, 160, WAVE_TRI, 0.28f, 110);
                e->on = 0;
                continue;
            }
            static const float dmg[NE_TYPES] = { 40.f, 40.f, 20.f, 45.f, 45.f, 30.f, 35.f, 0.f, 25.f };
            hurt(dmg[e->type]);
            kill_ent(e, false);
            continue;
        }
        if (e->type == E_BLIMP && j->y < e->y - 2.f && fabsf(q[0]) < 3.f) {
            /* its cable */
            float dz = q[2];
            if (dx * dx + dz * dz < 1.9f * 1.9f) {
                hurt(30.f);
                sparks(e->x, j->y, e->z, 8);
                kill_ent(e, false);
            }
        }
    }
}

static void parts_step(float dt)
{
    for (int i = 0; i < MAX_PT; i++) {
        part_t *q = &g_pt[i];
        if (q->life <= 0)
            continue;
        q->life -= dt;
        q->v[1] -= q->grav * dt;
        for (int k = 0; k < 3; k++)
            q->p[k] += q->v[k] * dt;
        if (q->p[1] < 0.2f) {
            q->p[1] = 0.2f;
            q->v[1] = fabsf(q->v[1]) * 0.3f;
        }
    }
    for (int i = 0; i < MAX_POP; i++)
        if (g_pop[i].t > 0) {
            g_pop[i].t -= dt;
            g_pop[i].p[1] += 6.f * dt;
        }
}

/* ── The computer flies (the title demo, `auto`) ─────────────────────────── */
static void autopilot(input_t *in)
{
    const jet_t *j = &g_jet;
    float hw, jl = lat_of(j->x, j->z, &hw);
    float tl = 0, ty = 24.f;
    /* a fuel balloon ahead, or else something to shoot */
    int fb = -1;
    float fbd = 360.f;
    for (int i = 0; i < MAX_ENT; i++) {
        const ent_t *e = &g_ent[i];
        float dx = e->x - j->x;
        if (e->on && e->type == E_FUEL && dx > 20.f && dx < fbd) {
            fbd = dx;
            fb = i;
        }
    }
    int lk = g_lock;
    if (fb >= 0) {
        tl = g_ent[fb].lat;
        ty = g_ent[fb].y;
    } else if (lk >= 0 && g_ent[lk].type != E_TURRET) {
        tl = g_ent[lk].lat;
        ty = clampf(g_ent[lk].y, 8.f, 46.f);
    }
    /* keep off cables, mines and spires; go under arches */
    for (int i = 0; i < MAX_ENT; i++) {
        const ent_t *e = &g_ent[i];
        float dx = e->x - j->x;
        if (!e->on || dx < 0 || dx > 160.f)
            continue;
        if (e->type == E_BLIMP && fabsf(tl - e->lat) < 7.f)
            tl = e->lat + (e->lat > 0 ? -9.f : 9.f);
        else if ((e->type == E_MINE || e->type == E_DRONE) && dx < 110.f && fabsf(tl - e->lat) < 6.f && fabsf(ty - e->y) < 6.f)
            ty = e->y + (e->y > 30.f ? -9.f : 9.f);
    }
    for (int i = 0; i < g_npil; i++) {
        float dx = wrapx(g_pil[i].x - j->x);
        if (dx > -5.f && dx < 150.f && ty < g_pil[i].h + 4.f) {
            float pl = lat_of(g_pil[i].x, g_pil[i].z, NULL);
            if (fabsf(tl - pl) < g_pil[i].r + 8.f)
                tl = pl + (pl > 0 ? -1.f : 1.f) * (g_pil[i].r + 9.f);
        }
    }
    for (int i = 0; i < g_narch; i++) {
        float dx = wrapx(g_arch[i].x - j->x);
        if (dx > -10.f && dx < 200.f) {
            tl = 0;
            ty = 20.f;
        }
    }
    float lim = wall_lim(hw, ty) - 3.f;
    tl = clampf(tl, -lim, lim);
    (void)jl;
    float ax = j->x + 90.f, az = z_of(ax, tl);
    float want = atan2f(az - j->z, ax - j->x);
    in->sx = clampf(wrapf(want - j->h) * 2.6f - j->yr * 0.35f, -1.f, 1.f);
    in->sy = clampf((ty - j->y) * 0.09f - j->p * 1.4f, -1.f, 1.f);
    in->dthr = j->thr < 0.55f ? 0.6f : -0.6f;
    in->thr_set = -1;
    if (lk >= 0) {
        const ent_t *e = &g_ent[lk];
        float d[3] = { e->x - j->x, e->y - j->y, e->z - j->z }, f[3];
        jet_fwd(f);
        float n = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        float c = (d[0] * f[0] + d[1] * f[1] + d[2] * f[2]) / n;
        in->gun = n < 280.f && c > 0.993f;
        bool any = false;
        for (int i = 0; i < MAX_MSL; i++)
            any |= g_msl[i].life > 0;
        in->msl = !any && j->msl > 1 && (e->type == E_HELI || e->type == E_JET || e->type == E_SAM) && n < 300.f;
    }
}

/* ── Input ───────────────────────────────────────────────────────────────── */
#define PAUSE_H 26                        /* the pause corner: top of the right bar */
#define TH_Y0   44                        /* the throttle slider in the right bar */
#define TH_Y1   136
#define GUN_Y   142
#define MSLB_Y  190
static uint32_t s_prev_btn;
static int   s_touch_n;
static bool  s_steer_on, s_msl_touch;
static int   s_ax, s_ay, s_fx, s_fy;     /* the touch stick: anchor and finger */
static bool  s_touch_used, s_gun_touch;

static float stick_curve(int raw)
{
    const float dz = 0.14f;
    float v = clampf((float)raw / 30000.f, -1.f, 1.f), m = fabsf(v);
    if (m <= dz)
        return 0;
    float u = (m - dz) / (1.f - dz);
    u = u * (0.35f + 0.65f * u);
    return v < 0 ? -u : u;
}

static input_t read_input(papp_surface_t *surf)
{
    input_t a = { 0 };
    a.thr_set = -1;
    papp_gamepad_t pad = { 0 };
    uint32_t held = papp_input_gamepad(0, &pad) ? pad.buttons : 0;
    uint32_t press = held & ~s_prev_btn;
    s_prev_btn = held;
    if (g_keys && press)
        papp_log("gorgewing: pressed %08lx, held %08lx\n", (unsigned long)press, (unsigned long)held);
    if (press & PAPP_BTN_A)
        a.start = true;
    if (press & (PAPP_BTN_START | PAPP_BTN_SELECT))
        a.pause = true;
    if (held & PAPP_BTN_LEFT)
        a.sx -= 1.f;
    if (held & PAPP_BTN_RIGHT)
        a.sx += 1.f;
    if (held & PAPP_BTN_UP)
        a.sy += 1.f;
    if (held & PAPP_BTN_DOWN)
        a.sy -= 1.f;
    if (a.sx == 0 && (pad.lx > 4000 || pad.lx < -4000))
        a.sx = stick_curve(pad.lx);
    if (a.sy == 0 && (pad.ly > 4000 || pad.ly < -4000))
        a.sy = -stick_curve(pad.ly);
    if (held & (PAPP_BTN_X | PAPP_BTN_R2))
        a.dthr += 0.7f;
    if (held & (PAPP_BTN_Y | PAPP_BTN_L2))
        a.dthr -= 0.7f;
    if (pad.rt > 3000)
        a.dthr += 0.9f * (float)pad.rt / 65535.f;
    if (pad.lt > 3000)
        a.dthr -= 0.9f * (float)pad.lt / 65535.f;
    a.gun = (held & (PAPP_BTN_A | PAPP_BTN_R1)) != 0;
    a.msl = (press & (PAPP_BTN_B | PAPP_BTN_L1)) != 0;

    /* touch: a stick in the view, the throttle slider, GUN and MSL in the right bar, the pause corner */
    papp_touch_point_t tp[3];
    int n = papp_disp_touch(surf, tp, 3);
    bool steer = false, msl_t = false;
    s_gun_touch = false;
    for (int i = 0; i < n && i < 3; i++) {
        int x = tp[i].x, y = tp[i].y;
        s_touch_used = true;
        if (x >= VR && y < PAUSE_H + 6) {
            if (s_touch_n == 0)
                a.pause = true;
        } else if (g_state != ST_PLAY && g_state != ST_DOWN) {
            if (s_touch_n == 0)
                a.tap = true;
        } else if (x >= VR - 4 && y >= TH_Y0 - 10 && y < GUN_Y - 2) {
            a.thr_set = clampf((float)(TH_Y1 - y) / (float)(TH_Y1 - TH_Y0), 0.f, 1.f);
        } else if (x >= VR - 4 && y >= GUN_Y - 2 && y < MSLB_Y - 2) {
            a.gun = true;
            s_gun_touch = true;
        } else if (x >= VR - 4 && y >= MSLB_Y - 2) {
            msl_t = true;
        } else if (x >= VX && !steer) {
            if (!s_steer_on) {
                s_ax = x;
                s_ay = y;
            }
            s_fx = x;
            s_fy = y;
            steer = true;
        }
    }
    if (msl_t && !s_msl_touch)
        a.msl = true;
    s_msl_touch = msl_t;
    s_steer_on = steer;
    if (steer) {
        a.sx = clampf((float)(s_fx - s_ax) / 45.f, -1.f, 1.f);
        a.sy = clampf(-(float)(s_fy - s_ay) / 40.f, -1.f, 1.f);
    }
    s_touch_n = n > 0 ? n : 0;
    if (g_invert)
        a.sy = -a.sy;
    return a;
}

/* ── One step of the game ────────────────────────────────────────────────── */
static void update(input_t *in, float dt)
{
    jet_t *j = &g_jet;
    g_t += dt;
    g_state_t += dt;
    if (g_hurt_t > 0)
        g_hurt_t -= dt;
    if (g_shake > 0)
        g_shake = fmaxf(0, g_shake - dt * 2.5f);
    if (g_msg_t > 0)
        g_msg_t -= dt;

    g_lock = j->alive ? find_lock() : -1;
    if (g_lock >= 0) {
        if (g_lock_t <= 0 && g_state == ST_PLAY)
            sfx(1500.f, 1500.f, 50, WAVE_TRI, 0.08f);
        g_lock_t += dt;
    } else
        g_lock_t = 0;

    if (demo() || g_state == ST_DOWN || g_state == ST_OVER) {
        input_t keep = *in;
        autopilot(in);
        if (g_state == ST_DOWN || g_state == ST_OVER || g_state == ST_TITLE) {
            in->gun = g_state == ST_TITLE && in->gun;
            in->msl = g_state == ST_TITLE && in->msl;
        }
        in->start = keep.start;
        in->pause = keep.pause;
        in->tap = keep.tap;
    }
    float x0 = j->x;
    jet_step(in, dt);
    if (g_state == ST_PLAY) {
        float dd = j->x - x0;
        if (dd > 0)
            g_dist += dd;
        while (g_dist - g_dist_pts >= 10.f) {
            g_dist_pts += 10.f;
            add_score(1);
        }
        int sec = 1 + (int)(g_dist / 2000.f);
        if (sec > g_sector) {
            g_sector = sec;
            add_score(500 * (sec - 1));
            char b[32];
            snprintf(b, sizeof(b), "SECTOR %d  +%d", sec, 500 * (sec - 1));
            note(b, 0x07FF);
            sfx(523.f, 523.f, 120, WAVE_TRI, 0.2f);
            sfx_at(784.f, 784.f, 200, WAVE_TRI, 0.2f, 120);
        }
        if (j->fuel < 20.f && j->fuel > 0) {
            g_alarm_t -= dt;
            if (g_alarm_t <= 0) {
                sfx(880.f, 880.f, 90, WAVE_SQUARE, 0.12f);
                g_alarm_t = j->fuel < 10.f ? 0.5f : 0.9f;
            }
        }
    }
    if ((j->alive && j->fuel > 0) || g_state == ST_TITLE) {
        j->gun_t -= dt;
        if (in->gun && j->gun_t <= 0 && j->alive) {
            fire_gun();
            j->gun_t = 0.075f;
        }
        if (in->msl && j->alive && j->msl > 0)
            fire_missile();
        if (g_state == ST_TITLE && j->msl < 3)
            j->msl = 6;
    }
    weapons_step(dt);
    ents_step(dt);
    shots_step(dt);
    parts_step(dt);
    spawner();
    if (j->x >= LOOP_L)
        shift_all(-LOOP_L);
    else if (j->x < 0)
        shift_all(LOOP_L);

    if (g_state == ST_DOWN && g_state_t > 2.4f) {
        if (g_over_fuel || j->lives <= 0)
            game_over();
        else {
            float x = j->x;
            reset_jet(x);
            j->hull = 100.f;
            j->fuel = fmaxf(j->fuel, 15.f);
            j->invul = 3.f;
            g_state = ST_PLAY;
            g_state_t = 0;
            clear_ahead(160.f);
            note("GO", 0xFFE0);
        }
    }

    /* the camera: behind the jet, its heading following the jet's */
    g_cam_h += wrapf(j->h - g_cam_h) * fminf(1.f, 3.2f * dt);
    g_cam_h = wrapf(g_cam_h);
    g_cam_y += (j->y + 3.8f - g_cam_y) * fminf(1.f, 5.f * dt);

    /* the engine sound */
    float spd = (j->v - V_MIN) / (V_MAX - V_MIN);
    s_eng_f = j->alive && j->fuel > 0 ? 260.f + 520.f * spd + 120.f * j->thr : 120.f;
    s_eng_vol = j->alive && j->fuel > 0 ? 0.10f + 0.12f * j->thr : 0.f;
    s_roar_vol = j->alive ? (j->fuel > 0 ? 0.14f + 0.22f * j->thr : 0.05f) : 0.f;
}

/* ── Drawing ─────────────────────────────────────────────────────────────── */
#define FOCAL     235.f
#define CAM_BACK  16.f
#define CAM_PITCH (-7.f)
#define VIEW_D    440.f
#define NEAR_D    96.f
#define FOG_RGB   0xE2C4A6
#define FOG_DENS  0.0042f

static e3d_camera g_cam;

/* the background: sky with far mesas by heading, haze below the horizon */
#define HILLS 1024
static uint8_t  s_far[HILLS];
static uint16_t s_col[VW];
static uint16_t s_sky[H], s_mesa_c[H];
static uint16_t s_haze;
static int      s_horizon;

static void make_background(void)
{
    s_horizon = (int)(H * 0.5f - FOCAL * tanf(-CAM_PITCH * RAD) + 0.5f);
    for (int y = 0; y < H; y++) {
        float t = clampf((float)y / (float)s_horizon, 0, 1);
        s_sky[y] = rgb565(mix_rgb(0x3468B8, 0xD4D8DC, t * t * t));
        float hz = clampf((float)(s_horizon - y) / 22.f, 0, 1);
        s_mesa_c[y] = rgb565(mix_rgb(mix_rgb(0xB07A62, 0x8A5A48, hz), FOG_RGB, 0.45f));
    }
    s_haze = rgb565(FOG_RGB);
    for (int i = 0; i < HILLS; i++) {
        float a = (float)i / HILLS * PI2;
        float m = sinf(a * 5.f + 0.3f) + 0.5f * sinf(a * 11.f + 1.f) + 0.25f * sinf(a * 23.f);
        float f = m > 0.7f ? 15.f + 2.f * sinf(a * 40.f) : m > 0.5f ? 3.f + (m - 0.5f) * 60.f : 2.f + 1.5f * sinf(a * 13.f);
        s_far[i] = (uint8_t)clampf(f, 1.f, 20.f);
    }
}

static void bg_columns(float heading_deg)
{
    for (int x = 0; x < VW; x++) {
        float a = heading_deg + atanf(((float)x + 0.5f - VW * 0.5f) / FOCAL) * DEG;
        int i = (int)(a * (HILLS / 360.f)) % HILLS;
        if (i < 0)
            i += HILLS;
        s_col[x] = (uint16_t)i;
    }
}

static void sky_row(uint16_t *row, int y, int w, void *ctx)
{
    (void)ctx;
    if (y >= s_horizon) {
        uint16_t c = s_haze;
        for (int x = 0; x < w; x++)
            row[x] = c;
        return;
    }
    int up = s_horizon - y;
    uint16_t sky = s_sky[y];
    if (up > 20) {
        for (int x = 0; x < w; x++)
            row[x] = sky;
        return;
    }
    uint16_t mc = s_mesa_c[y];
    for (int x = 0; x < w; x++)
        row[x] = s_far[s_col[x]] >= up ? mc : sky;
}

static bool project(const float p[3], float *sx, float *sy, float *depth)
{
    float ch = cosf(g_cam.heading * RAD), sh = sinf(g_cam.heading * RAD);
    float cp = cosf(g_cam.pitch * RAD), sp = sinf(g_cam.pitch * RAD);
    float d[3] = { p[0] - g_cam.pos[0], p[1] - g_cam.pos[1], p[2] - g_cam.pos[2] };
    float dep = d[0] * cp * ch + d[1] * sp + d[2] * cp * sh;
    if (dep < 1.f)
        return false;
    float rx = -d[0] * sh + d[2] * ch;
    float up = -d[0] * sp * ch + d[1] * cp - d[2] * sp * sh;
    *sx = (float)VX + VW * 0.5f + FOCAL * rx / dep;
    *sy = H * 0.5f - FOCAL * up / dep;
    *depth = dep;
    return true;
}

static void draw_scene(e3d_t *e)
{
    const jet_t *j = &g_jet;
    float sk = g_shake;
    float ch = cosf(g_cam_h), sh = sinf(g_cam_h);
    g_cam = (e3d_camera){ .pos = { j->x - ch * CAM_BACK + frand2() * sk * 0.15f, g_cam_y + frand2() * sk * 0.15f,
                                   j->z - sh * CAM_BACK },
                          .heading = g_cam_h * DEG, .pitch = CAM_PITCH };
    /* keep the camera inside the canyon */
    {
        float hw, cl = lat_of(g_cam.pos[0], g_cam.pos[2], &hw), lim = hw + g_cam.pos[1] * LEAN - 2.f;
        if (fabsf(cl) > lim)
            g_cam.pos[2] = z_of(g_cam.pos[0], cl > 0 ? lim : -lim);
        if (g_cam.pos[1] < 2.f)
            g_cam.pos[1] = 2.f;
    }
    if (g_icon) {
        /* in front of the jet and to its right, looking back at it down the canyon */
        float hx = cosf(j->h), hz = sinf(j->h);
        g_cam = (e3d_camera){ .pos = { j->x + hx * 10.f - hz * 4.5f, j->y + 1.6f, j->z + hz * 10.f + hx * 4.5f },
                              .pitch = -8.f };
        g_cam.heading = atan2f(j->z - g_cam.pos[2], j->x - g_cam.pos[0]) * DEG;
    }
    *e3d_cam(e) = g_cam;
    bg_columns(g_cam.heading);
    /* the sun: high, ahead and to the left; it moves with the jet so the shadow stays put */
    *e3d_light_get(e) = (e3d_light){ .pos = { j->x + 260.f, 620.f, j->z - 300.f }, .r = 255, .g = 244, .b = 226 };
    if (g_icon)
        *e3d_light_get(e) = (e3d_light){ .pos = { j->x + 300.f, 500.f, j->z + 260.f }, .r = 255, .g = 244, .b = 226 };

    float cx = g_cam.pos[0];
    int vis[NCH], nvis = 0;
    float off[NCH], dist[NCH];
    for (int c = 0; c < NCH; c++) {
        float dx = wrapx(g_ch[c].cx - cx);
        if (dx < (g_icon ? -VIEW_D : -40.f) || dx > VIEW_D)
            continue;
        vis[nvis] = c;
        off[nvis] = cx + dx - g_ch[c].cx;
        dist[nvis] = fabsf(dx);
        nvis++;
    }
    e3d_mat m;
    for (int i = 0; i < nvis; i++) {
        e3d_move(&m, off[i], 0, 0);
        draw_obj(e, g_ch[vis[i]].floor[dist[i] > NEAR_D + 30.f], &m, -1, E3D_FLOOR);
    }

    /* the jet, its flame (longer with the throttle) */
    if (j->alive && !(j->invul > 0 && ((int)(g_t * 12.f) & 1))) {
        m = mat_orient(j->x, j->y, j->z, j->h, j->p, j->roll, 1.f);
        draw_obj(e, m_jet, &m, -1, 0);
        e3d_mat f = m, r;
        e3d_move(&r, -3.95f, 0.f, 0.f);
        e3d_mul(&f, &r);
        float len = j->fuel > 0 ? 0.35f + 0.75f * j->thr + 0.12f * frand() : 0.f;
        if (len > 0) {
            e3d_scale(&r, len, 0.8f + 0.2f * j->thr, 0.8f + 0.2f * j->thr);
            e3d_mul(&f, &r);
            draw_obj(e, m_flame, &f, (int32_t)mix_rgb(0xFF9030, 0xFFF0C0, j->thr), E3D_UNLIT | E3D_NO_SHADOW);
        }
    }

    /* things in the canyon */
    float fx = cosf(g_cam.heading * RAD), fz = sinf(g_cam.heading * RAD);
    for (int i = 0; i < MAX_ENT; i++) {
        const ent_t *en = &g_ent[i];
        if (!en->on)
            continue;
        float dx = en->x - g_cam.pos[0], dz = en->z - g_cam.pos[2];
        float ahead = dx * fx + dz * fz;
        if (ahead < -8.f || ahead > VIEW_D)
            continue;
        bool flash = en->hit_t > 0;
        int32_t col = flash ? 0xFFFFFF : -1;
        uint32_t fl = flash ? E3D_UNLIT : 0;
        uint32_t shadow = ahead < 200.f ? 0 : E3D_NO_SHADOW;
        switch (en->type) {
        case E_TURRET: {
            e3d_mat b;
            float pv[3];
            if (en->side) {
                b = mat_orient(en->x, en->y, en->z, 0, 0, (en->side < 0 ? -90.f : 90.f) * RAD, 1.f);
                pv[0] = en->x;
                pv[1] = en->y;
                pv[2] = z_of(en->x, en->lat - (float)en->side * 1.4f);
            } else {
                b = mat_orient(en->x, 0, en->z, 0, 0, 0, 1.f);
                pv[0] = en->x;
                pv[1] = 1.4f;
                pv[2] = en->z;
            }
            draw_obj(e, m_turret, &b, col, fl | E3D_NO_SHADOW);
            b = mat_orient(pv[0], pv[1], pv[2], en->aim_h, en->aim_p, 0, 1.f);
            draw_obj(e, m_barrel, &b, col, fl | E3D_NO_SHADOW);
            break;
        }
        case E_SAM:
            m = mat_orient(en->x, 0, en->z, PI_F + canyon_dir(en->x), 0, 0, 1.f);
            draw_obj(e, m_sam, &m, col, fl | E3D_NO_SHADOW);
            break;
        case E_DRONE:
            m = mat_orient(en->x, en->y, en->z, PI_F + en->t * 0.7f, 0.1f * sinf(en->t * 3.f), 0.15f * sinf(en->t * 2.3f), 1.f);
            draw_obj(e, m_drone, &m, col, fl | shadow);
            break;
        case E_HELI: {
            float hh = PI_F + canyon_dir(en->x) + 0.25f * sinf(en->t * 0.7f + en->ph);
            m = mat_orient(en->x, en->y, en->z, hh, en->mode ? 0.12f : 0.f, 0.08f * sinf(en->t * 0.8f + en->ph), 1.f);
            draw_obj(e, m_heli, &m, col, fl | shadow);
            e3d_mat r, rm = m;
            e3d_move(&r, 0, 1.65f, 0);
            e3d_mul(&rm, &r);
            e3d_rotate(&r, 0, fmodf(en->t * 1400.f, 360.f), 0);
            e3d_mul(&rm, &r);
            draw_obj(e, m_rotor, &rm, -1, E3D_NO_SHADOW | E3D_DOUBLE_SIDED);
            break;
        }
        case E_JET:
            m = mat_orient(en->x, en->y, en->z, PI_F + canyon_dir(en->x), 0, 0.3f * sinf(en->t * 1.7f), 1.f);
            draw_obj(e, m_ejet, &m, col, fl | shadow);
            break;
        case E_MINE:
            m = mat_orient(en->x, en->y, en->z, en->t * 0.8f, en->t * 0.5f, 0, 1.f);
            draw_obj(e, m_mine, &m, col, fl | E3D_NO_SHADOW);
            break;
        case E_BLIMP: {
            m = mat_orient(en->x, en->y, en->z, PI_F + canyon_dir(en->x), 0, 0, 1.f);
            draw_obj(e, m_blimp, &m, col, fl | shadow);
            e3d_mat cb, r;
            e3d_move(&cb, en->x, 0, en->z);
            e3d_scale(&r, 1.f, en->y - 2.3f, 1.f);
            e3d_mul(&cb, &r);
            draw_obj(e, m_cable, &cb, -1, E3D_UNLIT | E3D_NO_SHADOW);
            break;
        }
        case E_FUEL:
            m = mat_orient(en->x, en->y, en->z, en->t * 0.6f, 0, 0, 1.f);
            draw_obj(e, m_fuel, &m, -1, shadow);
            break;
        case E_EMSL:
            m = mat_orient(en->x, en->y, en->z, atan2f(en->vz, en->vx), atan2f(en->vy, hypotf(en->vx, en->vz)), 0, 1.5f);
            draw_obj(e, m_missile, &m, col, fl | E3D_NO_SHADOW);
            break;
        default:
            break;
        }
    }
    /* the cannon's rounds, the enemy shells, missiles */
    for (int i = 0; i < MAX_BUL; i++) {
        const bullet_t *b = &g_bul[i];
        if (b->life <= 0)
            continue;
        m = mat_orient(b->p[0], b->p[1], b->p[2], atan2f(b->v[2], b->v[0]), atan2f(b->v[1], hypotf(b->v[0], b->v[2])), 0, 1.f);
        draw_obj(e, m_bullet, &m, 0xFFF080, E3D_UNLIT | E3D_NO_SHADOW | E3D_FLAT);
    }
    for (int i = 0; i < MAX_SHOT; i++) {
        const shot_t *s = &g_shot[i];
        if (s->life <= 0)
            continue;
        m = mat_orient(s->p[0], s->p[1], s->p[2], s->life * 9.f, s->life * 7.f, 0, 1.f);
        draw_obj(e, m_shell, &m, ((int)(s->life * 20.f) & 1) ? 0xFF5020 : 0xFFC040, E3D_UNLIT | E3D_NO_SHADOW | E3D_FLAT);
    }
    for (int i = 0; i < MAX_MSL; i++) {
        const missile_t *s = &g_msl[i];
        if (s->life <= 0)
            continue;
        m = mat_orient(s->p[0], s->p[1], s->p[2], atan2f(s->v[2], s->v[0]), atan2f(s->v[1], hypotf(s->v[0], s->v[2])), 0, 1.5f);
        draw_obj(e, m_missile, &m, -1, E3D_NO_SHADOW);
    }
    for (int i = 0; i < MAX_PT; i++) {
        const part_t *q = &g_pt[i];
        if (q->life <= 0)
            continue;
        float dx = q->p[0] - g_cam.pos[0], dz = q->p[2] - g_cam.pos[2];
        float ahead = dx * fx + dz * fz;
        if (ahead < -4.f || ahead > 320.f)
            continue;
        float t = q->life / q->life0;
        float s = q->size * (q->grav < 0 ? 1.7f - 0.7f * t : 0.5f + 0.5f * t);
        m = mat_orient(q->p[0], q->p[1], q->p[2], t * 3.f, t * 2.f, 0, s);
        draw_obj(e, m_part, &m, (int32_t)mix_rgb(q->c1, q->c0, t), E3D_FLAT | E3D_UNLIT | E3D_NO_SHADOW);
    }

    /* the walls last (no shadows of their own) */
    for (int i = 0; i < nvis; i++) {
        e3d_move(&m, off[i], 0, 0);
        draw_obj(e, g_ch[vis[i]].wall[dist[i] > NEAR_D], &m, -1, E3D_NO_SHADOW);
    }
}

/* ── HUD ─────────────────────────────────────────────────────────────────── */
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

/* the HUD draws straight into the frame in PSRAM: clip once per box, then two pixels per
   32-bit access (W is even and the frame 64-byte aligned, so an even x is word aligned) */
typedef uint32_t u32_alias __attribute__((may_alias));
static void fill(uint16_t *px, int x, int y, int w, int h, uint16_t col)
{
    int x1 = x + w, y1 = y + h;
    if (x < 0)
        x = 0;
    if (y < 0)
        y = 0;
    if (x1 > W)
        x1 = W;
    if (y1 > H)
        y1 = H;
    uint32_t c2 = col | (uint32_t)col << 16;
    for (int j = y; j < y1; j++) {
        uint16_t *r = px + j * W;
        int i = x;
        if ((i & 1) && i < x1)
            r[i++] = col;
        for (; i + 1 < x1; i += 2)
            *(u32_alias *)(r + i) = c2;
        if (i < x1)
            r[i] = col;
    }
}

/* keep/8 of each channel, two RGB565 pixels at once */
static inline uint32_t shade2(uint32_t v, int keep)
{
    uint32_t o = 0;
    if (keep & 4)
        o += (v >> 1) & 0x7BEF7BEFu;
    if (keep & 2)
        o += (v >> 2) & 0x39E739E7u;
    if (keep & 1)
        o += (v >> 3) & 0x18E318E3u;
    return o;
}

static void shade_box(uint16_t *px, int x0, int y0, int x1, int y1, int keep)
{
    if (keep >= 8)
        return;
    if (x0 < 0)
        x0 = 0;
    if (y0 < 0)
        y0 = 0;
    if (x1 > W)
        x1 = W;
    if (y1 > H)
        y1 = H;
    for (int y = y0; y < y1; y++) {
        uint16_t *r = px + y * W;
        int x = x0;
        if ((x & 1) && x < x1) {
            r[x] = (uint16_t)shade2(r[x], keep);
            x++;
        }
        for (; x + 1 < x1; x += 2) {
            u32_alias *p = (u32_alias *)(r + x);
            *p = shade2(*p, keep);
        }
        if (x < x1)
            r[x] = (uint16_t)shade2(r[x], keep);
    }
}

static void frame_box(uint16_t *px, int x, int y, int w, int h, uint16_t col)
{
    fill(px, x, y, w, 1, col);
    fill(px, x, y + h - 1, w, 1, col);
    fill(px, x, y, 1, h, col);
    fill(px, x + w - 1, y, 1, h, col);
}

/* a small jet seen from above, nose up (the lives) */
static void jet_icon(uint16_t *px, int x, int y, uint16_t col)
{
    fill(px, x + 4, y, 2, 10, col);
    for (int k = 0; k < 4; k++)
        fill(px, x + 4 - k - 1, y + 4 + k, 4 + 2 * k, 1, col);
    fill(px, x + 2, y + 9, 6, 1, col);
}

/* a vertical meter: v 0..1 fills from the bottom */
static void meter(uint16_t *px, int x, int y0, int y1, float v, uint16_t bar)
{
    shade_box(px, x, y0, x + 32, y1, 4);
    int h = y1 - y0, fh = (int)(clampf(v, 0.f, 1.f) * (float)(h - 4));
    if (fh > 0)
        fill(px, x + 2, y1 - 2 - fh, 28, fh, bar);
    frame_box(px, x, y0, 32, h, 0x8410);
    for (int q = 1; q < 4; q++)
        fill(px, x + 1, y1 - 2 - q * (h - 4) / 4, 5, 1, 0x8410);
}

static void draw_bars(uint16_t *px)
{
    fill(px, 0, 0, VX - 1, H, 0x1083);
    fill(px, VX - 1, 0, 1, H, 0x31A6);
    fill(px, VR, 0, 1, H, 0x31A6);
    fill(px, VR + 1, 0, W - VR - 1, H, 0x1083);
}

static void draw_side(uint16_t *px, const input_t *in)
{
    const jet_t *j = &g_jet;
    char line[16];
    /* left: fuel and missiles */
    bool low = j->fuel < 20.f, blink = ((int)(g_t * 4.f)) & 1;
    text_at(px, 4 + (32 - text_w("FUEL", 1)) / 2, 4, "FUEL", low && blink ? 0xF800 : 0xBDF7, 1);
    uint16_t fc = j->fuel > 50.f ? 0x2E66 : j->fuel > 20.f ? 0xFE60 : 0xF800;
    meter(px, 4, 14, 170, j->fuel / 100.f, fc);
    snprintf(line, sizeof(line), "%d%%", (int)(j->fuel + 0.5f));
    text_at(px, 4 + (32 - text_w(line, 1)) / 2, 174, line, low ? 0xF800 : 0xFFFF, 1);
    text_at(px, 4 + (32 - text_w("MSL", 1)) / 2, 192, "MSL", 0xBDF7, 1);
    snprintf(line, sizeof(line), "%d", j->msl);
    text_at(px, 4 + (32 - text_w(line, 2)) / 2, 203, line, j->msl ? 0xFFFF : 0xF800, 2);
    for (int i = 0; i < j->msl && i < MSL_MAX; i++)
        fill(px, 4 + i * 3 + (32 - MSL_MAX * 3) / 2, 222, 2, 9, 0xC618);
    /* right: the pause corner, throttle, GUN, MSL */
    shade_box(px, VR + 3, 2, W - 3, PAUSE_H, 5);
    frame_box(px, VR + 3, 2, W - VR - 6, PAUSE_H - 2, 0x8410);
    fill(px, VR + 14, 8, 4, 13, 0xC618);
    fill(px, VR + 22, 8, 4, 13, 0xC618);
    text_at(px, VR + 2 + (36 - text_w("THR", 1)) / 2, TH_Y0 - 11, "THR", 0xBDF7, 1);
    int x = VR + 4;
    shade_box(px, x, TH_Y0, x + 32, TH_Y1, 4);
    int h = TH_Y1 - TH_Y0, fh = (int)(j->thr * (float)(h - 4));
    if (fh > 0)
        fill(px, x + 2, TH_Y1 - 2 - fh, 28, fh, j->thr > 0.8f ? 0xFC00 : 0x2C9F);
    frame_box(px, x, TH_Y0, 32, h, 0x8410);
    int hy = TH_Y1 - 2 - fh;
    fill(px, x - 2, hy - 1, 36, 3, 0xFE60);
    struct { int y, h; const char *s; bool on; } b[2] = { { GUN_Y, MSLB_Y - GUN_Y - 4, "GUN", in->gun && j->alive },
                                                         { MSLB_Y, H - 3 - MSLB_Y, "MSL", s_msl_touch } };
    for (int i = 0; i < 2; i++) {
        shade_box(px, VR + 3, b[i].y, W - 3, b[i].y + b[i].h, b[i].on ? 8 : 5);
        frame_box(px, VR + 3, b[i].y, W - VR - 6, b[i].h, b[i].on ? 0xFE60 : 0x8410);
        text_at(px, VR + 2 + (36 - text_w(b[i].s, 1)) / 2, b[i].y + (b[i].h - 7) / 2, b[i].s,
                b[i].on ? 0xFE60 : 0xE71C, 1);
    }
}

static void draw_hud(uint16_t *px, const input_t *in, float fps)
{
    const jet_t *j = &g_jet;
    char line[64];
    draw_side(px, in);
    if (g_state == ST_TITLE) {
        shade_box(px, VX, 34, VR, 196, 4);
        text_c(px, 42, "GORGEWING", 0xFFFF, 5);
        text_c(px, 82, "A JET FIGHTER RUN DOWN THE CANYON", 0xFE60, 1);
        text_c(px, 100, "D-PAD OR STICK STEERS   UP CLIMBS", 0xC618, 1);
        text_c(px, 110, "X MORE THROTTLE   Y LESS THROTTLE", 0xC618, 1);
        text_c(px, 120, "A CANNON   B MISSILE   START PAUSE", 0xC618, 1);
        text_c(px, 130, "FLY INTO GREEN BALLOONS FOR FUEL", 0x07E0, 1);
        text_c(px, 140, "TOUCH: DRAG TO STEER  GUN AND MSL AT RIGHT", 0x9CF3, 1);
        snprintf(line, sizeof(line), "HIGH SCORE %06d   LONGEST %d.%d KM", g_hi, g_best_dist / 1000, (g_best_dist % 1000) / 100);
        text_c(px, 158, line, 0x9FFF, 1);
        if (((int)(g_t * 2.f)) & 1)
            text_c(px, 176, "PRESS A OR TAP TO FLY", 0xFFFF, 1);
        return;
    }
    /* score, high score, lives and the hull */
    shade_box(px, VX, 0, VX + 84, 28, 5);
    text_at(px, VX + 3, 2, "SCORE", 0xBDF7, 1);
    snprintf(line, sizeof(line), "%06d", g_score);
    text_at(px, VX + 3, 11, line, 0xFFFF, 2);
    snprintf(line, sizeof(line), "HI %06d", g_score > g_hi ? g_score : g_hi);
    text_c(px, 3, line, 0x9FFF, 1);
    shade_box(px, VR - 70, 0, VR, 28, 5);
    for (int i = 0; i < j->lives && i < 5; i++)
        jet_icon(px, VR - 14 - i * 13, 3, 0xFFE0);
    int hb = (int)(clampf(j->hull, 0, 100) * 0.64f);
    fill(px, VR - 67, 18, 64, 6, 0x2104);
    fill(px, VR - 67, 18, hb, 6, j->hull > 50 ? 0x2E66 : j->hull > 25 ? 0xFE60 : 0xF800);
    frame_box(px, VR - 68, 17, 66, 8, 0x8410);
    /* speed, altitude, sector */
    shade_box(px, VX, H - 14, VX + 118, H, 5);
    snprintf(line, sizeof(line), "%3d KMH  ALT %2d", (int)(j->v * 3.6f + 0.5f), (int)(j->y + 0.5f));
    text_at(px, VX + 3, H - 11, line, 0xFFFF, 1);
    snprintf(line, sizeof(line), "SECTOR %d", g_sector);
    text_r(px, VR - 3, H - 11, line, 0xBDF7, 1);

    if (j->alive && g_state == ST_PLAY) {
        /* the gunsight, 150 m ahead of the nose */
        float f[3], sx, sy, dep;
        jet_fwd(f);
        float p[3] = { j->x + f[0] * 150.f, j->y + f[1] * 150.f, j->z + f[2] * 150.f };
        if (project(p, &sx, &sy, &dep)) {
            int x = (int)sx, y = (int)sy;
            fill(px, x - 7, y, 5, 1, 0x07E0);
            fill(px, x + 3, y, 5, 1, 0x07E0);
            fill(px, x, y - 7, 1, 5, 0x07E0);
            fill(px, x, y + 3, 1, 5, 0x07E0);
        }
        /* the missile lock */
        if (g_lock >= 0) {
            const ent_t *e = &g_ent[g_lock];
            float q[3] = { e->x, e->y, e->z };
            if (project(q, &sx, &sy, &dep)) {
                int s = (int)clampf(FOCAL * k_erad[e->type] * 1.5f / dep, 6.f, 34.f);
                uint16_t c = j->msl > 0 ? (((int)(g_t * 8.f)) & 1 ? 0xF800 : 0xFE60) : 0x8410;
                if (sx > VX + s && sx < VR - s && sy > s && sy < H - s) {
                    frame_box(px, (int)sx - s, (int)sy - s, 2 * s, 2 * s, c);
                    if (j->msl > 0)
                        text_at(px, (int)sx - 11, (int)sy + s + 2, "LOCK", c, 1);
                }
            }
        }
        /* the touch stick */
        if (s_steer_on) {
            frame_box(px, s_ax - 20, s_ay - 20, 40, 40, 0x8410);
            fill(px, s_fx - 3, s_fy - 3, 7, 7, 0xFE60);
        }
    }
    /* points where things were shot */
    for (int i = 0; i < MAX_POP; i++) {
        float sx, sy, dep;
        if (g_pop[i].t > 0 && project(g_pop[i].p, &sx, &sy, &dep)) {
            snprintf(line, sizeof(line), "+%d", g_pop[i].val);
            int w = text_w(line, 1);
            if (sx - w / 2 > VX && sx + w / 2 < VR && sy > 30 && sy < H - 20)
                text_at(px, (int)sx - w / 2, (int)sy - 12, line, 0xFFE0, 1);
        }
    }
    if (g_hurt_t > 0) {
        fill(px, VX, 0, VW, 3, 0xF800);
        fill(px, VX, H - 3, VW, 3, 0xF800);
        fill(px, VX, 0, 3, H, 0xF800);
        fill(px, VR - 3, 0, 3, H, 0xF800);
    }
    if (g_state == ST_PLAY && j->y > CEIL - 1.f && ((int)(g_t * 3.f) & 1))
        text_c(px, 40, "CEILING", 0xFD20, 1);
    if (g_state == ST_PLAY && j->fuel < 20.f && j->fuel > 0 && g_msg_t <= 0 && ((int)(g_t * 2.f) & 1))
        text_c(px, 60, "LOW FUEL", 0xF800, 2);
    if (g_msg_t > 0)
        text_c(px, 60, g_msg, g_msg_col, 2);
    if (g_state == ST_OVER) {
        shade_box(px, VX + 20, 48, VR - 20, 186, 3);
        text_c(px, 58, g_over_fuel ? "OUT OF FUEL" : "GAME OVER", g_over_fuel ? 0xFD20 : 0xF800, 3);
        snprintf(line, sizeof(line), "SCORE %06d", g_score);
        text_c(px, 92, line, 0xFFFF, 2);
        snprintf(line, sizeof(line), "FLOWN %d.%d KM   SECTOR %d", (int)g_dist / 1000, ((int)g_dist % 1000) / 100, g_sector);
        text_c(px, 116, line, 0xC618, 1);
        snprintf(line, sizeof(line), "HIGH SCORE %06d", g_hi);
        text_c(px, 128, line, 0x9FFF, 1);
        if (g_new_hi && ((int)(g_t * 3.f) & 1))
            text_c(px, 144, "NEW HIGH SCORE", 0x07E0, 2);
        if (g_state_t > 2.f)
            text_c(px, 168, "PRESS A OR TAP", 0xBDF7, 1);
    }
    if (g_show_fps) {
        snprintf(line, sizeof(line), "%.0f FPS", (double)fps);
        text_r(px, VR - 3, 32, line, 0x07E0, 1);
    }
}

/* ── Presenting: a thread scales one frame with the PPA while the next is drawn ── */
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

static e3d_t *g_e;

static int menu(const char *title, const char *const *items, int n)
{
    papp_sem_take(g_pres_done, PAPP_WAIT_FOREVER);   /* nothing is being presented now */
    s_eng_vol = s_roar_vol = 0;
    int r = papp_ui_menu(title, items, n, 0);
    papp_sem_give(g_pres_done);
    papp_input_flush();
    s_prev_btn = ~0u;          /* ignore the buttons still held */
    s_touch_n = 3;
    return r;
}

/* returns false to exit */
static bool pause_menu(void)
{
    enum { M_RESUME, M_RESTART, M_PITCH, M_SOUND, M_HAZE, M_FPS, M_EXIT };
    bool title = g_state == ST_TITLE;
    const char *const items[] = { title ? "Back" : "Resume", title ? "Start a run" : "Restart run",
                                  g_invert ? "Pitch: inverted" : "Pitch: normal", g_sound_on ? "Sound: on" : "Sound: off",
                                  g_haze ? "Haze: on" : "Haze: off", g_show_fps ? "Show fps: on" : "Show fps: off", "Exit" };
    int r = menu("Gorgewing", items, 7);   /* papp_ui_menu shows 7 at most */
    if (r == M_RESTART)
        new_game();
    else if (r == M_PITCH || r == M_SOUND || r == M_HAZE || r == M_FPS) {
        if (r == M_PITCH)
            g_invert = !g_invert;
        else if (r == M_SOUND)
            g_sound_on = !g_sound_on;
        else if (r == M_HAZE) {
            g_haze = !g_haze;
            e3d_params_get(g_e)->fog_density = g_haze ? FOG_DENS : 0.f;
        } else
            g_show_fps = !g_show_fps;
        settings_io(true);
    }
    return r != M_EXIT && !papp_exit_requested();
}

int main(int argc, char **argv)
{
    settings_io(false);
    records_io(false);
    int secs = 0;
    bool nosound = false;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "auto"))
            g_auto = true;
        else if (!strcmp(argv[i], "god"))
            g_god = true;
        else if (!strcmp(argv[i], "fps"))
            g_show_fps = true;
        else if (!strcmp(argv[i], "icon"))
            g_icon = true;
        else if (!strcmp(argv[i], "haze"))
            g_haze = true;
        else if (!strcmp(argv[i], "keys"))
            g_keys = true;
        else if (!strcmp(argv[i], "nosound"))
            nosound = true;
        else if (!strcmp(argv[i], "nohaze"))
            g_haze = false;
        else if (!strcmp(argv[i], "secs") && i + 1 < argc)
            secs = atoi(argv[++i]);
    }
    srand((unsigned)papp_time_us());

    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 1 };
    g_surf = papp_disp_surface_create(&cfg);
    uint16_t *fb[2];
    for (int i = 0; i < 2; i++)
        fb[i] = papp_alloc(sizeof(uint16_t) * W * H, PAPP_MEM_LARGE | PAPP_MEM_DMA, 64);
    g_pres_go = papp_sem_create(0, 1);
    g_pres_done = papp_sem_create(1, 1);
    papp_thread_cfg_t pcfg = { .name = "gw_present", .stack_size = 4096, .prio = PAPP_PRIO_HIGH, .core = 0 };
    papp_thread_t *pres = (g_surf && fb[0] && fb[1] && g_pres_go && g_pres_done)
                              ? papp_thread_create(presenter_main, NULL, &pcfg) : NULL;
    e3d_t *e = e3d_create(VW, H, 2);
    g_e = e;
    int64_t t0 = papp_time_us();
    bool ok = pres && e && b_init();
    ok = ok && make_models() && build_canyon();
    b_free();
    if (!ok) {
        papp_log("gorgewing: out of memory\n");
        if (pres) {
            g_pres_buf = NULL;
            papp_sem_give(g_pres_go);
            papp_thread_join(pres, PAPP_WAIT_FOREVER, NULL);
        }
        free_canyon();
        free_models();
        if (e)
            e3d_destroy(e);
        return 1;
    }
    papp_log("gorgewing: canyon built in %d ms (%d chunks, %d arches, %d spires)\n",
             (int)((papp_time_us() - t0) / 1000), NCH, g_narch, g_npil);
    make_background();
    e3d_set_focal(e, FOCAL);
    e3d_set_background_rows(e, sky_row, NULL);
    e3d_params *par = e3d_params_get(e);
    par->shadows = true;
    par->shadow_y = 0.03f;
    par->shadow_dark = 0.55f;
    par->ambient = 0.40f;
    par->diffuse = 0.85f;
    par->specular = 0.25f;
    par->shininess = 12.f;
    par->fog_density = g_haze ? FOG_DENS : 0.f;
    par->fog_rgb565 = rgb565(FOG_RGB);
    par->fog_keep_bg = true;

    g_snd_lock = papp_mutex_create(false);
    papp_audio_cfg_t acfg = { .rate = SR, .channels = 1, .format = PAPP_AUDIO_S16, .latency_ms = 60 };
    g_snd = g_snd_lock && !nosound ? papp_audio_open(&acfg) : NULL;
    papp_thread_cfg_t acfg_t = { .name = "gw_audio", .stack_size = 4096, .prio = PAPP_PRIO_HIGH, .core = 1 };
    papp_thread_t *aud = g_snd ? papp_thread_create(audio_main, NULL, &acfg_t) : NULL;
    if (!aud && g_snd) {
        papp_audio_close(g_snd);
        g_snd = NULL;
    }

    reset_jet(40.f);
    g_jet.fuel = g_jet.hull = 100.f;
    g_jet.lives = 3;
    g_jet.msl = START_MSL;
    g_cam_h = g_jet.h;
    g_cam_y = g_jet.y + 3.8f;
    g_spawn_x = 300.f;
    g_fuel_x = 340.f;
    g_state = ST_TITLE;
    if (g_auto)
        new_game();
    if (g_icon) {
        g_jet.roll = -0.5f;
        g_jet.p = 0.08f;
        g_jet.thr = 1.f;
    }
    papp_log("gorgewing: running%s%s\n", g_auto ? " (auto)" : "", g_god ? " (god)" : "");

    int cur = 0, frames = 0, g_frame_no = 0;
    int64_t start = papp_time_us(), last = start, stat_t = last, log_t = last, seg_t = last;
    float fps = 0;
    bool run = true;
    /* performance per run: average fps, the worst 1 s, render times */
    int pf_n = 0, pf_run = 0, pf_max = 0;
    double pf_t = 0, pf_geom = 0, pf_rast = 0, pf_upd = 0, pf_scene = 0, pf_e3d = 0, pf_hud = 0, pf_wait = 0;
    int64_t tm_wait = 0;
    float pf_worst = 999.f;
    while (run && !papp_exit_requested()) {
        int64_t now = papp_time_us();
        float dt = (float)(now - last) * 1e-6f, dt_raw = dt;
        last = now;
        if (dt > 0.05f)
            dt = 0.05f;
        if (secs > 0 && now - start > (int64_t)secs * 1000000)
            break;

        input_t in = read_input(g_surf);
        if (in.pause) {
            run = pause_menu();
            last = papp_time_us();
            continue;
        }
        bool go = in.start || in.tap;
        if (g_state == ST_TITLE && go) {
            new_game();
        } else if (g_state == ST_OVER && (g_auto ? g_state_t > 3.f : go && g_state_t > 2.f)) {
            if (g_auto)
                new_game();
            else {
                g_state = ST_TITLE;
                g_state_t = 0;
                reset_jet(g_jet.x);
                g_jet.fuel = g_jet.hull = 100.f;
                g_jet.msl = START_MSL;
            }
        }
        if (g_icon)
            dt = frames < 3 ? 0.02f : 0.f;
        update(&in, dt);

        uint16_t *px = fb[cur];
        int64_t t1 = papp_time_us();
        e3d_begin(e, px + VX, W);
        draw_scene(e);
        int64_t t2 = papp_time_us();
        e3d_end(e);
        int64_t t3 = papp_time_us();
        draw_bars(px);
        if (!g_icon)
            draw_hud(px, &in, fps);
        int64_t t4 = papp_time_us();

        frames++;
        g_frame_no++;
        {
            const e3d_stats *st = e3d_get_stats(e);
            if (st->t_geom_us + st->t_raster_us > 40000)
                papp_log("gorgewing: slow frame %d: geom %d us, raster %d us, objs %d, tris %d/%d, shadow %d, state %d, "
                         "x %.0f, scene %d us\n", g_frame_no, st->t_geom_us, st->t_raster_us, st->objects, st->tris_drawn,
                         st->tris_in, st->shadow_tris, g_state, (double)g_jet.x, (int)(t2 - t1));
        }
        if (now - stat_t >= 1000000) {
            fps = (float)frames * 1e6f / (float)(now - stat_t);
            frames = 0;
            stat_t = now;
            if (g_state == ST_PLAY && pf_n > 30)
                pf_worst = fminf(pf_worst, fps);
        }
        bool playing = g_state == ST_PLAY || g_state == ST_DOWN;
        if (playing) {
            if (pf_n == 0) {
                pf_run = g_runs;
                seg_t = now;
            }
            const e3d_stats *st = e3d_get_stats(e);
            pf_n++;
            pf_t += dt_raw;
            pf_geom += st->t_geom_us;
            pf_rast += st->t_raster_us;
            pf_upd += (double)(t1 - now);
            pf_scene += (double)(t2 - t1);
            pf_e3d += (double)(t3 - t2);
            pf_hud += (double)(t4 - t3);
            pf_wait += (double)tm_wait;
            if (st->t_geom_us + st->t_raster_us > pf_max && g_frame_no > 10)
                pf_max = st->t_geom_us + st->t_raster_us;
        }
        /* the log lines: at the end of each run, and every 60 s of a long one */
        if (pf_n > 100 && (!playing || now - seg_t >= 60000000)) {
            if (pf_worst > 998.f)
                pf_worst = (float)(pf_n / pf_t);
            papp_log("gorgewing: perf run %d%s: %.1f fps average, worst 1 s %.1f, geom %.1f ms + raster %.1f ms average, "
                     "worst frame %.1f ms, %.1f km, score %d\n", pf_run, playing ? " (60 s)" : "", pf_n / pf_t,
                     (double)pf_worst, pf_geom / pf_n / 1000.0, pf_rast / pf_n / 1000.0, pf_max / 1000.0,
                     (double)(g_dist / 1000.f), g_score);
            papp_log("gorgewing: frame run %d: input+update %.2f ms, scene %.2f ms, e3d_end %.2f ms, hud %.2f ms, "
                     "wait for present %.2f ms\n", pf_run, pf_upd / pf_n / 1000.0, pf_scene / pf_n / 1000.0,
                     pf_e3d / pf_n / 1000.0, pf_hud / pf_n / 1000.0, pf_wait / pf_n / 1000.0);
            pf_n = 0;
            pf_t = pf_geom = pf_rast = pf_upd = pf_scene = pf_e3d = pf_hud = pf_wait = 0;
            pf_worst = 999.f;
            pf_max = 0;
        } else if (!playing)
            pf_n = 0;
        if (now - log_t >= 5000000) {
            const e3d_stats *st = e3d_get_stats(e);
            papp_audio_stats_t as = { 0 };
            if (g_snd)
                papp_audio_stats(g_snd, &as);
            int ne = 0;
            for (int i = 0; i < MAX_ENT; i++)
                ne += g_ent[i].on;
            papp_log("gorgewing: %.1f fps, state %d, %.2f km, x %.0f, fuel %.0f, hull %.0f, lives %d, score %d, ents %d, "
                     "objs %d, tris %d/%d, shadow %d, geom %d us, raster %d us, underruns %lu\n",
                     (double)fps, g_state, (double)(g_dist / 1000.f), (double)g_jet.x, (double)g_jet.fuel,
                     (double)g_jet.hull, g_jet.lives, g_score, ne, st->objects, st->tris_drawn, st->tris_in,
                     st->shadow_tris, st->t_geom_us, st->t_raster_us, (unsigned long)as.underruns);
            log_t = now;
        }
        int64_t t5 = papp_time_us();
        papp_sem_take(g_pres_done, PAPP_WAIT_FOREVER);
        tm_wait = papp_time_us() - t5;
        g_pres_buf = px;
        papp_sem_give(g_pres_go);
        cur ^= 1;
    }
    if (pf_worst > 998.f && pf_n > 0)
        pf_worst = (float)(pf_n / pf_t);
    if (pf_n > 100)
        papp_log("gorgewing: perf run %d (to exit): %.1f fps average, worst 1 s %.1f, geom %.1f ms + raster %.1f ms average, "
                 "worst frame %.1f ms, %.1f km, score %d\n", pf_run, pf_n / pf_t, (double)pf_worst,
                 pf_geom / pf_n / 1000.0, pf_rast / pf_n / 1000.0, pf_max / 1000.0, (double)(g_dist / 1000.f), g_score);
    if (pf_n > 100)
        papp_log("gorgewing: frame run %d: input+update %.2f ms, scene %.2f ms, e3d_end %.2f ms, hud %.2f ms, "
                 "wait for present %.2f ms\n", pf_run, pf_upd / pf_n / 1000.0, pf_scene / pf_n / 1000.0,
                 pf_e3d / pf_n / 1000.0, pf_hud / pf_n / 1000.0, pf_wait / pf_n / 1000.0);

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
    free_canyon();
    free_models();
    papp_sem_destroy(g_pres_go);
    papp_sem_destroy(g_pres_done);
    papp_free(fb[0]);
    papp_free(fb[1]);
    papp_disp_surface_destroy(g_surf);
    return 0;
}
