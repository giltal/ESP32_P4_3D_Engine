/*
 * Driftrock — rocks in space, in 3D, on the e3d engine.
 *
 * A field of tumbling, lit rocks drifts over a starfield, seen from above at an
 * angle. Your ship turns, thrusts and drifts on its momentum; everything wraps
 * from one edge of the field to the other (and is drawn on both sides while it
 * crosses). A shot breaks a big rock into two medium ones, a medium one into two
 * small ones, and a small one into dust. Saucers come by now and then: the big
 * one fires anywhere, the small one aims. Hyperspace takes you somewhere else on
 * the field, at the risk of landing on a rock.
 *
 * Scoring: big rock 20, medium 50, small 100, big saucer 200, small saucer 1000.
 * An extra ship every 10000. Each wave brings one more big rock (up to 11), and
 * they get faster; the saucers come sooner and aim better.
 *
 * Save: the pause menu's "Save game" keeps the wave, score, lives and the rocks
 * in /sd/data/driftrock/save.txt; "Load game" (pause menu) and "Continue" (title
 * screen) bring it back. One slot; it stays after a game over.
 *
 * Pad: left/right (or the stick) turn, up, A or R1 thrusts, B or X fires (hold
 * for a stream), down, Y or L1 jumps to hyperspace, START or SELECT pauses.
 * Touch: touch the field and the ship turns to that point and fires; the THRUST
 * and HYPER buttons at the bottom left; the top-right corner pauses.
 *
 * Arguments: `auto` = the computer plays (attract mode, for testing),
 *            `wave N` = start at wave N, `icon` = a still close-up for the icon,
 *            `saucer` = a saucer at once (for testing).
 */
#include "papp/papp.h"
#include "e3d.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

PAPP_MANIFEST(.id = "giltal.driftrock", .name = "Driftrock", .version = PAPP_APP_VERSION, .main_stack = 16384);

#define W 400
#define H 240

/* ── World (x = depth away from the camera = up on the screen, z = right, y = up)
 * Everything moves in the plane y = 0; the field wraps at its edges. */
#define FX0   (-176.f)
#define FX1    192.f
#define FZH    280.f
#define FLX   (FX1 - FX0)
#define FLZ   (2.f * FZH)
#define FXC   ((FX0 + FX1) * 0.5f)

#define SHIP_SC    1.45f
#define SHIP_R     11.f
#define TURN_DPS   250.f       /* degrees per second */
#define THRUST     250.f       /* units/s^2 */
#define MAX_SPEED  290.f
#define DRAG       0.42f       /* per second (exponential) */
#define SHOT_SPEED 430.f
#define SHOT_LIFE  0.82f
#define MAX_PSHOTS 4
#define MAX_USHOTS 6
#define MAX_ROCKS  64
#define MAX_PARTS  150
#define MAX_BOOMS  14
#define MAX_POPS   6
#define NSHAPES    6
#define LIFE_EVERY 10000

static const float rock_r[3]   = { 30.f, 16.f, 8.5f };
static const int   rock_pts[3] = { 20, 50, 100 };

/* Camera: fixed, above the near edge, looking down at the field */
#define CAM_X     (-296.f)
#define CAM_Y      634.f
#define CAM_PITCH (-65.f)
#define FOCAL      425.f

/* ── Sound: a few synthesised voices mixed on an audio thread (as Shardwall) ── */
#define SR 22050
enum { WAVE_SQUARE, WAVE_TRI, WAVE_NOISE };

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
    while (!g_snd_quit && !papp_exit_requested()) {
        papp_mutex_lock(g_snd_lock);
        for (int i = 0; i < 256; i++) {
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
                if (v->wave == WAVE_SQUARE)
                    s = v->ph < 0.5f ? 1.f : -1.f;
                else if (v->wave == WAVE_TRI)
                    s = v->ph < 0.5f ? 4.f * v->ph - 1.f : 3.f - 4.f * v->ph;
                else {
                    g_noise = g_noise * 1664525u + 1013904223u;
                    s = (float)(int32_t)g_noise * (1.f / 2147483648.f);
                }
                acc += s * v->vol * env;
                v->ph += v->f * (1.f / SR);
                if (v->ph >= 1.f)
                    v->ph -= 1.f;
                v->f *= v->fmul;
                v->left--;
            }
            if (acc > 1.f) acc = 1.f;
            if (acc < -1.f) acc = -1.f;
            buf[i] = (int16_t)(acc * 26000.f);
        }
        papp_mutex_unlock(g_snd_lock);
        papp_audio_write(g_snd, buf, 256, 100);
    }
    return 0;
}

/* ── Meshes ──────────────────────────────────────────────────────────────── */
static float frand(void) { return (float)rand() / (float)RAND_MAX; }

static void tri_out(e3d_mesh *m, int *nt, int a, int b, int c, const float ctr[3])
{
    /* outward normal = (v0 - v1) x (v2 - v1); flip the corners if it points inwards */
    float *p0 = m->pos[a], *p1 = m->pos[b], *p2 = m->pos[c];
    float u[3] = { p0[0] - p1[0], p0[1] - p1[1], p0[2] - p1[2] };
    float v[3] = { p2[0] - p1[0], p2[1] - p1[1], p2[2] - p1[2] };
    float n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
    float d[3] = { (p0[0] + p1[0] + p2[0]) / 3 - ctr[0], (p0[1] + p1[1] + p2[1]) / 3 - ctr[1],
                   (p0[2] + p1[2] + p2[2]) / 3 - ctr[2] };
    e3d_tri *t = &m->tris[(*nt)++];
    if (n[0] * d[0] + n[1] * d[1] + n[2] * d[2] < 0) {
        int s = a;
        a = c;
        c = s;
    }
    t->v[0] = (uint16_t)a;
    t->v[1] = (uint16_t)b;
    t->v[2] = (uint16_t)c;
    t->r = t->g = t->b = 255;
}

static void quad_out(e3d_mesh *m, int *nt, int a, int b, int c, int d, const float ctr[3])
{
    tri_out(m, nt, a, b, c, ctr);
    tri_out(m, nt, a, c, d, ctr);
}

/* Several parts, each placed by a matrix, as one mesh (fewer objects to draw). */
typedef struct { e3d_mesh *mesh; e3d_mat m; } part_t;

static e3d_mesh *merge(part_t *parts, int n, float crease)
{
    int nv = 0, nt = 0;
    bool ok = true;
    for (int i = 0; i < n; i++) {
        if (!parts[i].mesh) {
            ok = false;
            continue;
        }
        nv += parts[i].mesh->nverts;
        nt += parts[i].mesh->ntris;
    }
    e3d_mesh *m = ok ? e3d_mesh_create(nv, nt) : NULL;
    if (m) {
        int v0 = 0, t0 = 0;
        for (int i = 0; i < n; i++) {
            const e3d_mesh *p = parts[i].mesh;
            for (int v = 0; v < p->nverts; v++) {
                float q[3] = { p->pos[v][0], p->pos[v][1], p->pos[v][2] };
                e3d_apply(&parts[i].m, q);
                memcpy(m->pos[v0 + v], q, sizeof(q));
            }
            for (int t = 0; t < p->ntris; t++) {
                e3d_tri *d = &m->tris[t0 + t];
                *d = p->tris[t];
                for (int k = 0; k < 3; k++)
                    d->v[k] = (uint16_t)(p->tris[t].v[k] + v0);
            }
            v0 += p->nverts;
            t0 += p->ntris;
        }
        e3d_mesh_finish(m, crease);
    }
    for (int i = 0; i < n; i++)
        e3d_mesh_free(parts[i].mesh);
    return m;
}

/* a closed box centred on the origin */
static e3d_mesh *box(float hx, float hy, float hz)
{
    e3d_mesh *m = e3d_mesh_create(8, 12);
    if (!m)
        return NULL;
    for (int i = 0; i < 8; i++) {
        m->pos[i][0] = (i & 1) ? hx : -hx;
        m->pos[i][1] = (i & 2) ? hy : -hy;
        m->pos[i][2] = (i & 4) ? hz : -hz;
    }
    float c[3] = { 0, 0, 0 };
    int nt = 0;
    quad_out(m, &nt, 0, 1, 3, 2, c);
    quad_out(m, &nt, 4, 5, 7, 6, c);
    quad_out(m, &nt, 0, 1, 5, 4, c);
    quad_out(m, &nt, 2, 3, 7, 6, c);
    quad_out(m, &nt, 0, 2, 6, 4, c);
    quad_out(m, &nt, 1, 3, 7, 5, c);
    e3d_mesh_finish(m, 1.f);
    return m;
}

/* octahedron: rx, ry, rz half sizes */
static e3d_mesh *octa(float rx, float ry, float rz)
{
    e3d_mesh *m = e3d_mesh_create(6, 8);
    if (!m)
        return NULL;
    static const float p[6][3] = { { 1, 0, 0 }, { -1, 0, 0 }, { 0, 1, 0 }, { 0, -1, 0 }, { 0, 0, 1 }, { 0, 0, -1 } };
    for (int i = 0; i < 6; i++) {
        m->pos[i][0] = p[i][0] * rx;
        m->pos[i][1] = p[i][1] * ry;
        m->pos[i][2] = p[i][2] * rz;
    }
    float c[3] = { 0, 0, 0 };
    int nt = 0;
    for (int a = 0; a < 2; a++)
        for (int b = 2; b < 4; b++)
            for (int d = 4; d < 6; d++)
                tri_out(m, &nt, a, b, d, c);
    e3d_mesh_finish(m, 1.f);
    return m;
}

static e3d_mesh *tetra(float s)
{
    e3d_mesh *m = e3d_mesh_create(4, 4);
    if (!m)
        return NULL;
    static const float p[4][3] = { { 1, 1, 1 }, { 1, -1, -1 }, { -1, 1, -1 }, { -1, -1, 1 } };
    for (int i = 0; i < 4; i++)
        for (int k = 0; k < 3; k++)
            m->pos[i][k] = p[i][k] * s;
    float ctr[3] = { 0, 0, 0 };
    int nt = 0;
    tri_out(m, &nt, 0, 1, 2, ctr);
    tri_out(m, &nt, 0, 1, 3, ctr);
    tri_out(m, &nt, 0, 2, 3, ctr);
    tri_out(m, &nt, 1, 2, 3, ctr);
    e3d_mesh_finish(m, 1.f);
    return m;
}

/* a cone along -x: base ring of radius 1 at x = 0, tip at x = -1 */
static e3d_mesh *cone(int seg)
{
    e3d_mesh *m = e3d_mesh_create(seg + 2, seg * 2);
    if (!m)
        return NULL;
    for (int i = 0; i < seg; i++) {
        float a = 6.2832f * (float)i / (float)seg;
        m->pos[i][0] = 0;
        m->pos[i][1] = cosf(a);
        m->pos[i][2] = sinf(a);
    }
    m->pos[seg][0] = -1.f;          /* tip */
    m->pos[seg][1] = m->pos[seg][2] = 0;
    m->pos[seg + 1][0] = 0.f;       /* base centre */
    m->pos[seg + 1][1] = m->pos[seg + 1][2] = 0;
    float c[3] = { -0.3f, 0, 0 };
    int nt = 0;
    for (int i = 0; i < seg; i++) {
        int j = (i + 1) % seg;
        tri_out(m, &nt, i, j, seg, c);
        tri_out(m, &nt, j, i, seg + 1, c);
    }
    e3d_mesh_finish(m, 60.f);
    return m;
}

/* the ship's hull: an arrowhead pointing along +x (Voidmarch's) */
static e3d_mesh *hull(void)
{
    e3d_mesh *m = e3d_mesh_create(6, 8);
    if (!m)
        return NULL;
    static const float p[6][3] = {
        { 13, 4, 0 },     /* nose */
        { -7, 3, -11 },   /* left */
        { -7, 3, 11 },    /* right */
        { -5, 9, 0 },     /* top */
        { -7, 0, 0 },     /* bottom */
        { -9, 4, 0 },     /* tail */
    };
    memcpy(m->pos, p, sizeof(p));
    float c[3] = { -3, 4, 0 };
    int nt = 0;
    tri_out(m, &nt, 0, 1, 3, c);
    tri_out(m, &nt, 0, 3, 2, c);
    tri_out(m, &nt, 0, 2, 4, c);
    tri_out(m, &nt, 0, 4, 1, c);
    tri_out(m, &nt, 5, 1, 3, c);
    tri_out(m, &nt, 5, 3, 2, c);
    tri_out(m, &nt, 5, 2, 4, c);
    tri_out(m, &nt, 5, 4, 1, c);
    e3d_mesh_finish(m, 1.f);
    return m;
}

/* A flat ring in the xz plane (a planet's), both sides */
static e3d_mesh *ring(float r0, float r1, int seg, uint32_t rgb)
{
    e3d_mesh *m = e3d_mesh_create(seg * 2, seg * 2);
    if (!m)
        return NULL;
    for (int i = 0; i < seg; i++) {
        float a = 6.2832f * (float)i / (float)seg;
        m->pos[2 * i][0] = cosf(a) * r0;
        m->pos[2 * i][1] = 0;
        m->pos[2 * i][2] = sinf(a) * r0;
        m->pos[2 * i + 1][0] = cosf(a) * r1;
        m->pos[2 * i + 1][1] = 0;
        m->pos[2 * i + 1][2] = sinf(a) * r1;
    }
    float c[3] = { 0, -1.f, 0 };    /* normals up */
    int nt = 0;
    for (int i = 0; i < seg; i++) {
        int j = (i + 1) % seg;
        quad_out(m, &nt, 2 * i, 2 * i + 1, 2 * j + 1, 2 * j, c);
        float k = (i & 1) ? 1.f : 0.82f;    /* bands */
        for (int q = nt - 2; q < nt; q++) {
            m->tris[q].r = (uint8_t)((float)(rgb >> 16) * k);
            m->tris[q].g = (uint8_t)((float)((rgb >> 8) & 255) * k);
            m->tris[q].b = (uint8_t)((float)(rgb & 255) * k);
        }
    }
    e3d_mesh_finish(m, 1.f);
    return m;
}

/* A rock: an icosahedron (subdivided once for the bigger ones) with every
 * corner pushed in or out, and a few broad dents and bumps. Each triangle gets
 * a slightly different shade of the rock's colour. */
static e3d_mesh *rock(int subdiv, uint32_t seed, uint32_t rgb, float rough)
{
    static const int ico_f[20][3] = {
        { 0, 11, 5 }, { 0, 5, 1 }, { 0, 1, 7 }, { 0, 7, 10 }, { 0, 10, 11 }, { 1, 5, 9 }, { 5, 11, 4 },
        { 11, 10, 2 }, { 10, 7, 6 }, { 7, 1, 8 }, { 3, 9, 4 }, { 3, 4, 2 }, { 3, 2, 6 }, { 3, 6, 8 },
        { 3, 8, 9 }, { 4, 9, 5 }, { 2, 4, 11 }, { 6, 2, 10 }, { 8, 6, 7 }, { 9, 8, 1 } };
    const float t = 1.618034f;
    float vp[42][3] = {
        { -1, t, 0 }, { 1, t, 0 }, { -1, -t, 0 }, { 1, -t, 0 }, { 0, -1, t }, { 0, 1, t },
        { 0, -1, -t }, { 0, 1, -t }, { t, 0, -1 }, { t, 0, 1 }, { -t, 0, -1 }, { -t, 0, 1 } };
    int f[80][3], nf = 20, nv = 12;
    memcpy(f, ico_f, sizeof(ico_f));
    if (subdiv) {
        int ea[30], eb[30], em[30], ne = 0, nf2 = 0;
        int f2[80][3];
        for (int i = 0; i < 20; i++) {
            int mid[3];
            for (int k = 0; k < 3; k++) {
                int a = f[i][k], b = f[i][(k + 1) % 3], e = -1;
                for (int q = 0; q < ne; q++)
                    if ((ea[q] == a && eb[q] == b) || (ea[q] == b && eb[q] == a))
                        e = q;
                if (e < 0) {
                    for (int c = 0; c < 3; c++)
                        vp[nv][c] = (vp[a][c] + vp[b][c]) * 0.5f;
                    ea[ne] = a;
                    eb[ne] = b;
                    em[ne] = nv++;
                    e = ne++;
                }
                mid[k] = em[e];
            }
            int a = f[i][0], b = f[i][1], c = f[i][2];
            int nn[4][3] = { { a, mid[0], mid[2] }, { mid[0], b, mid[1] }, { mid[2], mid[1], c }, { mid[0], mid[1], mid[2] } };
            memcpy(&f2[nf2], nn, sizeof(nn));
            nf2 += 4;
        }
        memcpy(f, f2, sizeof(f2));
        nf = nf2;
    }
    uint32_t s = seed * 2654435761u + 12345u;
#define RND() ((s = s * 1664525u + 1013904223u), (float)(s >> 8) * (1.f / 16777216.f))
    float bump[4][4];               /* direction + strength of broad bumps (negative = dents) */
    for (int b = 0; b < 4; b++) {
        float d[3] = { RND() - 0.5f, RND() - 0.5f, RND() - 0.5f };
        float l = sqrtf(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]) + 1e-4f;
        for (int k = 0; k < 3; k++)
            bump[b][k] = d[k] / l;
        bump[b][3] = (RND() - 0.6f) * 0.55f;
    }
    float stretch[3] = { 0.85f + RND() * 0.3f, 0.78f + RND() * 0.25f, 0.85f + RND() * 0.3f };
    e3d_mesh *m = e3d_mesh_create(nv, nf);
    if (!m)
        return NULL;
    for (int i = 0; i < nv; i++) {
        float l = sqrtf(vp[i][0] * vp[i][0] + vp[i][1] * vp[i][1] + vp[i][2] * vp[i][2]);
        float n[3] = { vp[i][0] / l, vp[i][1] / l, vp[i][2] / l };
        float r = 1.f + (RND() - 0.5f) * rough;
        for (int b = 0; b < 4; b++) {
            float d = n[0] * bump[b][0] + n[1] * bump[b][1] + n[2] * bump[b][2];
            if (d > 0.4f)
                r += bump[b][3] * (d - 0.4f) / 0.6f;
        }
        for (int k = 0; k < 3; k++)
            m->pos[i][k] = n[k] * r * stretch[k];
    }
    float ctr[3] = { 0, 0, 0 };
    int nt = 0;
    for (int i = 0; i < nf; i++) {
        tri_out(m, &nt, f[i][0], f[i][1], f[i][2], ctr);
        float k = 0.8f + RND() * 0.3f;
        m->tris[nt - 1].r = (uint8_t)fminf(255.f, (float)(rgb >> 16) * k);
        m->tris[nt - 1].g = (uint8_t)fminf(255.f, (float)((rgb >> 8) & 255) * k);
        m->tris[nt - 1].b = (uint8_t)fminf(255.f, (float)(rgb & 255) * k);
    }
#undef RND
    e3d_mesh_finish(m, 34.f);
    return m;
}

static e3d_mat mat_at(float x, float y, float z, float rx, float ry, float rz, float sx, float sy, float sz)
{
    e3d_mat m, r;
    e3d_move(&m, x, y, z);
    e3d_rotate(&r, rx, ry, rz);
    e3d_mul(&m, &r);
    e3d_scale(&r, sx, sy, sz);
    e3d_mul(&m, &r);
    return m;
}

/* ── Game state ──────────────────────────────────────────────────────────── */
typedef struct {
    float x, z, vx, vz;
    float rot[3], spin[3];
    uint8_t size, shape;
    bool alive;
} rock_t;

typedef struct { float x, z, vx, vz, life; bool alive; } shot_t;
typedef struct { float p[3], v[3], rot[3], spin[3], life, life0, size; uint32_t rgb; bool hot; } spark_t;
typedef struct { float p[3], t, dur, size; uint32_t rgb; } boom_t;
typedef struct { float p[3], t; int pts; } pop_t;
typedef struct {
    bool  alive, small;
    float x, z, vx, vz, turn_t, fire_t, wob, age;
} ufo_t;

enum { ST_TITLE, ST_PLAY, ST_DEAD, ST_CLEAR, ST_OVER };

static rock_t  g_rock[MAX_ROCKS];
static shot_t  g_ps[MAX_PSHOTS], g_us[MAX_USHOTS];
static spark_t g_part[MAX_PARTS];
static boom_t  g_boom[MAX_BOOMS];
static pop_t   g_pop[MAX_POPS];
static ufo_t   g_ufo;
static float   g_ufo_t;
static int     g_state, g_wave, g_lives, g_score, g_hiscore, g_next_life, g_rocks_left;
static float   g_state_t, g_t, g_shake, g_flash, g_wave_t;
/* the ship */
static float   g_sx, g_sz, g_svx, g_svz, g_head, g_roll;
static bool    g_ship_on, g_thrusting;
static float   g_inv_t;                 /* > 0: invulnerable after a respawn (blinks) */
static float   g_hyper_t;               /* > 0: in hyperspace */
static float   g_fire_cd, g_trail_t, g_thrust_snd, g_beat_t;
static int     g_beat_n;
static bool    g_auto, g_icon, g_saucer_test;

static uint32_t mix_rgb(uint32_t a, uint32_t b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int r = (int)((float)(a >> 16) + ((float)(b >> 16) - (float)(a >> 16)) * t);
    int g = (int)((float)((a >> 8) & 255) + ((float)((b >> 8) & 255) - (float)((a >> 8) & 255)) * t);
    int bl = (int)((float)(a & 255) + ((float)(b & 255) - (float)(a & 255)) * t);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

/* rock colours, one per shape */
static const uint32_t rock_rgb[NSHAPES] = { 0x9C8C7C, 0x8A93A6, 0xB07850, 0x7C8C70, 0xC0A888, 0x8C7468 };

static float wrapf(float v, float lo, float len)
{
    while (v < lo) v += len;
    while (v >= lo + len) v -= len;
    return v;
}
/* the shortest way from a to b across the wrapping field */
static float dwx(float a, float b) { float d = b - a; return d > FLX * 0.5f ? d - FLX : d < -FLX * 0.5f ? d + FLX : d; }
static float dwz(float a, float b) { float d = b - a; return d > FZH ? d - FLZ : d < -FZH ? d + FLZ : d; }
static float dist2w(float x0, float z0, float x1, float z1)
{
    float dx = dwx(x0, x1), dz = dwz(z0, z1);
    return dx * dx + dz * dz;
}

/* ── Effects ─────────────────────────────────────────────────────────────── */
static void spawn_parts(float x, float y, float z, float vx, float vz, uint32_t rgb, int n, float speed, bool hot)
{
    for (int k = 0, i = 0; k < MAX_PARTS && i < n; k++) {
        spark_t *s = &g_part[k];
        if (s->life > 0)
            continue;
        s->p[0] = x + (frand() - 0.5f) * 6.f;
        s->p[1] = y + (frand() - 0.5f) * 6.f;
        s->p[2] = z + (frand() - 0.5f) * 6.f;
        float a = frand() * 6.2832f, e = (frand() - 0.5f) * 1.6f;
        float sp = speed * (0.3f + frand());
        s->v[0] = cosf(a) * cosf(e) * sp + vx * 0.5f;
        s->v[1] = sinf(e) * sp;
        s->v[2] = sinf(a) * cosf(e) * sp + vz * 0.5f;
        for (int q = 0; q < 3; q++) {
            s->rot[q] = frand() * 360.f;
            s->spin[q] = (frand() - 0.5f) * 900.f;
        }
        s->life0 = s->life = hot ? 0.35f + frand() * 0.5f : 0.7f + frand() * 0.9f;
        s->size = 0.6f + frand() * 0.8f;
        s->rgb = rgb;
        s->hot = hot;
        i++;
    }
}

static void boom(float x, float y, float z, float size, float dur, uint32_t rgb)
{
    int best = 0;
    for (int i = 0; i < MAX_BOOMS; i++) {
        if (g_boom[i].t >= g_boom[i].dur) {
            best = i;
            break;
        }
        if (g_boom[i].t / g_boom[i].dur > g_boom[best].t / g_boom[best].dur)
            best = i;
    }
    g_boom[best] = (boom_t){ { x, y, z }, 0, dur, size, rgb };
}

static void pop(float x, float y, float z, int pts)
{
    for (int i = 0; i < MAX_POPS; i++)
        if (g_pop[i].t <= 0) {
            g_pop[i] = (pop_t){ { x, y, z }, 1.2f, pts };
            return;
        }
}

static void add_score(int pts)
{
    if (g_state != ST_PLAY && g_state != ST_DEAD && g_state != ST_CLEAR)
        return;
    g_score += pts;
    if (g_score >= g_next_life) {
        g_next_life += LIFE_EVERY;
        if (g_lives < 9) {
            g_lives++;
            for (int i = 0; i < 4; i++)
                sfx_at(880.f * (1 + 0.25f * (float)i), 880.f * (1 + 0.25f * (float)i), 70, WAVE_SQUARE, 0.18f, i * 70);
        }
    }
    if (g_score > g_hiscore)
        g_hiscore = g_score;
}

static void hiscore_io(bool save)
{
    if (save && g_auto)
        return;                          /* the computer's games do not count */
    const char *path = "/sd/data/driftrock/hiscore.txt";
    if (save) {
        mkdir("/sd/data", 0777);
        mkdir("/sd/data/driftrock", 0777);
        FILE *f = fopen(path, "w");
        if (f) {
            fprintf(f, "%d\n", g_hiscore);
            fclose(f);
        }
    } else {
        FILE *f = fopen(path, "r");
        if (f) {
            if (fscanf(f, "%d", &g_hiscore) != 1)
                g_hiscore = 0;
            fclose(f);
        }
    }
}

/* ── Rocks ───────────────────────────────────────────────────────────────── */
static rock_t *add_rock(int size, float x, float z, float vx, float vz, int shape)
{
    for (int i = 0; i < MAX_ROCKS; i++) {
        rock_t *r = &g_rock[i];
        if (r->alive)
            continue;
        memset(r, 0, sizeof(*r));
        r->alive = true;
        r->size = (uint8_t)size;
        r->shape = (uint8_t)(shape < 0 ? (int)(frand() * NSHAPES) % NSHAPES : shape % NSHAPES);
        r->x = wrapf(x, FX0, FLX);
        r->z = wrapf(z, -FZH, FLZ);
        r->vx = vx;
        r->vz = vz;
        float sp = 40.f + 30.f * (float)size;
        for (int k = 0; k < 3; k++) {
            r->rot[k] = frand() * 360.f;
            r->spin[k] = (frand() - 0.5f) * 2.f * sp;
        }
        g_rocks_left++;
        return r;
    }
    return NULL;
}

static float rock_speed(int size)
{
    float w = (float)(g_wave < 12 ? g_wave : 12);
    static const float base[3] = { 24.f, 42.f, 64.f };
    return (base[size] + 2.5f * w * (1.f + 0.4f * (float)size)) * (0.8f + 0.5f * frand());
}

static void load_wave(int w)
{
    memset(g_rock, 0, sizeof(g_rock));
    g_rocks_left = 0;
    for (int i = 0; i < MAX_PSHOTS; i++)
        g_ps[i].alive = false;
    for (int i = 0; i < MAX_USHOTS; i++)
        g_us[i].alive = false;
    g_ufo.alive = false;
    g_ufo_t = g_saucer_test ? 4.f : fmaxf(8.f, 20.f - 1.5f * (float)w) + frand() * 8.f;
    int n = 4 + w;
    if (n > 11)
        n = 11;
    for (int i = 0; i < n; i++) {
        /* along the edges of the field, away from the ship in the middle */
        float x, z;
        if (i & 1) {
            x = FX0 + frand() * FLX;
            z = -FZH + frand() * 60.f - 30.f;
        } else {
            x = FX0 + frand() * 50.f - 25.f;
            z = -FZH + frand() * FLZ;
        }
        float a = frand() * 6.2832f, sp = rock_speed(0);
        add_rock(0, x, z, cosf(a) * sp, sinf(a) * sp, i);
    }
    g_wave_t = 0;
    g_beat_t = 1.f;
}

static void ship_reset(void)
{
    g_sx = FXC;
    g_sz = 0;
    g_svx = g_svz = 0;
    g_head = 0;
    g_roll = 0;
    g_hyper_t = 0;
    g_ship_on = true;
    g_inv_t = 2.5f;
}

static void new_game(int wave)
{
    g_wave = wave;
    g_lives = 3;
    g_score = 0;
    g_next_life = LIFE_EVERY;
    load_wave(g_wave);
    ship_reset();
    g_state = ST_PLAY;
    g_state_t = 0;
}

/* ── Saved game: wave, score, lives and the rocks ────────────────────────── */
#define SAVE_PATH "/sd/data/driftrock/save.txt"
static int   g_save_wave = -1;       /* wave of the saved game, -1 = none */
static float g_msg_t;                /* "GAME SAVED" on the screen while > 0 */
static const char *g_msg;

static void note(const char *msg)
{
    g_msg = msg;
    g_msg_t = 2.f;
}

static void save_peek(void)
{
    g_save_wave = -1;
    FILE *f = fopen(SAVE_PATH, "r");
    if (!f)
        return;
    int w;
    if (fscanf(f, "DRIFTROCK 1 wave %d", &w) == 1 && w >= 0)
        g_save_wave = w;
    fclose(f);
}

/* false when there is nothing to save (title, game over) or the card cannot be written */
static bool save_game(void)
{
    int wave = g_wave;
    bool fresh = false;                  /* the wave is cleared: save the start of the next one */
    if (g_state == ST_OVER || g_state == ST_TITLE || g_lives <= 0)
        return false;
    if (g_state == ST_CLEAR || g_rocks_left <= 0) {
        wave++;
        fresh = true;
    }
    mkdir("/sd/data", 0777);
    mkdir("/sd/data/driftrock", 0777);
    FILE *f = fopen(SAVE_PATH, "w");
    if (!f) {
        papp_log("driftrock: cannot write %s\n", SAVE_PATH);
        return false;
    }
    int n = fresh ? 0 : g_rocks_left;
    fprintf(f, "DRIFTROCK 1 wave %d score %d lives %d next %d rocks %d\n", wave, g_score, g_lives, g_next_life, n);
    for (int i = 0; i < MAX_ROCKS && n > 0; i++) {
        const rock_t *r = &g_rock[i];
        if (r->alive)
            fprintf(f, "%d %d %d %d %d %d\n", r->size, r->shape, (int)(r->x * 10.f), (int)(r->z * 10.f),
                    (int)(r->vx * 10.f), (int)(r->vz * 10.f));
    }
    bool ok = fclose(f) == 0;
    if (ok)
        g_save_wave = wave;
    papp_log("driftrock: %s wave %d, score %d, lives %d, rocks %d\n", ok ? "saved" : "could not save", wave + 1,
             g_score, g_lives, n);
    return ok;
}

static bool load_game(void)
{
    FILE *f = fopen(SAVE_PATH, "r");
    if (!f)
        return false;
    int wave, score, lives, next, n;
    bool ok = fscanf(f, "DRIFTROCK 1 wave %d score %d lives %d next %d rocks %d", &wave, &score, &lives, &next,
                     &n) == 5 && wave >= 0 && lives > 0 && lives <= 9 && n >= 0 && n <= MAX_ROCKS;
    static rock_t saved[MAX_ROCKS];
    int got = 0;
    for (int i = 0; ok && i < n; i++) {
        int s, sh, x, z, vx, vz;
        ok = fscanf(f, "%d %d %d %d %d %d", &s, &sh, &x, &z, &vx, &vz) == 6 && s >= 0 && s < 3 && sh >= 0 &&
             sh < NSHAPES && abs(vx) < 5000 && abs(vz) < 5000;
        if (ok)
            saved[got++] = (rock_t){ .x = (float)x * 0.1f, .z = (float)z * 0.1f, .vx = (float)vx * 0.1f,
                                     .vz = (float)vz * 0.1f, .size = (uint8_t)s, .shape = (uint8_t)sh };
    }
    fclose(f);
    if (!ok) {
        papp_log("driftrock: the saved game is damaged\n");
        return false;
    }
    load_wave(wave);                     /* clears shots, the saucer; fresh rocks for the wave */
    if (got > 0) {
        memset(g_rock, 0, sizeof(g_rock));
        g_rocks_left = 0;
        for (int i = 0; i < got; i++)
            add_rock(saved[i].size, saved[i].x, saved[i].z, saved[i].vx, saved[i].vz, saved[i].shape);
    }
    memset(g_part, 0, sizeof(g_part));
    memset(g_boom, 0, sizeof(g_boom));
    memset(g_pop, 0, sizeof(g_pop));
    g_shake = g_flash = 0;
    g_wave = wave;
    g_score = score;
    if (g_score > g_hiscore)
        g_hiscore = g_score;
    g_lives = lives;
    g_next_life = next > score ? next : score + LIFE_EVERY;
    ship_reset();
    g_state = ST_PLAY;
    g_state_t = 0;
    papp_log("driftrock: continued at wave %d, score %d, lives %d, rocks %d\n", wave + 1, score, lives, got);
    return true;
}

/* ── Rules ───────────────────────────────────────────────────────────────── */
static void kill_ship(void)
{
    boom(g_sx, 0, g_sz, 34.f, 0.9f, 0xFFE0A0);
    boom(g_sx, 0, g_sz, 20.f, 0.5f, 0xFFFFFF);
    spawn_parts(g_sx, 0, g_sz, g_svx, g_svz, 0xE0F0FF, 16, 150.f, false);
    spawn_parts(g_sx, 0, g_sz, g_svx, g_svz, 0xFFA040, 18, 170.f, true);
    sfx(300, 40, 900, WAVE_NOISE, 0.32f);
    sfx(500, 60, 700, WAVE_SQUARE, 0.18f);
    g_shake = 9.f;
    g_flash = 1.f;
    g_ship_on = false;
    g_thrusting = false;
    g_lives--;
    g_state = ST_DEAD;
    g_state_t = 0;
}

/* break rock i; (vx, vz) = the velocity of what hit it */
static void break_rock(int i, float vx, float vz, bool scored)
{
    rock_t *r = &g_rock[i];
    if (!r->alive)
        return;
    r->alive = false;
    g_rocks_left--;
    int s = r->size;
    float rr = rock_r[s];
    uint32_t col = rock_rgb[r->shape];
    boom(r->x, 0, r->z, rr * 1.1f, 0.35f + 0.15f * (float)(2 - s), 0xFFE8C0);
    spawn_parts(r->x, 0, r->z, r->vx, r->vz, col, 7 - 2 * s, 60.f + 40.f * (float)(2 - s), false);
    spawn_parts(r->x, 0, r->z, r->vx, r->vz, 0xFFB050, 6 - s, 110.f, true);
    if (scored)
        add_score(rock_pts[s]);
    static const float f0[3] = { 160.f, 280.f, 520.f };
    sfx(f0[s] * 3.f, f0[s] * 0.4f, 260 - 60 * s, WAVE_NOISE, 0.30f - 0.06f * (float)s);
    sfx(f0[s], f0[s] * 0.5f, 160, WAVE_TRI, 0.18f);
    g_shake = fmaxf(g_shake, 3.f - (float)s);
    if (s < 2) {
        /* two smaller ones, flung apart across the line of the hit */
        float hl = sqrtf(vx * vx + vz * vz) + 1e-3f;
        float hx = vx / hl, hz = vz / hl;
        for (int k = 0; k < 2; k++) {
            float side = k ? 1.f : -1.f;
            float a = atan2f(hz, hx) + side * (0.6f + frand() * 0.8f);
            float sp = rock_speed(s + 1);
            float nvx = r->vx * 0.5f + cosf(a) * sp, nvz = r->vz * 0.5f + sinf(a) * sp;
            add_rock(s + 1, r->x - hz * side * rr * 0.4f, r->z + hx * side * rr * 0.4f, nvx, nvz, -1);
        }
    }
}

static void kill_ufo(bool scored)
{
    ufo_t *u = &g_ufo;
    if (!u->alive)
        return;
    u->alive = false;
    int pts = u->small ? 1000 : 200;
    papp_log("driftrock: saucer down%s\n", scored ? "" : " (no score)");
    boom(u->x, 2.f, u->z, 40.f, 0.8f, 0xFFD0FF);
    spawn_parts(u->x, 2.f, u->z, u->vx, u->vz, 0xFF50FF, 14, 150.f, false);
    spawn_parts(u->x, 2.f, u->z, u->vx, u->vz, 0xFFFFA0, 10, 180.f, true);
    if (scored) {
        pop(u->x, 14.f, u->z, pts);
        add_score(pts);
        for (int q = 0; q < 4; q++)
            sfx_at(1046.f, 1046.f * 1.5f, 80, WAVE_SQUARE, 0.15f, q * 90);
    }
    g_flash = fmaxf(g_flash, 0.5f);
    sfx(1800, 100, 500, WAVE_NOISE, 0.25f);
}

static float ship_dir_x(void) { return cosf(g_head * (float)M_PI / 180.f); }
static float ship_dir_z(void) { return sinf(g_head * (float)M_PI / 180.f); }

/* press: as fast as you tap; held: a steady stream */
static void fire(bool press)
{
    if (g_fire_cd > (press ? 0.1f : 0.f) || g_state != ST_PLAY || !g_ship_on || g_hyper_t > 0)
        return;
    for (int i = 0; i < MAX_PSHOTS; i++)
        if (!g_ps[i].alive) {
            float dx = ship_dir_x(), dz = ship_dir_z();
            g_ps[i] = (shot_t){ g_sx + dx * 16.f, g_sz + dz * 16.f, dx * SHOT_SPEED + g_svx * 0.6f,
                                dz * SHOT_SPEED + g_svz * 0.6f, SHOT_LIFE, true };
            g_fire_cd = 0.2f;
            sfx(1500, 380, 110, WAVE_SQUARE, 0.15f);
            return;
        }
}

static void hyperspace(void)
{
    if (g_state != ST_PLAY || !g_ship_on || g_hyper_t > 0)
        return;
    boom(g_sx, 0, g_sz, 18.f, 0.4f, 0x80E0FF);
    spawn_parts(g_sx, 0, g_sz, 0, 0, 0x60D0FF, 12, 120.f, true);
    sfx(300, 2400, 450, WAVE_TRI, 0.2f);
    g_hyper_t = 0.7f;
    g_thrusting = false;
}

/* the hyperspace jump ends: somewhere else, at a standstill; a rock there is the end of the ship */
static void hyper_land(void)
{
    g_sx = FX0 + 30.f + frand() * (FLX - 60.f);
    g_sz = -FZH + 30.f + frand() * (FLZ - 60.f);
    g_svx = g_svz = 0;
    boom(g_sx, 0, g_sz, 22.f, 0.45f, 0x80E0FF);
    spawn_parts(g_sx, 0, g_sz, 0, 0, 0xA0F0FF, 10, 90.f, true);
    sfx(2400, 300, 300, WAVE_TRI, 0.18f);
    for (int i = 0; i < MAX_ROCKS; i++) {
        rock_t *r = &g_rock[i];
        float rr = rock_r[r->size] + SHIP_R;
        if (r->alive && dist2w(r->x, r->z, g_sx, g_sz) < rr * rr) {
            break_rock(i, g_svx, g_svz, true);
            kill_ship();
            return;
        }
    }
}

static void update_ufo(float dt)
{
    ufo_t *u = &g_ufo;
    if (!u->alive) {
        if (g_state != ST_PLAY)
            return;
        g_ufo_t -= dt;
        if (g_ufo_t > 0)
            return;
        g_ufo_t = fmaxf(7.f, 18.f - 1.2f * (float)g_wave) + frand() * 8.f;
        float dir = frand() < 0.5f ? 1.f : -1.f;
        bool small = g_score >= 40000 || (g_wave >= 1 && frand() < 0.15f + 0.1f * (float)g_wave);
        *u = (ufo_t){ .alive = true, .small = small, .x = FX0 + 40.f + frand() * (FLX - 80.f), .z = -dir * (FZH + 10.f),
                      .vz = dir * (small ? 95.f : 70.f), .turn_t = 1.f, .fire_t = 1.2f };
        papp_log("driftrock: %s saucer\n", small ? "small" : "big");
        return;
    }
    u->age += dt;
    u->turn_t -= dt;
    if (u->turn_t <= 0) {
        u->turn_t = 0.8f + frand() * 1.4f;
        static const float vx[3] = { -60.f, 0.f, 60.f };
        u->vx = vx[(int)(frand() * 3.f) % 3] * (u->small ? 1.3f : 1.f);
    }
    u->x = wrapf(u->x + u->vx * dt, FX0, FLX);
    u->z += u->vz * dt;
    if (fabsf(u->z) > FZH + 20.f) {
        u->alive = false;
        return;
    }
    u->wob -= dt;
    if (u->wob <= 0) {
        u->wob = u->small ? 0.12f : 0.18f;
        sfx(u->small ? 900.f : 520.f, u->small ? 1300.f : 760.f, u->small ? 100 : 150, WAVE_TRI, 0.08f);
    }
    u->fire_t -= dt;
    if (u->fire_t <= 0 && g_state == ST_PLAY) {
        u->fire_t = (u->small ? 0.9f : 1.3f) * (0.7f + 0.6f * frand());
        float a;
        if (u->small && g_ship_on && g_hyper_t <= 0) {
            float err = fmaxf(3.f, 26.f - 3.f * (float)g_wave) * (frand() - 0.5f) * 2.f;
            a = atan2f(dwz(u->z, g_sz), dwx(u->x, g_sx)) + err * (float)M_PI / 180.f;
        } else
            a = frand() * 6.2832f;
        for (int i = 0; i < MAX_USHOTS; i++)
            if (!g_us[i].alive) {
                g_us[i] = (shot_t){ u->x + cosf(a) * 14.f, u->z + sinf(a) * 14.f, cosf(a) * 250.f, sinf(a) * 250.f,
                                    1.3f, true };
                sfx(700, 300, 90, WAVE_SQUARE, 0.10f);
                break;
            }
    }
}

/* move a shot; true while it lives */
static bool move_shot(shot_t *s, float dt)
{
    s->life -= dt;
    if (s->life <= 0) {
        s->alive = false;
        return false;
    }
    s->x = wrapf(s->x + s->vx * dt, FX0, FLX);
    s->z = wrapf(s->z + s->vz * dt, -FZH, FLZ);
    return true;
}

static int shot_hits_rock(const shot_t *s)
{
    for (int i = 0; i < MAX_ROCKS; i++) {
        const rock_t *r = &g_rock[i];
        float rr = rock_r[r->size] * 0.95f + 2.f;
        if (r->alive && dist2w(r->x, r->z, s->x, s->z) < rr * rr)
            return i;
    }
    return -1;
}

static void update(float dt)
{
    g_t += dt;
    g_state_t += dt;
    if (g_shake > 0) g_shake = fmaxf(0, g_shake - dt * 18.f);
    if (g_flash > 0) g_flash = fmaxf(0, g_flash - dt * 2.5f);
    if (g_fire_cd > 0) g_fire_cd -= dt;
    if (g_inv_t > 0) g_inv_t -= dt;

    /* the ship */
    if (g_ship_on && g_state != ST_TITLE) {
        if (g_hyper_t > 0) {
            g_hyper_t -= dt;
            if (g_hyper_t <= 0)
                hyper_land();
        } else {
            if (g_thrusting) {
                g_svx += ship_dir_x() * THRUST * dt;
                g_svz += ship_dir_z() * THRUST * dt;
                float sp = sqrtf(g_svx * g_svx + g_svz * g_svz);
                if (sp > MAX_SPEED) {
                    g_svx *= MAX_SPEED / sp;
                    g_svz *= MAX_SPEED / sp;
                }
                /* a trail of sparks and a rumble */
                g_trail_t -= dt;
                if (g_trail_t <= 0) {
                    g_trail_t = 0.035f;
                    float tx = g_sx - ship_dir_x() * 15.f, tz = g_sz - ship_dir_z() * 15.f;
                    spawn_parts(tx, 1.f, tz, g_svx - ship_dir_x() * 160.f, g_svz - ship_dir_z() * 160.f,
                                frand() < 0.5f ? 0xFFC040 : 0xFF7020, 1, 20.f, true);
                }
                g_thrust_snd -= dt;
                if (g_thrust_snd <= 0) {
                    g_thrust_snd = 0.07f;
                    sfx(180, 120, 90, WAVE_NOISE, 0.07f);
                }
            }
            float k = expf(-DRAG * dt);
            g_svx *= k;
            g_svz *= k;
            g_sx = wrapf(g_sx + g_svx * dt, FX0, FLX);
            g_sz = wrapf(g_sz + g_svz * dt, -FZH, FLZ);
        }
    }

    /* rocks */
    for (int i = 0; i < MAX_ROCKS; i++) {
        rock_t *r = &g_rock[i];
        if (!r->alive)
            continue;
        r->x = wrapf(r->x + r->vx * dt, FX0, FLX);
        r->z = wrapf(r->z + r->vz * dt, -FZH, FLZ);
        for (int k = 0; k < 3; k++)
            r->rot[k] = fmodf(r->rot[k] + r->spin[k] * dt, 360.f);
    }

    /* player shots: rocks, the saucer */
    for (int i = 0; i < MAX_PSHOTS; i++) {
        shot_t *s = &g_ps[i];
        /* two half steps so a fast shot cannot skip a small rock */
        for (int h = 0; h < 2 && s->alive; h++) {
            if (!move_shot(s, dt * 0.5f))
                break;
            int r = shot_hits_rock(s);
            if (r >= 0) {
                break_rock(r, s->vx, s->vz, true);
                s->alive = false;
                break;
            }
            if (g_ufo.alive) {
                float ur = g_ufo.small ? 13.f : 22.f;
                if (dist2w(g_ufo.x, g_ufo.z, s->x, s->z) < ur * ur) {
                    kill_ufo(true);
                    s->alive = false;
                }
            }
        }
    }
    /* saucer shots: rocks, the ship */
    for (int i = 0; i < MAX_USHOTS; i++) {
        shot_t *s = &g_us[i];
        for (int h = 0; h < 2 && s->alive; h++) {
            if (!move_shot(s, dt * 0.5f))
                break;
            int r = shot_hits_rock(s);
            if (r >= 0) {
                break_rock(r, s->vx, s->vz, false);
                s->alive = false;
                break;
            }
            if (g_state == ST_PLAY && g_ship_on && g_hyper_t <= 0 && g_inv_t <= 0 && !g_icon &&
                dist2w(g_sx, g_sz, s->x, s->z) < SHIP_R * SHIP_R) {
                s->alive = false;
                kill_ship();
            }
        }
    }

    update_ufo(dt);
    if (g_ufo.alive) {
        float ur = g_ufo.small ? 13.f : 22.f;
        /* it comes in where the rocks of a new wave are: a moment's grace */
        for (int i = 0; i < MAX_ROCKS && g_ufo.alive && g_ufo.age > 1.5f; i++) {
            rock_t *r = &g_rock[i];
            float rr = rock_r[r->size] * 0.9f + ur;
            if (r->alive && dist2w(r->x, r->z, g_ufo.x, g_ufo.z) < rr * rr) {
                break_rock(i, g_ufo.vx, g_ufo.vz, false);
                kill_ufo(false);
            }
        }
        if (g_ufo.alive && g_state == ST_PLAY && g_ship_on && g_hyper_t <= 0 && g_inv_t <= 0 && !g_icon &&
            dist2w(g_sx, g_sz, g_ufo.x, g_ufo.z) < (ur + SHIP_R) * (ur + SHIP_R)) {
            kill_ufo(true);
            kill_ship();
        }
    }

    /* the ship against the rocks */
    if (g_state == ST_PLAY && g_ship_on && g_hyper_t <= 0 && g_inv_t <= 0 && !g_icon)
        for (int i = 0; i < MAX_ROCKS; i++) {
            rock_t *r = &g_rock[i];
            float rr = rock_r[r->size] * 0.85f + SHIP_R * 0.8f;
            if (r->alive && dist2w(r->x, r->z, g_sx, g_sz) < rr * rr) {
                break_rock(i, g_svx - r->vx, g_svz - r->vz, true);
                kill_ship();
                break;
            }
        }

    /* the beat: two low notes, quicker as the wave goes on */
    if (g_state == ST_PLAY && g_rocks_left > 0) {
        g_wave_t += dt;
        g_beat_t -= dt;
        if (g_beat_t <= 0) {
            g_beat_t = fmaxf(0.26f, 0.95f - 0.012f * g_wave_t);
            float f = (g_beat_n++ & 1) ? 82.4f : 92.5f;
            sfx(f, f * 0.92f, 110, WAVE_SQUARE, 0.2f);
        }
    }

    /* sparks: they drift and slow down (space) */
    for (int k = 0; k < MAX_PARTS; k++) {
        spark_t *s = &g_part[k];
        if (s->life <= 0)
            continue;
        s->life -= dt;
        float d = expf(-1.6f * dt);
        for (int a = 0; a < 3; a++) {
            s->v[a] *= d;
            s->p[a] += s->v[a] * dt;
            s->rot[a] += s->spin[a] * dt;
        }
    }
    for (int i = 0; i < MAX_BOOMS; i++)
        if (g_boom[i].t < g_boom[i].dur)
            g_boom[i].t += dt;
    for (int i = 0; i < MAX_POPS; i++)
        if (g_pop[i].t > 0) {
            g_pop[i].t -= dt;
            g_pop[i].p[0] += 25.f * dt;
        }

    switch (g_state) {
    case ST_PLAY:
        if (g_rocks_left <= 0) {
            g_state = ST_CLEAR;
            g_state_t = 0;
            add_score(0);
            static const float arp[5] = { 523.3f, 659.3f, 784.f, 1046.5f, 1318.5f };
            for (int i = 0; i < 5; i++)
                sfx_at(arp[i], arp[i], 160, WAVE_SQUARE, 0.2f, 300 + i * 90);
        }
        break;
    case ST_DEAD:
        if (g_state_t > 1.8f) {
            if (g_lives <= 0) {
                g_state = ST_OVER;
                g_state_t = 0;
                hiscore_io(true);
                static const float dn[4] = { 392.f, 330.f, 262.f, 196.f };
                for (int i = 0; i < 4; i++)
                    sfx_at(dn[i], dn[i] * 0.97f, 220, WAVE_TRI, 0.3f, i * 200);
            } else {
                /* back in the middle once nothing is too near it */
                bool clear = true;
                for (int i = 0; i < MAX_ROCKS && clear; i++) {
                    float rr = rock_r[g_rock[i].size] + 55.f;
                    if (g_rock[i].alive && dist2w(g_rock[i].x, g_rock[i].z, FXC, 0) < rr * rr)
                        clear = false;
                }
                if (clear || g_state_t > 6.f) {
                    ship_reset();
                    g_state = ST_PLAY;
                    g_state_t = 0;
                    sfx(400, 800, 200, WAVE_TRI, 0.15f);
                }
            }
        }
        break;
    case ST_CLEAR:
        if (g_state_t > 2.4f && !g_ufo.alive) {
            g_wave++;
            load_wave(g_wave);
            g_state = ST_PLAY;
            g_state_t = 0;
            g_inv_t = fmaxf(g_inv_t, 1.5f);
        }
        break;
    case ST_OVER:
        if (g_auto && g_state_t > 3.f)
            new_game(0);
        break;
    }
}

/* ── Camera maths (no heading or tilt) ───────────────────────────────────── */
static bool project(float x, float y, float z, float *sx, float *sy)
{
    float a = -CAM_PITCH * (float)M_PI / 180.f;
    float d = cosf(a) * (x - CAM_X) - sinf(a) * (y - CAM_Y);
    float v = sinf(a) * (x - CAM_X) + cosf(a) * (y - CAM_Y);
    if (d < 1.f)
        return false;
    *sx = W * 0.5f + FOCAL * z / d;
    *sy = H * 0.5f - FOCAL * v / d;
    return true;
}

/* screen point -> the point of the plane y = py under it */
static bool unproject(float sx, float sy, float py, float *x, float *z)
{
    float a = -CAM_PITCH * (float)M_PI / 180.f;
    float u = (sx - W * 0.5f) / FOCAL, v = (H * 0.5f - sy) / FOCAL;
    float dx = cosf(a) + v * sinf(a), dy = -sinf(a) + v * cosf(a);
    if (dy > -1e-3f)
        return false;
    float t = (py - CAM_Y) / dy;
    *x = CAM_X + t * dx;
    *z = t * u;
    return true;
}

/* ── Input ───────────────────────────────────────────────────────────────── */
typedef struct {
    bool start, pause, fire, fire_press, thrust, hyper, touch_tap;
    float turn;                 /* -1..1, left = negative */
    bool aim;                   /* touch: turn towards aim_head, fire when facing it */
    float aim_head;
} actions_t;

/* touch buttons (surface pixels) */
#define TB_X   6
#define TB_W   70
#define TB_THR_Y 186
#define TB_THR_H 48
#define TB_HYP_Y 150
#define TB_HYP_H 30
static uint32_t s_prev_btn;
static int s_touch_n;
static bool s_touch_used, s_hyper_held;

static bool in_rect(int x, int y, int rx, int ry, int rw, int rh) { return x >= rx && x < rx + rw && y >= ry && y < ry + rh; }

static actions_t read_input(papp_surface_t *surf)
{
    actions_t a = { 0 };
    papp_gamepad_t pad = { 0 };
    uint32_t held = papp_input_gamepad(0, &pad) ? pad.buttons : 0;
    uint32_t press = held & ~s_prev_btn;
    s_prev_btn = held;
    if (press & (PAPP_BTN_A | PAPP_BTN_B | PAPP_BTN_X | PAPP_BTN_Y | PAPP_BTN_R1 | PAPP_BTN_L1))
        a.start = true;
    if (press & (PAPP_BTN_START | PAPP_BTN_SELECT))
        a.pause = true;
    if (held & (PAPP_BTN_B | PAPP_BTN_X | PAPP_BTN_R2))
        a.fire = true;
    if (press & (PAPP_BTN_B | PAPP_BTN_X | PAPP_BTN_R2))
        a.fire_press = true;
    if (held & (PAPP_BTN_UP | PAPP_BTN_A | PAPP_BTN_R1))
        a.thrust = true;
    if (press & (PAPP_BTN_DOWN | PAPP_BTN_Y | PAPP_BTN_L1 | PAPP_BTN_L2))
        a.hyper = true;
    if (held & PAPP_BTN_LEFT) a.turn -= 1.f;
    if (held & PAPP_BTN_RIGHT) a.turn += 1.f;
    if (a.turn == 0 && (pad.lx > 6000 || pad.lx < -6000))
        a.turn = (float)pad.lx / 32767.f;
    if (pad.ly < -16000)
        a.thrust = true;

    /* touch: up to two fingers; each one is a button or a point to aim at */
    papp_touch_point_t tp[2];
    int n = papp_disp_touch(surf, tp, 2);
    bool hyper_now = false;
    if (n > 0) {
        s_touch_used = true;
        for (int i = 0; i < n && i < 2; i++) {
            int x = tp[i].x, y = tp[i].y;
            bool corner = x > W - 48 && y < 32;
            if (corner) {
                if (s_touch_n == 0)
                    a.pause = true;
            } else if (in_rect(x, y, TB_X - 4, TB_THR_Y - 4, TB_W + 8, TB_THR_H + 8) && g_state == ST_PLAY) {
                a.thrust = true;
            } else if (in_rect(x, y, TB_X - 4, TB_HYP_Y - 4, TB_W + 8, TB_HYP_H + 4) && g_state == ST_PLAY) {
                hyper_now = true;
            } else {
                float wx, wz;
                if (s_touch_n == 0)
                    a.touch_tap = true;
                if (g_state == ST_PLAY && unproject((float)x, (float)y, 0, &wx, &wz)) {
                    a.aim = true;
                    a.aim_head = atan2f(wz - g_sz, wx - g_sx) * 180.f / (float)M_PI;
                }
            }
        }
    }
    if (hyper_now && !s_hyper_held)
        a.hyper = true;
    s_hyper_held = hyper_now;
    s_touch_n = n > 0 ? n : 0;
    return a;
}

/* the computer plays: aim ahead of the nearest rock, keep away, jump when cornered */
static void auto_play(actions_t *a)
{
    *a = (actions_t){ 0 };
    if (g_state != ST_PLAY || !g_ship_on || g_hyper_t > 0)
        return;
    float best = 1e9f, tx = 0, tz = 0, danger = 1e9f;
    for (int i = 0; i < MAX_ROCKS; i++) {
        const rock_t *r = &g_rock[i];
        if (!r->alive)
            continue;
        float dx = dwx(g_sx, r->x), dz = dwz(g_sz, r->z);
        float d = sqrtf(dx * dx + dz * dz);
        float t = d / SHOT_SPEED;
        float ax = dx + (r->vx - g_svx * 0.6f) * t, az = dz + (r->vz - g_svz * 0.6f) * t;
        if (d < best) {
            best = d;
            tx = ax;
            tz = az;
        }
        /* closing in? */
        float closing = -(dx * (r->vx - g_svx) + dz * (r->vz - g_svz)) / (d + 1e-3f);
        float gap = d - rock_r[r->size] - SHIP_R;
        if (closing > 0 && gap / closing < 0.35f)
            danger = fminf(danger, gap);
    }
    if (g_ufo.alive) {
        float dx = dwx(g_sx, g_ufo.x), dz = dwz(g_sz, g_ufo.z), d = sqrtf(dx * dx + dz * dz);
        if (d < best * 1.5f) {
            float t = d / SHOT_SPEED;
            tx = dx + g_ufo.vx * t;
            tz = dz + g_ufo.vz * t;
        }
    }
    if (best < 1e8f) {
        float want = atan2f(tz, tx) * 180.f / (float)M_PI;
        float err = fmodf(want - g_head + 540.f, 360.f) - 180.f;
        a->turn = err > 4.f ? 1.f : err < -4.f ? -1.f : err / 4.f;
        if (fabsf(err) < 9.f && best < 330.f)
            a->fire = true;
    }
    float sp = sqrtf(g_svx * g_svx + g_svz * g_svz);
    if (best > 220.f && sp < 40.f && fmodf(g_t, 3.f) < 0.4f)
        a->thrust = true;
    if (danger < 25.f && g_inv_t <= 0)
        a->hyper = true;
}

static void control_ship(const actions_t *a, float dt)
{
    g_thrusting = false;
    float turn = 0;
    if (g_state != ST_PLAY || !g_ship_on || g_hyper_t > 0) {
        g_roll += (0 - g_roll) * fminf(1.f, dt * 8.f);
        return;
    }
    if (a->aim) {
        float err = fmodf(a->aim_head - g_head + 540.f, 360.f) - 180.f;
        float step = TURN_DPS * 1.4f * dt;
        turn = err > step ? 1.4f : err < -step ? -1.4f : err / (TURN_DPS * dt + 1e-6f);
        if (fabsf(err) < 14.f)
            fire(false);
    } else
        turn = a->turn;
    g_head = fmodf(g_head + turn * TURN_DPS * dt + 360.f, 360.f);
    g_roll += (fmaxf(-1.f, fminf(1.f, turn)) * 28.f - g_roll) * fminf(1.f, dt * 10.f);
    g_thrusting = a->thrust;
    if (a->fire_press)
        fire(true);
    else if (a->fire)
        fire(false);
    if (a->hyper)
        hyperspace();
}

/* ── Drawing ─────────────────────────────────────────────────────────────── */
static e3d_mesh *m_rock[3][NSHAPES], *m_ship, *m_flame, *m_engine, *m_pshot, *m_ushot, *m_part, *m_boom, *m_ufo,
                *m_ufo_dome, *m_ufo_lights, *m_planet, *m_ring;

/* starfield: a few stars per row, made beforehand (the row function may only
 * write its row); a faint band of nebula across the sky */
#define NSTAR_ROW 3
#define NSKY 40              /* sky colours across a row (10 pixels each) */
static uint16_t s_star_x[H][NSTAR_ROW], s_star_c[H][NSTAR_ROW], s_sky[H][NSKY];
static uint8_t  s_star_tw[H][NSTAR_ROW];
static volatile int s_twinkle;

static uint16_t rgb565(uint32_t c) { return PAPP_RGB565((int)(c >> 16), (int)((c >> 8) & 255), (int)(c & 255)); }

static void make_sky(void)
{
    for (int y = 0; y < H; y++) {
        float t = (float)y / (float)H;
        /* colours across the row: a diagonal band of violet haze */
        for (int q = 0; q < NSKY; q++) {
            float u = ((float)q + 0.5f) / (float)NSKY;
            float band = expf(-powf((u - (0.25f + 0.6f * t)) * 3.2f, 2.f));
            uint32_t base = mix_rgb(0x03030C, 0x070A1C, t);
            s_sky[y][q] = rgb565(mix_rgb(base, 0x1C0E2E, band * 0.8f));
        }
        for (int i = 0; i < NSTAR_ROW; i++) {
            bool on = frand() < 0.4f;
            s_star_x[y][i] = on ? (uint16_t)(frand() * (W - 1)) : 0xFFFF;
            int b = 70 + (int)(frand() * 185);
            float h = frand();
            uint32_t c = h < 0.2f ? 0xFFD8B0 : h < 0.45f ? 0xC0D8FF : 0xFFFFFF;
            s_star_c[y][i] = rgb565(mix_rgb(0, c, (float)b / 255.f));
            s_star_tw[y][i] = (uint8_t)(frand() * 255);
        }
    }
}

static void sky_row(uint16_t *row, int y, int w, void *ctx)
{
    const uint16_t *c = s_sky[y];
    for (int q = 0, x = 0; q < NSKY; q++) {
        int x1 = (q + 1) * w / NSKY;
        for (uint16_t v = c[q]; x < x1; x++)
            row[x] = v;
    }
    int tw = s_twinkle;
    for (int i = 0; i < NSTAR_ROW; i++)
        if (s_star_x[y][i] < w) {
            /* now and then a star dims for a moment */
            if (((s_star_tw[y][i] + tw) & 63) < 3)
                continue;
            row[s_star_x[y][i]] = s_star_c[y][i];
        }
}

static void draw_obj(e3d_t *e, const e3d_mesh *mesh, const e3d_mat *m, int32_t rgb, uint32_t flags)
{
    e3d_object o;
    e3d_object_init(&o, mesh);
    o.m = *m;
    o.color = rgb;
    o.flags = flags | E3D_NO_SHADOW;
    e3d_draw(e, &o);
}

/* Something at (x, z) of radius r: drawn again on the far side while it
 * crosses an edge of the field. fn draws it at a position. */
typedef void (*draw_at_fn)(e3d_t *e, float x, float z, const void *ctx);
static void draw_wrapped(e3d_t *e, float x, float z, float r, draw_at_fn fn, const void *ctx)
{
    float ox[2] = { 0, 0 }, oz[2] = { 0, 0 };
    int nx = 1, nz = 1;
    if (x > FX1 - r) ox[nx++] = -FLX;
    else if (x < FX0 + r) ox[nx++] = FLX;
    if (z > FZH - r) oz[nz++] = -FLZ;
    else if (z < -FZH + r) oz[nz++] = FLZ;
    for (int i = 0; i < nx; i++)
        for (int j = 0; j < nz; j++)
            fn(e, x + ox[i], z + oz[j], ctx);
}

static void rock_at(e3d_t *e, float x, float z, const void *ctx)
{
    const rock_t *r = ctx;
    float s = rock_r[r->size];
    e3d_mat m = mat_at(x, 0, z, r->rot[0], r->rot[1], r->rot[2], s, s, s);
    draw_obj(e, m_rock[r->size][r->shape], &m, -1, 0);
}

static void ship_at(e3d_t *e, float x, float z, const void *ctx)
{
    e3d_mat m = mat_at(x, -4.f * SHIP_SC, z, g_roll, g_head, 0, SHIP_SC, SHIP_SC, SHIP_SC);
    draw_obj(e, m_ship, &m, 0xE8ECF8, 0);
    /* engine glow, and the flame while thrusting */
    float pulse = g_thrusting ? 1.5f + 0.4f * sinf(g_t * 47.f) : 1.0f + 0.2f * sinf(g_t * 9.f);
    e3d_mat l = mat_at(-9.f, 4.f, 0, 0, 0, 0, pulse, pulse, pulse), q = m;
    e3d_mul(&q, &l);
    draw_obj(e, m_engine, &q, (int32_t)mix_rgb(0x3070FF, 0xC0F0FF, pulse - 1.f), E3D_UNLIT);
    if (g_thrusting) {
        float len = 9.f + 5.f * frand(), rad = 2.6f + 0.6f * frand();
        l = mat_at(-9.5f, 4.f, 0, g_t * 900.f, 0, 0, len, rad, rad);
        q = m;
        e3d_mul(&q, &l);
        draw_obj(e, m_flame, &q, (int32_t)mix_rgb(0xFF6010, 0xFFE070, frand()), E3D_UNLIT);
        l = mat_at(-9.5f, 4.f, 0, 0, 0, 0, len * 0.55f, rad * 0.55f, rad * 0.55f);
        q = m;
        e3d_mul(&q, &l);
        draw_obj(e, m_flame, &q, 0xFFFFE0, E3D_UNLIT);
    }
}

static void ufo_at(e3d_t *e, float x, float z, const void *ctx)
{
    float s = g_ufo.small ? 1.15f : 1.9f;
    /* tipped towards the camera so the dome and the rim show */
    e3d_mat m = mat_at(x, 2.f * sinf(g_t * 4.f), z, 6.f * sinf(g_t * 3.f), 0, -22.f, s, s, s), r;
    e3d_rotate(&r, 0, g_t * 160.f, 0);
    e3d_mul(&m, &r);
    draw_obj(e, m_ufo, &m, g_ufo.small ? 0xE89838 : 0xA868E0, 0);
    /* the dome glows in time with the warble, the rim lights chase round */
    float p = 0.5f + 0.5f * sinf(g_t * 14.f);
    draw_obj(e, m_ufo_dome, &m, (int32_t)mix_rgb(0x40F0A0, 0xE0FFF0, p), E3D_UNLIT);
    draw_obj(e, m_ufo_lights, &m, ((int)(g_t * 8.f) & 1) ? 0xFFF060 : 0xFF4060, E3D_UNLIT);
}

typedef struct { float y, sc, spin; int32_t rgb; const e3d_mesh *mesh; } shot_ctx_t;
static void shot_at(e3d_t *e, float x, float z, const void *ctx)
{
    const shot_ctx_t *c = ctx;
    e3d_mat m = mat_at(x, c->y, z, c->spin, c->spin * 0.7f, 0, c->sc, c->sc, c->sc);
    draw_obj(e, c->mesh, &m, c->rgb, E3D_UNLIT);
}

static void draw_scene(e3d_t *e)
{
    e3d_camera *cam = e3d_cam(e);
    float sh = g_shake;
    *cam = (e3d_camera){ .pos = { CAM_X + (frand() - 0.5f) * sh, CAM_Y + (frand() - 0.5f) * sh, (frand() - 0.5f) * sh },
                         .pitch = CAM_PITCH };
    if (g_icon)
        *cam = (e3d_camera){ .pos = { -112.f, 134.f, 6.f }, .pitch = -50.f };

    float glow = 1.f + 0.5f * g_flash;
    *e3d_light_get(e) = (e3d_light){ .pos = { 700.f, 900.f, -900.f },
                                     .r = fminf(255, 250 * glow), .g = fminf(255, 240 * glow), .b = fminf(255, 228 * glow) };

    e3d_mat m;
    /* a ringed planet far below the field */
    if (!g_icon) {
        const float px = 980.f, py = -1000.f, pz = 900.f;
        m = mat_at(px, py, pz, 0, g_t * 2.f, 0, 1, 1, 1);
        draw_obj(e, m_planet, &m, -1, 0);
        m = mat_at(px, py, pz, 24.f, 0, -18.f, 1, 1, 1);
        draw_obj(e, m_ring, &m, -1, E3D_DOUBLE_SIDED);
    }

    for (int i = 0; i < MAX_ROCKS; i++)
        if (g_rock[i].alive)
            draw_wrapped(e, g_rock[i].x, g_rock[i].z, rock_r[g_rock[i].size] * 1.2f, rock_at, &g_rock[i]);

    if (g_ufo.alive)
        draw_wrapped(e, g_ufo.x, g_ufo.z, 22.f, ufo_at, NULL);

    /* the ship (blinks while it cannot be hit after a respawn) */
    bool show = g_ship_on && g_hyper_t <= 0 && g_state != ST_TITLE && g_state != ST_OVER;
    if (g_inv_t > 0 && g_state == ST_PLAY && ((int)(g_inv_t * 10.f) & 1))
        show = false;
    if (show || g_icon)
        draw_wrapped(e, g_sx, g_sz, 18.f, ship_at, NULL);

    for (int i = 0; i < MAX_PSHOTS; i++)
        if (g_ps[i].alive) {
            shot_ctx_t c = { 0, 1.f, g_t * 800.f, 0xB0FFFF, m_pshot };
            draw_wrapped(e, g_ps[i].x, g_ps[i].z, 4.f, shot_at, &c);
        }
    for (int i = 0; i < MAX_USHOTS; i++)
        if (g_us[i].alive) {
            shot_ctx_t c = { 0, 1.2f, g_t * 600.f, ((int)(g_t * 16.f) & 1) ? 0xFF5040 : 0xFFC080, m_ushot };
            draw_wrapped(e, g_us[i].x, g_us[i].z, 4.f, shot_at, &c);
        }

    /* sparks: debris is lit, hot sparks glow and cool */
    for (int k = 0; k < MAX_PARTS; k++) {
        const spark_t *s = &g_part[k];
        if (s->life <= 0)
            continue;
        float t = s->life / s->life0;
        float sc = 1.3f * s->size * (t < 0.3f ? t / 0.3f : 1.f);
        m = mat_at(s->p[0], s->p[1], s->p[2], s->rot[0], s->rot[1], s->rot[2], sc, sc, sc);
        if (s->hot)
            draw_obj(e, m_part, &m, (int32_t)mix_rgb(0x701000, s->rgb, t * 1.4f), E3D_UNLIT | E3D_DOUBLE_SIDED);
        else
            draw_obj(e, m_part, &m, (int32_t)s->rgb, E3D_FLAT | E3D_DOUBLE_SIDED);
    }

    /* fireballs: an unlit sphere that swells and cools */
    for (int i = 0; i < MAX_BOOMS; i++) {
        const boom_t *b = &g_boom[i];
        if (b->t >= b->dur)
            continue;
        float u = b->t / b->dur;
        float sc = b->size * (0.35f + 0.65f * sqrtf(u)) * (1.f - u * u) / 10.f;
        if (sc < 0.05f)
            continue;
        uint32_t c = u < 0.25f ? mix_rgb(0xFFFFFF, b->rgb, u / 0.25f)
                   : u < 0.6f ? mix_rgb(b->rgb, 0xFF8020, (u - 0.25f) / 0.35f)
                              : mix_rgb(0xFF8020, 0x901800, (u - 0.6f) / 0.4f);
        m = mat_at(b->p[0], b->p[1], b->p[2], 0, u * 90.f, 0, sc, sc, sc);
        draw_obj(e, m_boom, &m, (int32_t)c, E3D_UNLIT);
    }
}

static void text_at(uint16_t *px, int x, int y, const char *s, uint16_t col, int scale)
{
    e3d_text(px, W, W, H, x + 1, y + 1, s, 0x0000, scale);
    e3d_text(px, W, W, H, x, y, s, col, scale);
}

static void text_c(uint16_t *px, int y, const char *s, uint16_t col, int scale)
{
    int w = (int)strlen(s) * 6 * scale - scale;
    text_at(px, (W - w) / 2, y, s, col, scale);
}

static void fill(uint16_t *px, int x, int y, int w, int h, uint16_t col)
{
    for (int j = y; j < y + h; j++)
        for (int i = x; i < x + w; i++)
            if (i >= 0 && i < W && j >= 0 && j < H)
                px[j * W + i] = col;
}

/* a small ship for the lives */
static void ship_icon(uint16_t *px, int x, int y, uint16_t col)
{
    for (int i = 0; i < 7; i++)
        fill(px, x + 5 - i / 2, y + i, 1 + (i / 2) * 2, 1, col);
    fill(px, x + 1, y + 7, 9, 1, 0x4A69);
}

/* darken a box to 3/8 (rows y0..y1, columns x0..x1) */
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

/* darken rows y0..y1 to 3/8, fading out over the top and bottom 6 rows */
static void shade(uint16_t *px, int y0, int y1)
{
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < H; y++) {
        int e = y - y0 < y1 - 1 - y ? y - y0 : y1 - 1 - y;
        if (e > 5) e = 5;
        int keep = 8 - e;
        if (keep < 3) keep = 3;
        shade_box(px, 0, y, W, y + 1, keep);
    }
}

static void frame_box(uint16_t *px, int x, int y, int w, int h, uint16_t col)
{
    fill(px, x, y, w, 1, col);
    fill(px, x, y + h - 1, w, 1, col);
    fill(px, x, y, 1, h, col);
    fill(px, x + w - 1, y, 1, h, col);
}

static void draw_touch_buttons(uint16_t *px, bool thrust, bool hyper)
{
    shade_box(px, TB_X, TB_THR_Y, TB_X + TB_W, TB_THR_Y + TB_THR_H, thrust ? 8 : 5);
    frame_box(px, TB_X, TB_THR_Y, TB_W, TB_THR_H, thrust ? 0xFE60 : 0x6B6D);
    text_at(px, TB_X + (TB_W - 35) / 2, TB_THR_Y + 20, "THRUST", thrust ? 0xFE60 : 0xBDF7, 1);
    shade_box(px, TB_X, TB_HYP_Y, TB_X + TB_W, TB_HYP_Y + TB_HYP_H, hyper ? 8 : 5);
    frame_box(px, TB_X, TB_HYP_Y, TB_W, TB_HYP_H, hyper ? 0x07FF : 0x6B6D);
    text_at(px, TB_X + (TB_W - 29) / 2, TB_HYP_Y + 11, "HYPER", hyper ? 0x07FF : 0xBDF7, 1);
}

static void draw_hud(uint16_t *px)
{
    char line[48];
    snprintf(line, sizeof(line), "SCORE %06d", g_score);
    text_at(px, 4, 3, line, 0xFFFF, 1);
    snprintf(line, sizeof(line), "HI %06d", g_hiscore);
    text_at(px, 4, 12, line, 0xFFE0, 1);
    if (g_state != ST_TITLE) {
        snprintf(line, sizeof(line), "WAVE %d", g_wave + 1);
        text_c(px, 3, line, 0xFFE0, 1);
    }
    int spare = g_lives - (g_state == ST_DEAD || g_state == ST_OVER ? 0 : 1);
    for (int i = 0; i < spare && i < 8 && g_state != ST_TITLE; i++)
        ship_icon(px, W - 36 - i * 12, 3, 0x9FFF);
    text_at(px, W - 16, 3, "II", 0xC618, 1);

    for (int i = 0; i < MAX_POPS; i++)
        if (g_pop[i].t > 0) {
            float sx, sy;
            if (project(g_pop[i].p[0], g_pop[i].p[1], g_pop[i].p[2], &sx, &sy)) {
                snprintf(line, sizeof(line), "%d", g_pop[i].pts);
                text_at(px, (int)sx - 3 * (int)strlen(line), (int)sy, line, 0xFFE0, 1);
            }
        }

    if (s_touch_used && g_state == ST_PLAY)
        draw_touch_buttons(px, g_thrusting, g_hyper_t > 0);

    if (g_msg_t > 0 && g_msg)
        text_c(px, 214, g_msg, 0x07FF, 2);

    switch (g_state) {
    case ST_TITLE:
        shade(px, 52, 196);
        text_c(px, 64, "DRIFTROCK", 0xFFFF, 4);
        text_c(px, 102, "BREAK THE ROCKS - MIND THE SAUCERS", 0xFE60, 1);
        if (((int)(g_t * 2.f)) & 1)
            text_c(px, 124, "PRESS A OR TAP TO PLAY", 0xFFFF, 1);
        text_c(px, 142, "LEFT/RIGHT TURN   UP OR A THRUST", 0xA534, 1);
        text_c(px, 152, "B FIRES   DOWN OR Y HYPERSPACE", 0xA534, 1);
        text_c(px, 162, "TOUCH: TAP TO AIM AND FIRE", 0xA534, 1);
        if (g_save_wave >= 0) {
            snprintf(line, sizeof(line), "SAVED GAME  WAVE %d", g_save_wave + 1);
            text_c(px, 180, line, 0x9FFF, 1);
        }
        break;
    case ST_PLAY:
        if (g_state_t < 2.f && g_lives > 0 && g_wave_t < 2.f) {
            snprintf(line, sizeof(line), "WAVE %d", g_wave + 1);
            shade(px, 90, 132);
            text_c(px, 100, line, 0xFFFF, 3);
        }
        break;
    case ST_CLEAR:
        shade(px, 86, 128);
        text_c(px, 96, "WAVE CLEAR", 0x07FF, 3);
        break;
    case ST_OVER:
        shade(px, 72, 172);
        text_c(px, 86, "GAME OVER", 0xF9A6, 4);
        snprintf(line, sizeof(line), "SCORE %d", g_score);
        text_c(px, 126, line, 0xFFFF, 2);
        if (g_state_t > 1.f)
            text_c(px, 156, "PRESS A OR TAP", 0xBDF7, 1);
        break;
    }
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
    int r = papp_ui_menu(title, items, n, 0);
    papp_sem_give(g_pres_done);
    papp_input_flush();
    s_prev_btn = ~0u;          /* ignore the buttons still held */
    return r;
}

/* returns false to exit */
static bool pause_menu(void)
{
    enum { M_RESUME, M_SAVE, M_LOAD, M_SAVE_EXIT, M_RESTART, M_EXIT };
    static const char *const items[] = { "Resume", "Save game", "Load game", "Save and exit", "Restart", "Exit" };
    g_thrusting = false;
    int r = menu("Driftrock", items, 6);
    if (r == M_SAVE || r == M_SAVE_EXIT) {
        bool ok = save_game();
        note(ok ? "GAME SAVED" : g_state == ST_OVER || g_state == ST_TITLE || g_lives <= 0 ? "NOTHING TO SAVE"
                                                                                          : "COULD NOT SAVE");
        if (ok && r == M_SAVE_EXIT)
            return false;
    } else if (r == M_LOAD) {
        save_peek();
        if (g_save_wave < 0)
            note("NO SAVED GAME");
        else if (load_game())
            note("GAME LOADED");
        else
            note("SAVE DAMAGED");
    } else if (r == M_RESTART)
        new_game(0);
    return r != M_EXIT && !papp_exit_requested();
}

/* title screen: continue the saved game or start a new one */
static void start_menu(int start_wave)
{
    if (g_save_wave < 0) {
        new_game(start_wave);
        return;
    }
    char cont[32];
    snprintf(cont, sizeof(cont), "Continue (wave %d)", g_save_wave + 1);
    const char *const items[] = { cont, "New game" };
    int r = menu("Driftrock", items, 2);
    if (r == 0) {
        if (!load_game()) {
            note("SAVE DAMAGED");
            new_game(start_wave);
        }
    } else if (r == 1)
        new_game(start_wave);
}

static bool make_meshes(void)
{
    for (int s = 0; s < 3; s++)
        for (int k = 0; k < NSHAPES; k++) {
            m_rock[s][k] = rock(s < 2, (uint32_t)(k * 7 + s * 101 + 3), rock_rgb[k], s == 2 ? 0.3f : 0.22f);
            if (!m_rock[s][k])
                return false;
        }
    {
        part_t p[3] = { { hull(), mat_at(0, 0, 0, 0, 0, 0, 1, 1, 1) },
                        { box(5.f, 1.f, 2.f), mat_at(-3.f, 3.f, -11.f, 0, 0, 0, 1, 1, 1) },
                        { box(5.f, 1.f, 2.f), mat_at(-3.f, 3.f, 11.f, 0, 0, 0, 1, 1, 1) } };
        m_ship = merge(p, 3, 20.f);
    }
    {
        part_t p[2] = { { e3d_mesh_sphere(12.f, 12, 4, 255, 255, 255), mat_at(0, 0, 0, 0, 0, 0, 1, 0.28f, 1) },
                        { box(3.f, 1.f, 3.f), mat_at(0, -3.5f, 0, 0, 45.f, 0, 1, 1, 1) } };
        m_ufo = merge(p, 2, 40.f);
    }
    {
        part_t p[1] = { { e3d_mesh_sphere(5.5f, 8, 4, 255, 255, 255), mat_at(0, 3.f, 0, 0, 0, 0, 1, 0.8f, 1) } };
        m_ufo_dome = merge(p, 1, 40.f);
    }
    {
        part_t p[8];
        for (int i = 0; i < 8; i++) {
            float a = (float)i * 45.f * (float)M_PI / 180.f;
            p[i] = (part_t){ octa(1.6f, 1.6f, 1.6f), mat_at(cosf(a) * 12.5f, 0, sinf(a) * 12.5f, 0, 0, 0, 1, 1, 1) };
        }
        m_ufo_lights = merge(p, 8, 1.f);
    }
    m_flame = cone(6);
    m_engine = e3d_mesh_sphere(2.4f, 6, 4, 255, 255, 255);
    m_pshot = octa(3.2f, 2.f, 2.f);
    m_ushot = octa(3.f, 3.f, 3.f);
    m_part = tetra(2.2f);
    m_boom = e3d_mesh_sphere(10.f, 8, 6, 255, 255, 255);
    m_planet = e3d_mesh_sphere(150.f, 20, 12, 70, 96, 170);
    if (m_planet)
        for (int i = 0; i < m_planet->ntris; i++) {
            /* bands of cloud by latitude */
            float y = (m_planet->pos[m_planet->tris[i].v[0]][1] + m_planet->pos[m_planet->tris[i].v[1]][1] +
                       m_planet->pos[m_planet->tris[i].v[2]][1]) / 3.f;
            int band = (int)((y + 150.f) / 300.f * 12.f);
            static const uint32_t bands[6] = { 0x5068B4, 0x6E7CBC, 0x8C8EC4, 0x6474B8, 0x8088C0, 0x5C70B6 };
            uint32_t c = bands[band % 6];
            m_planet->tris[i].r = (uint8_t)(c >> 16);
            m_planet->tris[i].g = (uint8_t)(c >> 8);
            m_planet->tris[i].b = (uint8_t)c;
        }
    m_ring = ring(190.f, 265.f, 40, 0xA89070);
    return m_ship && m_ufo && m_ufo_dome && m_ufo_lights && m_flame && m_engine && m_pshot && m_ushot && m_part && m_boom &&
           m_planet && m_ring;
}

static void free_meshes(void)
{
    for (int s = 0; s < 3; s++)
        for (int k = 0; k < NSHAPES; k++)
            e3d_mesh_free(m_rock[s][k]);
    e3d_mesh *all[] = { m_ship, m_flame, m_engine, m_pshot, m_ushot, m_part, m_boom, m_ufo, m_ufo_dome, m_ufo_lights,
                        m_planet, m_ring };
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        e3d_mesh_free(all[i]);
}

/* a still for the icon: a big rock breaking up in front of the ship */
static void icon_scene(void)
{
    memset(g_rock, 0, sizeof(g_rock));
    g_rocks_left = 0;
    add_rock(0, 38.f, 30.f, 0, 0, 2);
    g_rock[0].rot[0] = 30.f;
    g_rock[0].rot[1] = 50.f;
    g_rock[0].rot[2] = 10.f;
    add_rock(2, 62.f, -38.f, 0, 0, 1);
    add_rock(1, -52.f, 62.f, 0, 0, 0);
    g_sx = -36.f;
    g_sz = -34.f;
    g_head = 44.f;
    g_roll = -14.f;
    g_ship_on = true;
    g_thrusting = true;
    g_ps[0] = (shot_t){ -2.f, -4.f, 0, 0, 1.f, true };
    g_ps[1] = (shot_t){ 12.f, 9.f, 0, 0, 1.f, true };
    g_boom[0] = (boom_t){ { 14.f, 8.f, 6.f }, 0.22f, 0.5f, 15.f, 0xFFD080 };
    spawn_parts(14.f, 8.f, 6.f, 0, 0, 0xFFB050, 10, 60.f, true);
    for (int k = 0; k < MAX_PARTS; k++)
        for (int q = 0; q < 3; q++)
            g_part[k].p[q] += g_part[k].v[q] * 0.12f;
}

int main(int argc, char **argv)
{
    int start_wave = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "auto"))
            g_auto = true;
        else if (!strcmp(argv[i], "saucer"))
            g_saucer_test = true;
        else if (!strcmp(argv[i], "icon"))
            g_icon = true;
        else if (!strcmp(argv[i], "wave") && i + 1 < argc)
            start_wave = atoi(argv[++i]) - 1;
    }
    if (start_wave < 0)
        start_wave = 0;
    srand((unsigned)papp_time_us());

    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 1 };
    g_surf = papp_disp_surface_create(&cfg);
    uint16_t *fb[2];
    for (int i = 0; i < 2; i++)
        fb[i] = papp_alloc(sizeof(uint16_t) * W * H, PAPP_MEM_LARGE | PAPP_MEM_DMA, 64);
    g_pres_go = papp_sem_create(0, 1);
    g_pres_done = papp_sem_create(1, 1);
    papp_thread_cfg_t pcfg = { .name = "dr_present", .stack_size = 4096, .prio = PAPP_PRIO_HIGH, .core = 0 };
    papp_thread_t *pres = (g_surf && fb[0] && fb[1] && g_pres_go && g_pres_done)
                              ? papp_thread_create(presenter_main, NULL, &pcfg) : NULL;
    e3d_t *e = e3d_create(W, H, 2);
    if (!pres || !e || !make_meshes()) {
        papp_log("driftrock: out of memory\n");
        return 1;
    }
    make_sky();
    e3d_set_focal(e, g_icon ? 260.f : FOCAL);
    e3d_set_background_rows(e, sky_row, NULL);
    e3d_params *par = e3d_params_get(e);
    par->shadows = false;
    par->ambient = 0.30f;
    par->diffuse = 0.95f;
    par->specular = 0.30f;
    par->shininess = 12.f;

    g_snd_lock = papp_mutex_create(false);
    papp_audio_cfg_t acfg = { .rate = SR, .channels = 1, .format = PAPP_AUDIO_S16, .latency_ms = 60 };
    g_snd = g_snd_lock ? papp_audio_open(&acfg) : NULL;
    papp_thread_cfg_t acfg_t = { .name = "dr_audio", .stack_size = 4096, .prio = PAPP_PRIO_HIGH, .core = 1 };
    papp_thread_t *aud = g_snd ? papp_thread_create(audio_main, NULL, &acfg_t) : NULL;
    if (!aud && g_snd) {
        papp_audio_close(g_snd);
        g_snd = NULL;
    }

    hiscore_io(false);
    save_peek();
    g_wave = start_wave;
    load_wave(start_wave);               /* rocks drifting behind the title */
    g_lives = 3;
    g_state = ST_TITLE;
    if (g_auto)
        new_game(start_wave);
    if (g_icon) {
        new_game(0);
        icon_scene();
    }
    papp_log("driftrock: running%s, wave %d\n", g_auto ? " (auto)" : "", start_wave + 1);

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
        if (g_msg_t > 0)
            g_msg_t -= dt;
        s_twinkle = (int)(g_t * 20.f);

        actions_t act = read_input(g_surf);
        if (act.pause && g_state != ST_TITLE) {
            run = pause_menu();
            last = papp_time_us();
            continue;
        }
        bool go = act.start || act.touch_tap;
        if (g_state == ST_TITLE) {
            if (go)
                start_menu(start_wave);
            else if (act.pause)
                run = pause_menu();
        } else if (g_state == ST_OVER && go && g_state_t > 1.f)
            new_game(0);
        if (g_auto) {
            bool p = act.pause;
            auto_play(&act);
            act.pause = p;
        }
        if (g_icon)
            dt = 0;
        else
            control_ship(&act, dt);
        update(dt);

        uint16_t *px = fb[cur];
        e3d_begin(e, px, W);
        draw_scene(e);
        e3d_end(e);
        if (!g_icon)
            draw_hud(px);

        frames++;
        if (now - stat_t >= 1000000) {
            fps = (float)frames * 1e6f / (float)(now - stat_t);
            frames = 0;
            stat_t = now;
        }
        if (now - log_t >= 5000000) {
            const e3d_stats *st = e3d_get_stats(e);
            papp_audio_stats_t as = { 0 };
            if (g_snd)
                papp_audio_stats(g_snd, &as);
            papp_log("driftrock: %.1f fps, wave %d, rocks %d, lives %d, score %d, ship %.0f/%.0f, objs %d, tris %d, "
                     "geom %d us, raster %d us, underruns %lu\n",
                     (double)fps, g_wave + 1, g_rocks_left, g_lives, g_score, (double)g_sx, (double)g_sz, st->objects,
                     st->tris_drawn, st->t_geom_us, st->t_raster_us, (unsigned long)as.underruns);
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
    if (g_score > 0 && g_score >= g_hiscore)
        hiscore_io(true);
    e3d_destroy(e);
    free_meshes();
    papp_sem_destroy(g_pres_go);
    papp_sem_destroy(g_pres_done);
    papp_free(fb[0]);
    papp_free(fb[1]);
    papp_disp_surface_destroy(g_surf);
    return 0;
}
