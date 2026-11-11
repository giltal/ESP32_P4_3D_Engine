/*
 * Voidmarch — a fixed shooter in 3D on the e3d engine.
 *
 * A formation of invaders marches across a lit plane: side to side, a step
 * nearer at each edge, faster as it thins out. Your ship guards the near end.
 * It moves freely in the plane (left/right and forwards/back) within its zone
 * in front of the bunkers. Everything stands on the plane and casts a shadow
 * from the lamp; hits blow up in fireballs and shards that bounce on the floor.
 * A saucer crosses at the back now and then.
 *
 * A formation that reaches the line in front of your zone costs a life and is
 * pushed back. Clear it to finish the wave; the next one starts closer and
 * marches faster.
 *
 * Pad: D-pad or left stick moves (up = forwards), A/B fires.
 * Touch: the ship follows the finger (a little ahead of it); holding fires.
 * Pause: START, SELECT or tap the top-right corner.
 *
 * Arguments: `auto` = the computer plays (attract mode, for testing),
 *            `wave N` = start at wave N, `icon` = a still close-up for the icon.
 */
#include "papp/papp.h"
#include "e3d.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

PAPP_MANIFEST(.id = "giltal.voidmarch", .name = "Voidmarch", .version = PAPP_APP_VERSION, .main_stack = 16384);

#define W 400
#define H 240

/* ── World (x = depth away from the camera, z = right, y = up) ─────────────── */
#define FZ        200.f         /* the ship and the formation stay within z = +-FZ */
#define FIELD_X0  (-40.f)
#define FIELD_X1  580.f
#define SHIP_X0   48.f          /* the ship's zone along x */
#define SHIP_X1   140.f
#define SHIP_Y    5.f
#define SHIP_HZ   15.f
#define SHIP_HX   12.f
#define LOSE_X    162.f         /* a formation whose front row gets here has landed */
#define BUNK_X    188.f
#define NBUNK     4
#define BCOLS     4             /* bunker: 4 columns of blocks, the outer two 2 high, the inner two 1 (an arch) */
#define BLK_HZ    6.f
#define BLK_HX    6.f
#define BLK_H     9.f
#define ICOLS     7
#define IROWS     5
#define INV_DZ    44.f
#define INV_DX    38.f
#define INV_SC    1.8f          /* invader meshes are drawn this much larger */
#define INV_HZ    15.f
#define INV_HX    12.f
#define INV_Y     17.f          /* centre height of an invader above the plane */
#define SHOT_Y    8.f
#define MAX_PSHOTS 2
#define MAX_ESHOTS 16
#define MAX_PARTS  120
#define MAX_BOOMS  12
#define MAX_POPS   6

/* Camera (Shardwall's) */
#define CAM_X     (-310.f)
#define CAM_Y      430.f
#define CAM_PITCH (-37.f)
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

/* A box from y = 0 to h with bevelled top edges (Shardwall's brick). */
static e3d_mesh *bevel_box(float hx, float hz, float h, float bev, bool bottom)
{
    e3d_mesh *m = e3d_mesh_create(12, bottom ? 20 : 18);
    if (!m)
        return NULL;
    static const float sx[4] = { -1, 1, 1, -1 }, sz[4] = { -1, -1, 1, 1 };
    for (int i = 0; i < 4; i++) {
        float *p0 = m->pos[i], *p1 = m->pos[4 + i], *p2 = m->pos[8 + i];
        p0[0] = sx[i] * hx;         p0[1] = 0;       p0[2] = sz[i] * hz;
        p1[0] = sx[i] * hx;         p1[1] = h - bev; p1[2] = sz[i] * hz;
        p2[0] = sx[i] * (hx - bev); p2[1] = h;       p2[2] = sz[i] * (hz - bev);
    }
    float ctr[3] = { 0, h * 0.5f, 0 };
    int nt = 0;
    for (int i = 0; i < 4; i++) {
        int j = (i + 1) & 3;
        quad_out(m, &nt, i, j, 4 + j, 4 + i, ctr);
        quad_out(m, &nt, 4 + i, 4 + j, 8 + j, 8 + i, ctr);
    }
    quad_out(m, &nt, 8, 9, 10, 11, ctr);
    if (bottom)
        quad_out(m, &nt, 0, 1, 2, 3, ctr);
    e3d_mesh_finish(m, 30.f);
    return m;
}

/* Several parts, each placed by a matrix, as one mesh (fewer objects to draw). */
typedef struct { e3d_mesh *mesh; e3d_mat m; } part_t;

static e3d_mesh *merge(part_t *parts, int n, float crease)
{
    int nv = 0, nt = 0;
    for (int i = 0; i < n; i++) {
        if (!parts[i].mesh)
            return NULL;
        nv += parts[i].mesh->nverts;
        nt += parts[i].mesh->ntris;
    }
    e3d_mesh *m = e3d_mesh_create(nv, nt);
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

/* the ship's hull: an arrowhead pointing along +x */
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
enum { KIND_SPIKE, KIND_WING, KIND_DOME, NKINDS };
static const int kind_of_row[IROWS] = { KIND_DOME, KIND_DOME, KIND_WING, KIND_WING, KIND_SPIKE };   /* row 0 = front */
static const int kind_pts[NKINDS] = { 30, 20, 10 };

typedef struct {
    bool  alive[IROWS][ICOLS];
    int   left;                 /* invaders alive */
    float x;                    /* x of the front row */
    float z;                    /* z of the formation's centre */
    float dir;                  /* +1 / -1 */
    float drop;                 /* x still to step towards the ship */
    float fire_t;               /* time to the next shot */
    float beat_t;               /* time to the next march step (pose and sound) */
    int   pose;
    float hit_t;                /* > 0: it just landed */
} form_t;

typedef struct { float x, z; bool alive; } pshot_t;
typedef struct { float x, z, vx, spin; bool alive; } eshot_t;
typedef struct { float p[3], v[3], rot[3], spin[3], life, life0, size; uint32_t rgb; bool hot; } spark_t;
typedef struct { float p[3], t, dur, size; uint32_t rgb; } boom_t;
typedef struct { float p[3], t; int pts; } pop_t;
typedef struct { float x, z, dir, wob; bool alive; } ufo_t;

enum { ST_TITLE, ST_PLAY, ST_DEAD, ST_CLEAR, ST_OVER };

static form_t  g_f;
static uint8_t g_bunk[NBUNK][BCOLS];     /* blocks left in each column */
static pshot_t g_ps[MAX_PSHOTS];
static eshot_t g_es[MAX_ESHOTS];
static spark_t g_part[MAX_PARTS];
static boom_t  g_boom[MAX_BOOMS];
static pop_t   g_pop[MAX_POPS];
static ufo_t   g_ufo;
static float   g_ufo_t;
static int     g_state, g_wave, g_lives, g_score, g_hiscore, g_next_life;
static float   g_state_t, g_t, g_shake, g_flash, g_inv_t;
static float   g_sx = SHIP_X0 + 10.f, g_sz;   /* ship position in the plane */
static float   g_fire_cd, g_roll, g_pitch;
static bool    g_auto, g_icon;
static int     g_beat_n;

static float frand(void) { return (float)rand() / (float)RAND_MAX; }

static uint32_t hsv(float h, float s, float v)
{
    h = fmodf(h, 360.f);
    if (h < 0) h += 360.f;
    float c = v * s, x = c * (1.f - fabsf(fmodf(h / 60.f, 2.f) - 1.f)), m = v - c, r, g, b;
    if (h < 60)       { r = c; g = x; b = 0; }
    else if (h < 120) { r = x; g = c; b = 0; }
    else if (h < 180) { r = 0; g = c; b = x; }
    else if (h < 240) { r = 0; g = x; b = c; }
    else if (h < 300) { r = x; g = 0; b = c; }
    else              { r = c; g = 0; b = x; }
    return ((uint32_t)((r + m) * 255) << 16) | ((uint32_t)((g + m) * 255) << 8) | (uint32_t)((b + m) * 255);
}

static uint32_t mix_rgb(uint32_t a, uint32_t b, float t)
{
    if (t < 0) t = 0;
    if (t > 1) t = 1;
    int r = (int)((float)(a >> 16) + ((float)(b >> 16) - (float)(a >> 16)) * t);
    int g = (int)((float)((a >> 8) & 255) + ((float)((b >> 8) & 255) - (float)((a >> 8) & 255)) * t);
    int bl = (int)((float)(a & 255) + ((float)(b & 255) - (float)(a & 255)) * t);
    return ((uint32_t)r << 16) | ((uint32_t)g << 8) | (uint32_t)bl;
}

static uint32_t row_rgb(int r) { return hsv(200.f - 38.f * (float)r, 0.62f, 0.95f); }

static float inv_x(int r) { return g_f.x + (float)r * INV_DX; }
static float inv_z(int c) { return g_f.z + ((float)c - (ICOLS - 1) * 0.5f) * INV_DZ; }
static float bunk_z(int b) { return ((float)b - (NBUNK - 1) * 0.5f) * 100.f; }
static float bcol_z(int b, int c) { return bunk_z(b) + ((float)c - (BCOLS - 1) * 0.5f) * (BLK_HZ * 2.f + 0.6f); }
static int   bcol_full(int c) { return (c == 0 || c == BCOLS - 1) ? 2 : 1; }

/* march speed (units/s along z) */
static float form_speed(void)
{
    float base = 20.f + 3.f * (float)(g_wave < 10 ? g_wave : 10);
    float thin = 1.f - (float)g_f.left / (float)(IROWS * ICOLS);
    return base * (1.f + 4.f * thin * thin * thin + 1.2f * thin);
}

static void reset_bunkers(void)
{
    for (int b = 0; b < NBUNK; b++)
        for (int c = 0; c < BCOLS; c++)
            g_bunk[b][c] = (uint8_t)bcol_full(c);
}

static void load_wave(int w)
{
    memset(&g_f, 0, sizeof(g_f));
    for (int r = 0; r < IROWS; r++)
        for (int c = 0; c < ICOLS; c++)
            g_f.alive[r][c] = true;
    g_f.left = IROWS * ICOLS;
    g_f.x = 340.f - 9.f * (float)(w < 8 ? w : 8);
    g_f.z = -20.f;
    g_f.dir = 1.f;
    g_f.fire_t = 1.5f;
    for (int i = 0; i < MAX_ESHOTS; i++)
        g_es[i].alive = false;
    for (int i = 0; i < MAX_PSHOTS; i++)
        g_ps[i].alive = false;
    g_ufo.alive = false;
    g_ufo_t = 12.f + frand() * 8.f;
    reset_bunkers();
}

static void new_game(int wave)
{
    g_wave = wave;
    g_lives = 3;
    g_score = 0;
    g_next_life = 1500;
    g_sz = 0;
    g_sx = SHIP_X0 + 10.f;
    load_wave(g_wave);
    g_state = ST_PLAY;
    g_state_t = 0;
    g_inv_t = 1.5f;
}

/* ── Effects ─────────────────────────────────────────────────────────────── */
static void spawn_parts(float x, float y, float z, uint32_t rgb, int n, float speed, bool hot)
{
    for (int k = 0, i = 0; k < MAX_PARTS && i < n; k++) {
        spark_t *s = &g_part[k];
        if (s->life > 0)
            continue;
        s->p[0] = x + (frand() - 0.5f) * 10.f;
        s->p[1] = y + (frand() - 0.5f) * 10.f;
        s->p[2] = z + (frand() - 0.5f) * 12.f;
        float a = frand() * 6.2832f, e = frand() * 1.3f;
        s->v[0] = cosf(a) * cosf(e) * speed * (0.4f + frand());
        s->v[1] = sinf(e) * speed * (0.5f + frand()) + 60.f;
        s->v[2] = sinf(a) * cosf(e) * speed * (0.4f + frand());
        for (int q = 0; q < 3; q++) {
            s->rot[q] = frand() * 360.f;
            s->spin[q] = (frand() - 0.5f) * 1000.f;
        }
        s->life0 = s->life = 0.7f + frand() * 0.8f;
        s->size = 0.8f + frand() * 0.8f;
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
    g_score += pts;
    if (g_score >= g_next_life) {
        g_next_life += 3000;
        if (g_lives < 6) {
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
    const char *path = "/sd/data/voidmarch/hiscore.txt";
    if (save) {
        mkdir("/sd/data", 0777);
        mkdir("/sd/data/voidmarch", 0777);
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

/* ── Rules ───────────────────────────────────────────────────────────────── */
static void kill_ship(void)
{
    float y = SHIP_Y + 5.f;
    boom(g_sx, y, g_sz, 34.f, 0.9f, 0xFFE0A0);
    boom(g_sx, y, g_sz, 20.f, 0.5f, 0xFFFFFF);
    spawn_parts(g_sx, y, g_sz, 0xE0F0FF, 18, 160.f, false);
    spawn_parts(g_sx, y, g_sz, 0xFFA040, 14, 130.f, true);
    sfx(300, 40, 900, WAVE_NOISE, 0.30f);
    sfx(500, 60, 700, WAVE_SQUARE, 0.20f);
    g_shake = 9.f;
    g_flash = 1.f;
    g_lives--;
    g_state = ST_DEAD;
    g_state_t = 0;
    for (int i = 0; i < MAX_ESHOTS; i++)
        g_es[i].alive = false;
}

/* a shot crossing the bunker line at z: true if a block stopped it */
static bool hit_bunker(float z)
{
    for (int b = 0; b < NBUNK; b++) {
        if (fabsf(z - bunk_z(b)) > BCOLS * BLK_HZ + 3.f)
            continue;
        for (int c = 0; c < BCOLS; c++) {
            if (!g_bunk[b][c] || fabsf(z - bcol_z(b, c)) > BLK_HZ + 1.5f)
                continue;
            g_bunk[b][c]--;
            float yb = BLK_H * ((float)g_bunk[b][c] + 0.5f + (bcol_full(c) == 1 ? 1.f : 0.f));
            spawn_parts(BUNK_X, yb, bcol_z(b, c), 0x50E070, 5, 80.f, false);
            sfx(900, 300, 50, WAVE_NOISE, 0.10f);
            return true;
        }
    }
    return false;
}

static void kill_invader(int r, int c)
{
    g_f.alive[r][c] = false;
    g_f.left--;
    float x = inv_x(r), y = INV_Y, z = inv_z(c);
    boom(x, y, z, 20.f, 0.45f, 0xFFF0C0);
    spawn_parts(x, y, z, row_rgb(r), 8, 100.f, false);
    spawn_parts(x, y, z, 0xFFC060, 5, 130.f, true);
    add_score(kind_pts[kind_of_row[r]]);
    sfx(1400, 200, 160, WAVE_NOISE, 0.20f);
    sfx(700, 140, 140, WAVE_SQUARE, 0.12f);
}

static void fire(void)
{
    if (g_fire_cd > 0 || g_state != ST_PLAY || g_inv_t > 1.2f)
        return;
    for (int i = 0; i < MAX_PSHOTS; i++)
        if (!g_ps[i].alive) {
            g_ps[i] = (pshot_t){ g_sx + 14.f, g_sz, true };
            g_fire_cd = 0.3f;
            sfx(1500, 380, 110, WAVE_SQUARE, 0.16f);
            return;
        }
}

static void update_form(float dt)
{
    form_t *d = &g_f;
    if (d->hit_t > 0) d->hit_t -= dt;
    if (d->left <= 0)
        return;
    int c0 = ICOLS, c1 = -1, r0 = IROWS;
    for (int r = 0; r < IROWS; r++)
        for (int c = 0; c < ICOLS; c++)
            if (d->alive[r][c]) {
                if (c < c0) c0 = c;
                if (c > c1) c1 = c;
                if (r < r0) r0 = r;
            }
    float sp = form_speed();
    if (d->drop > 0) {
        float s = fminf(d->drop, 70.f * dt);
        d->x -= s;
        d->drop -= s;
    } else {
        d->z += d->dir * sp * dt;
        float zl = inv_z(c0) - INV_HZ, zr = inv_z(c1) + INV_HZ;
        if ((d->dir > 0 && zr > FZ) || (d->dir < 0 && zl < -FZ)) {
            d->z -= d->dir > 0 ? zr - FZ : zl + FZ;
            d->dir = -d->dir;
            d->drop = g_state == ST_TITLE ? 0.f : 13.f;
        }
    }
    /* march beat: the pose flips and the bass steps */
    d->beat_t -= dt;
    if (d->beat_t <= 0) {
        d->beat_t += fmaxf(0.09f, 11.f / sp);
        d->pose ^= 1;
        if (g_state == ST_PLAY) {
            static const float bass[4] = { 98.f, 87.3f, 77.8f, 73.4f };
            float f = bass[g_beat_n++ & 3];
            sfx(f, f * 0.9f, 90, WAVE_SQUARE, 0.22f);
        }
    }
    /* rows passing over the bunkers crush them */
    float front = inv_x(r0);
    if (front - INV_HX < BUNK_X + BLK_HX)
        for (int r = 0; r < IROWS; r++)
            for (int c = 0; c < ICOLS; c++)
                if (d->alive[r][c] && fabsf(inv_x(r) - BUNK_X) < INV_HX + BLK_HX)
                    for (int b = 0; b < NBUNK; b++)
                        for (int q = 0; q < BCOLS; q++)
                            if (g_bunk[b][q] && fabsf(bcol_z(b, q) - inv_z(c)) < INV_HZ + BLK_HZ) {
                                g_bunk[b][q] = 0;
                                spawn_parts(BUNK_X, 8.f, bcol_z(b, q), 0x50E070, 4, 70.f, false);
                            }
    /* landed */
    if (g_state == ST_PLAY && front - INV_HX < LOSE_X) {
        d->hit_t = 1.2f;
        d->x += 120.f;
        d->drop = 0;
        for (int c = 0; c < ICOLS; c++)
            boom(LOSE_X, 10.f, inv_z(c), 26.f, 0.7f, 0xFF6040);
        kill_ship();
        return;
    }
    /* fire: the front-most invader of a column, often the one nearest the ship */
    if (g_state == ST_PLAY && g_inv_t <= 0) {
        d->fire_t -= dt;
        if (d->fire_t <= 0) {
            float base = fmaxf(0.42f, 0.85f - 0.05f * (float)g_wave);
            d->fire_t = base * (0.5f + frand());
            int c = 0, tries = 0;
            do
                c = (int)(frand() * ICOLS) % ICOLS;
            while (tries++ < 12 && !(d->alive[0][c] || d->alive[1][c] || d->alive[2][c] || d->alive[3][c] ||
                                     d->alive[4][c]));
            if (frand() < 0.4f) {
                float bz = 1e9f;
                for (int q = 0; q < ICOLS; q++)
                    for (int r = 0; r < IROWS; r++)
                        if (d->alive[r][q]) {
                            if (fabsf(inv_z(q) - g_sz) < bz) {
                                bz = fabsf(inv_z(q) - g_sz);
                                c = q;
                            }
                            break;
                        }
            }
            for (int r = 0; r < IROWS; r++)
                if (d->alive[r][c]) {
                    for (int i = 0; i < MAX_ESHOTS; i++)
                        if (!g_es[i].alive) {
                            float v = 150.f + 9.f * (float)(g_wave < 10 ? g_wave : 10) + frand() * 40.f;
                            g_es[i] = (eshot_t){ inv_x(r) - INV_HX, inv_z(c), -v, frand() * 360.f, true };
                            sfx(260, 180, 70, WAVE_TRI, 0.10f);
                            break;
                        }
                    break;
                }
        }
    }
}

static void update(float dt)
{
    g_t += dt;
    g_state_t += dt;
    if (g_shake > 0) g_shake = fmaxf(0, g_shake - dt * 18.f);
    if (g_flash > 0) g_flash = fmaxf(0, g_flash - dt * 2.5f);
    if (g_fire_cd > 0) g_fire_cd -= dt;
    if (g_inv_t > 0) g_inv_t -= dt;
    if (g_sz > FZ - SHIP_HZ) g_sz = FZ - SHIP_HZ;
    if (g_sz < -FZ + SHIP_HZ) g_sz = -FZ + SHIP_HZ;
    if (g_sx > SHIP_X1) g_sx = SHIP_X1;
    if (g_sx < SHIP_X0) g_sx = SHIP_X0;

    if (g_state == ST_PLAY || g_state == ST_TITLE)
        update_form(g_state == ST_TITLE ? dt * 0.6f : dt);

    /* player shots */
    for (int i = 0; i < MAX_PSHOTS; i++) {
        pshot_t *s = &g_ps[i];
        if (!s->alive)
            continue;
        float x0 = s->x;
        s->x += 430.f * dt;
        if (s->x > FIELD_X1) {
            s->alive = false;
            continue;
        }
        if (x0 < BUNK_X + BLK_HX && s->x >= BUNK_X - BLK_HX && hit_bunker(s->z)) {
            s->alive = false;
            continue;
        }
        for (int r = 0; r < IROWS && s->alive; r++) {
            float ix = inv_x(r);
            if (s->x + 6.f < ix - INV_HX || x0 - 6.f > ix + INV_HX)
                continue;
            for (int c = 0; c < ICOLS; c++)
                if (g_f.alive[r][c] && fabsf(s->z - inv_z(c)) < INV_HZ + 1.5f) {
                    kill_invader(r, c);
                    s->alive = false;
                    break;
                }
        }
        if (!s->alive)
            continue;
        for (int j = 0; j < MAX_ESHOTS; j++) {
            eshot_t *e = &g_es[j];
            if (e->alive && fabsf(e->z - s->z) < 5.f && e->x < s->x + 5.f && e->x > x0 - 10.f) {
                e->alive = s->alive = false;
                boom(e->x, SHOT_Y, e->z, 10.f, 0.25f, 0xFFFFFF);
                spawn_parts(e->x, SHOT_Y, e->z, 0xFF6060, 4, 70.f, true);
                add_score(5);
                break;
            }
        }
        if (s->alive && g_ufo.alive && s->x > g_ufo.x - 16.f && fabsf(s->z - g_ufo.z) < 22.f) {
            static const int ufo_pts[4] = { 50, 100, 150, 300 };
            int pts = ufo_pts[(int)(frand() * 4) & 3];
            float uy = 26.f;
            boom(g_ufo.x, uy, g_ufo.z, 40.f, 0.8f, 0xFFD0FF);
            spawn_parts(g_ufo.x, uy, g_ufo.z, 0xFF50FF, 14, 150.f, false);
            spawn_parts(g_ufo.x, uy, g_ufo.z, 0xFFFFA0, 10, 180.f, true);
            pop(g_ufo.x, uy + 14.f, g_ufo.z, pts);
            add_score(pts);
            g_ufo.alive = s->alive = false;
            g_flash = fmaxf(g_flash, 0.5f);
            sfx(1800, 100, 500, WAVE_NOISE, 0.25f);
            for (int q = 0; q < 4; q++)
                sfx_at(1046.f, 1046.f * 1.5f, 80, WAVE_SQUARE, 0.15f, q * 90);
        }
    }

    /* enemy shots */
    for (int i = 0; i < MAX_ESHOTS; i++) {
        eshot_t *e = &g_es[i];
        if (!e->alive)
            continue;
        float x0 = e->x;
        e->x += e->vx * dt;
        e->spin += 600.f * dt;
        if (e->x < FIELD_X0) {
            e->alive = false;
            continue;
        }
        if (x0 > BUNK_X - BLK_HX && e->x <= BUNK_X + BLK_HX && hit_bunker(e->z)) {
            e->alive = false;
            continue;
        }
        if (g_state == ST_PLAY && !g_icon && e->x < g_sx + SHIP_HX && x0 > g_sx - SHIP_HX &&
            fabsf(e->z - g_sz) < SHIP_HZ * 0.8f) {
            e->alive = false;
            kill_ship();
        }
    }

    /* saucer */
    if (g_state == ST_PLAY) {
        if (!g_ufo.alive) {
            g_ufo_t -= dt;
            if (g_ufo_t <= 0) {
                g_ufo_t = 15.f + frand() * 10.f;
                float dir = frand() < 0.5f ? 1.f : -1.f;
                g_ufo = (ufo_t){ FIELD_X1 - 50.f, -dir * (FZ + 40.f), dir, 0, true };
            }
        } else {
            g_ufo.z += g_ufo.dir * 75.f * dt;
            g_ufo.wob -= dt;
            if (g_ufo.wob <= 0) {
                g_ufo.wob = 0.16f;
                sfx(620.f, 900.f, 140, WAVE_TRI, 0.09f);
            }
            if (fabsf(g_ufo.z) > FZ + 50.f)
                g_ufo.alive = false;
        }
    }

    /* sparks: gravity, bounce on the plane */
    for (int k = 0; k < MAX_PARTS; k++) {
        spark_t *s = &g_part[k];
        if (s->life <= 0)
            continue;
        s->life -= dt;
        s->v[1] -= 520.f * dt;
        for (int a = 0; a < 3; a++) {
            s->p[a] += s->v[a] * dt;
            s->rot[a] += s->spin[a] * dt;
        }
        if (s->p[1] < 2.f && s->v[1] < 0) {
            s->p[1] = 2.f;
            s->v[1] *= -0.42f;
            s->v[0] *= 0.6f;
            s->v[2] *= 0.6f;
            for (int a = 0; a < 3; a++)
                s->spin[a] *= 0.5f;
        }
    }
    for (int i = 0; i < MAX_BOOMS; i++)
        if (g_boom[i].t < g_boom[i].dur)
            g_boom[i].t += dt;
    for (int i = 0; i < MAX_POPS; i++)
        if (g_pop[i].t > 0) {
            g_pop[i].t -= dt;
            g_pop[i].p[1] += 20.f * dt;
        }

    switch (g_state) {
    case ST_PLAY:
        if (g_f.left <= 0) {
            g_state = ST_CLEAR;
            g_state_t = 0;
            g_flash = 1.f;
            add_score(200 * (g_wave + 1));
            static const float arp[5] = { 523.3f, 659.3f, 784.f, 1046.5f, 1318.5f };
            for (int i = 0; i < 5; i++)
                sfx_at(arp[i], arp[i], 160, WAVE_SQUARE, 0.2f, 300 + i * 90);
        }
        break;
    case ST_DEAD:
        if (g_state_t > 1.6f) {
            if (g_lives > 0) {
                g_state = ST_PLAY;
                g_state_t = 0;
                g_inv_t = 1.4f;           /* the invaders hold their fire for a moment */
            } else {
                g_state = ST_OVER;
                g_state_t = 0;
                hiscore_io(true);
                static const float dn[4] = { 392.f, 330.f, 262.f, 196.f };
                for (int i = 0; i < 4; i++)
                    sfx_at(dn[i], dn[i] * 0.97f, 220, WAVE_TRI, 0.3f, i * 200);
            }
        }
        break;
    case ST_CLEAR:
        if (g_state_t > 2.4f) {
            g_wave++;
            load_wave(g_wave);
            g_state = ST_PLAY;
            g_state_t = 0;
            g_inv_t = 1.5f;
        }
        break;
    case ST_OVER:
        if (g_auto && g_state_t > 3.f)
            new_game(0);
        break;
    }
}

/* ── Camera maths (no heading or tilt) ───────────────────────────────────── */
static float g_cam_x = CAM_X, g_cam_y = CAM_Y, g_cam_z;

static bool project(float x, float y, float z, float *sx, float *sy)
{
    float a = -CAM_PITCH * (float)M_PI / 180.f;
    float d = cosf(a) * (x - g_cam_x) - sinf(a) * (y - g_cam_y);
    float v = sinf(a) * (x - g_cam_x) + cosf(a) * (y - g_cam_y);
    if (d < 1.f)
        return false;
    *sx = W * 0.5f + FOCAL * (z - g_cam_z) / d;
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
    float t = (py - g_cam_y) / dy;
    *x = g_cam_x + t * dx;
    *z = g_cam_z + t * u;
    return true;
}

/* ── Input ───────────────────────────────────────────────────────────────── */
typedef struct { bool start, pause, fire, touch_tap; } actions_t;

static uint32_t s_prev_btn;
static int s_touch_n;

static actions_t read_input(papp_surface_t *surf, float dt)
{
    actions_t a = { 0 };
    papp_gamepad_t pad = { 0 };
    uint32_t held = papp_input_gamepad(0, &pad) ? pad.buttons : 0;
    uint32_t press = held & ~s_prev_btn;
    s_prev_btn = held;
    if (press & (PAPP_BTN_A | PAPP_BTN_B | PAPP_BTN_X | PAPP_BTN_Y | PAPP_BTN_R1 | PAPP_BTN_L1))
        a.start = true;
    if (held & (PAPP_BTN_A | PAPP_BTN_B | PAPP_BTN_R1 | PAPP_BTN_R2))
        a.fire = true;
    if (press & (PAPP_BTN_START | PAPP_BTN_SELECT))
        a.pause = true;

    float dz = 0, dx = 0;
    if (held & PAPP_BTN_LEFT) dz -= 1.f;
    if (held & PAPP_BTN_RIGHT) dz += 1.f;
    if (held & PAPP_BTN_UP) dx += 1.f;
    if (held & PAPP_BTN_DOWN) dx -= 1.f;
    if (dz == 0 && (pad.lx > 7000 || pad.lx < -7000))
        dz = (float)pad.lx / 32767.f;
    if (dx == 0 && (pad.ly > 7000 || pad.ly < -7000))
        dx = -(float)pad.ly / 32767.f;
    float vz = 0, vx = 0;
    if (g_state == ST_PLAY) {
        vz = dz * 175.f;
        vx = dx * 125.f;
    }

    /* touch: the ship goes to the point a little ahead of the finger; holding fires */
    papp_touch_point_t tp[2];
    int n = papp_disp_touch(surf, tp, 2);
    if (n > 0) {
        bool corner = tp[0].x > W - 48 && tp[0].y < 32;
        if (s_touch_n == 0) {
            if (corner)
                a.pause = true;
            else
                a.touch_tap = true;
        }
        float x, z;
        if (!corner && g_state == ST_PLAY && unproject((float)tp[0].x, (float)tp[0].y, SHIP_Y, &x, &z)) {
            x += 22.f;
            float k = fminf(1.f, dt * 12.f);
            vz = (z - g_sz) * k / fmaxf(dt, 1e-3f);
            vx = (x - g_sx) * k / fmaxf(dt, 1e-3f);
            a.fire = true;
        }
    }
    s_touch_n = n > 0 ? n : 0;
    g_sz += vz * dt;
    g_sx += vx * dt;
    g_roll += (fmaxf(-1.f, fminf(1.f, vz / 175.f)) * -24.f - g_roll) * fminf(1.f, dt * 10.f);
    g_pitch += (fmaxf(-1.f, fminf(1.f, vx / 125.f)) * 8.f - g_pitch) * fminf(1.f, dt * 10.f);
    return a;
}

/* the computer plays: line up under the nearest column, weave, dodge */
static void auto_play(float dt, actions_t *a)
{
    if (g_state != ST_PLAY)
        return;
    float tz = g_sz, bd = 1e9f;
    for (int c = 0; c < ICOLS; c++)
        for (int r = 0; r < IROWS; r++)
            if (g_f.alive[r][c]) {
                float z = inv_z(c) + g_f.dir * form_speed() * (inv_x(r) - g_sx) / 430.f;
                if (fabsf(z - g_sz) < bd) {
                    bd = fabsf(z - g_sz);
                    tz = z;
                }
                break;
            }
    float tx = 75.f + 35.f * sinf(g_t * 0.4f);
    for (int i = 0; i < MAX_ESHOTS; i++) {
        eshot_t *e = &g_es[i];
        if (e->alive && e->x < g_sx + 50.f && e->x > g_sx - 5.f && fabsf(e->z - g_sz) < SHIP_HZ + 4.f)
            tz = g_sz + (e->z > g_sz ? -45.f : 45.f);
    }
    float step = 135.f * dt, d = tz - g_sz;
    g_sz += d > step ? step : d < -step ? -step : d;
    step = 90.f * dt;
    d = tx - g_sx;
    g_sx += d > step ? step : d < -step ? -step : d;
    if (fabsf(tz - g_sz) < 7.f)
        a->fire = true;
}

/* ── Drawing ─────────────────────────────────────────────────────────────── */
static e3d_mesh *m_floor, *m_rail_side, *m_rail_back, *m_line, *m_inv[NKINDS][2], *m_ship, *m_engine, *m_blk,
                *m_pshot, *m_eshot, *m_part, *m_boom, *m_ufo;
static e3d_object o_floor;

/* starfield: a few stars per row, made beforehand (the row function may only write its row) */
#define NSTAR_ROW 2
static uint16_t s_star_x[H][NSTAR_ROW], s_star_c[H][NSTAR_ROW], s_sky[H];

static void make_sky(void)
{
    for (int y = 0; y < H; y++) {
        float t = (float)y / (float)H;
        s_sky[y] = PAPP_RGB565((int)(6 + 24 * t), (int)(4 + 12 * t), (int)(22 + 46 * t));
        for (int i = 0; i < NSTAR_ROW; i++) {
            bool on = frand() < 0.33f;
            s_star_x[y][i] = on ? (uint16_t)(frand() * (W - 1)) : 0xFFFF;
            int b = 90 + (int)(frand() * 165);
            s_star_c[y][i] = PAPP_RGB565(b, b, b + 30 > 255 ? 255 : b + 30);
        }
    }
}

static void sky_row(uint16_t *row, int y, int w, void *ctx)
{
    uint16_t c = s_sky[y];
    for (int x = 0; x < w; x++)
        row[x] = c;
    for (int i = 0; i < NSTAR_ROW; i++)
        if (s_star_x[y][i] < w)
            row[s_star_x[y][i]] = s_star_c[y][i];
}

static void draw_obj(e3d_t *e, const e3d_mesh *mesh, const e3d_mat *m, int32_t rgb, uint32_t flags)
{
    e3d_object o;
    e3d_object_init(&o, mesh);
    o.m = *m;
    o.color = rgb;
    o.flags = (flags & E3D_UNLIT) ? flags | E3D_NO_SHADOW : flags;   /* glowing things cast no shadow */
    e3d_draw(e, &o);
}

static void draw_ship(e3d_t *e, float x, float y, float z, float roll, float pitch)
{
    e3d_mat m = mat_at(x, y, z, roll, 0, pitch, 1.6f, 1.6f, 1.6f);
    draw_obj(e, m_ship, &m, 0xE8ECF8, 0);
    float pulse = 1.3f + 0.35f * sinf(g_t * 30.f);
    m = mat_at(x - 15.f, y + 7.f, z, 0, 0, 0, pulse, pulse, pulse);
    draw_obj(e, m_engine, &m, (int32_t)mix_rgb(0x40A0FF, 0xFFFFFF, pulse - 1.f), E3D_UNLIT);
}

static void draw_scene(e3d_t *e)
{
    e3d_camera *cam = e3d_cam(e);
    float sh = g_shake;
    g_cam_x = CAM_X;
    g_cam_y = CAM_Y;
    g_cam_z = g_sz * 0.1f;
    *cam = (e3d_camera){ .pos = { g_cam_x + (frand() - 0.5f) * sh, g_cam_y + (frand() - 0.5f) * sh,
                                  g_cam_z + (frand() - 0.5f) * sh },
                         .pitch = CAM_PITCH };
    if (g_icon)
        *cam = (e3d_camera){ .pos = { -25.f, 100.f, -55.f }, .pitch = -21.f, .heading = 11.f };

    float glow = 1.f + 0.5f * g_flash;
    *e3d_light_get(e) = (e3d_light){ .pos = { 620.f + 50.f * sinf(g_t * 0.31f), 430.f, -240.f + 70.f * cosf(g_t * 0.23f) },
                                     .r = fminf(255, 240 * glow), .g = fminf(255, 236 * glow), .b = fminf(255, 245 * glow) };

    e3d_draw(e, &o_floor);

    e3d_mat m;
    /* rails and the landing line */
    float pulse = 0.55f + 0.2f * sinf(g_t * 3.f);
    uint32_t rail = mix_rgb(0, 0x30D8FF, pulse);
    m = mat_at((FIELD_X0 + FIELD_X1) * 0.5f, 1.5f, -FZ - 12.f, 0, 0, 0, 1, 1, 1);
    draw_obj(e, m_rail_side, &m, (int32_t)rail, E3D_UNLIT);
    m = mat_at((FIELD_X0 + FIELD_X1) * 0.5f, 1.5f, FZ + 12.f, 0, 0, 0, 1, 1, 1);
    draw_obj(e, m_rail_side, &m, (int32_t)rail, E3D_UNLIT);
    m = mat_at(FIELD_X1 - 2.f, 1.5f, 0, 0, 0, 0, 1, 1, 1);
    draw_obj(e, m_rail_back, &m, (int32_t)rail, E3D_UNLIT);
    uint32_t line = g_f.hit_t > 0 && ((int)(g_f.hit_t * 12.f) & 1) ? 0xFF3020 : mix_rgb(0, 0xC03040, 0.55f);
    m = mat_at(LOSE_X - 4.f, 0.6f, 0, 0, 0, 0, 1, 1, 1);
    draw_obj(e, m_line, &m, (int32_t)line, E3D_UNLIT);

    /* bunkers */
    for (int b = 0; b < NBUNK; b++)
        for (int c = 0; c < BCOLS; c++) {
            int base = bcol_full(c) == 1 ? 1 : 0;     /* the arch: the inner columns are only the top block */
            for (int q = 0; q < g_bunk[b][c]; q++) {
                m = mat_at(BUNK_X, BLK_H * (float)(base + q), bcol_z(b, c), 0, 0, 0, 1, 1, 1);
                draw_obj(e, m_blk, &m, 0x48D068, 0);
            }
        }

    /* invaders */
    for (int r = 0; r < IROWS; r++) {
        int kind = kind_of_row[r];
        int32_t col = (int32_t)row_rgb(r);
        float x = inv_x(r);
        for (int c = 0; c < ICOLS; c++) {
            if (!g_f.alive[r][c])
                continue;
            float bob = (g_f.pose ^ (c & 1)) ? 1.6f : 0.f;
            float yaw = kind == KIND_SPIKE ? g_t * 120.f + (float)c * 30.f : (g_f.pose ? 12.f : -12.f);
            m = mat_at(x, INV_Y + bob, inv_z(c), 0, yaw, 0, INV_SC, INV_SC, INV_SC);
            draw_obj(e, m_inv[kind][g_f.pose], &m, col, kind == KIND_SPIKE ? E3D_FLAT : 0);
        }
    }

    /* saucer */
    if (g_ufo.alive) {
        m = mat_at(g_ufo.x, 26.f + 3.f * sinf(g_t * 4.f), g_ufo.z, 0, g_t * 200.f, 8.f * sinf(g_t * 3.f), 1.6f, 1.6f, 1.6f);
        draw_obj(e, m_ufo, &m, 0xF060E0, 0);
    }

    /* ship (blinks while the invaders hold fire after a respawn) */
    bool show = g_state == ST_PLAY || g_state == ST_CLEAR || g_state == ST_TITLE;
    if (g_state == ST_PLAY && g_inv_t > 0 && ((int)(g_inv_t * 10.f) & 1))
        show = false;
    if (show || g_icon)
        draw_ship(e, g_sx, SHIP_Y, g_sz, g_roll, g_pitch);

    /* shots */
    for (int i = 0; i < MAX_PSHOTS; i++)
        if (g_ps[i].alive) {
            m = mat_at(g_ps[i].x, SHOT_Y, g_ps[i].z, 0, 0, 0, 1.4f, 1.4f, 1.4f);
            draw_obj(e, m_pshot, &m, 0xB0FFFF, E3D_UNLIT);
        }
    for (int i = 0; i < MAX_ESHOTS; i++)
        if (g_es[i].alive) {
            m = mat_at(g_es[i].x, SHOT_Y, g_es[i].z, g_es[i].spin, 0, 0, 1.4f, 1.4f, 1.4f);
            draw_obj(e, m_eshot, &m, 0xFF5040, E3D_UNLIT);
        }

    /* sparks */
    for (int k = 0; k < MAX_PARTS; k++) {
        const spark_t *s = &g_part[k];
        if (s->life <= 0)
            continue;
        float t = s->life / s->life0;
        float sc = 1.5f * s->size * (t < 0.3f ? t / 0.3f : 1.f);
        m = mat_at(s->p[0], s->p[1], s->p[2], s->rot[0], s->rot[1], s->rot[2], sc, sc, sc);
        if (s->hot)
            draw_obj(e, m_part, &m, (int32_t)mix_rgb(0x801000, s->rgb, t * 1.4f), E3D_UNLIT | E3D_DOUBLE_SIDED);
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
                              : mix_rgb(0xFF8020, 0xB01800, (u - 0.6f) / 0.4f);
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
    for (int i = 0; i < 6; i++)
        fill(px, x + 5 - i, y + i, 1 + 2 * i, 1, col);
    fill(px, x + 1, y + 6, 9, 1, 0x4A69);
}

/* darken rows y0..y1 to 3/8, fading out over the top and bottom 6 rows */
static void shade(uint16_t *px, int y0, int y1)
{
    for (int y = y0 < 0 ? 0 : y0; y < y1 && y < H; y++) {
        int e = y - y0 < y1 - 1 - y ? y - y0 : y1 - 1 - y;
        if (e > 5) e = 5;
        int keep = 8 - e;
        if (keep < 3) keep = 3;
        uint16_t *r = px + y * W;
        for (int x = 0; x < W; x++) {
            uint16_t c = r[x];
            int rr = (c >> 11) * keep / 8, gg = ((c >> 5) & 63) * keep / 8, bb = (c & 31) * keep / 8;
            r[x] = (uint16_t)((rr << 11) | (gg << 5) | bb);
        }
    }
}

static void draw_hud(uint16_t *px)
{
    char line[48];
    snprintf(line, sizeof(line), "SCORE %06d", g_score);
    text_at(px, 4, 3, line, 0xFFFF, 1);
    snprintf(line, sizeof(line), "HI %06d", g_hiscore);
    text_at(px, 4, 12, line, 0xFFE0, 1);
    snprintf(line, sizeof(line), "WAVE %d", g_wave + 1);
    text_c(px, 3, line, 0xFFE0, 1);
    for (int i = 0; i < g_lives - (g_state == ST_DEAD ? 0 : 1) && i < 6; i++)
        ship_icon(px, W - 36 - i * 13, 3, 0x9FFF);
    text_at(px, W - 16, 3, "II", 0xC618, 1);

    for (int i = 0; i < MAX_POPS; i++)
        if (g_pop[i].t > 0) {
            float sx, sy;
            if (project(g_pop[i].p[0], g_pop[i].p[1], g_pop[i].p[2], &sx, &sy)) {
                snprintf(line, sizeof(line), "%d", g_pop[i].pts);
                text_at(px, (int)sx - 3 * (int)strlen(line), (int)sy, line, 0xFFE0, 1);
            }
        }

    switch (g_state) {
    case ST_TITLE:
        shade(px, 56, 176);
        text_c(px, 70, "VOIDMARCH", 0xFFFF, 4);
        text_c(px, 110, "HOLD THE LINE", 0xFE60, 1);
        if (((int)(g_t * 2.f)) & 1)
            text_c(px, 140, "PRESS A OR TAP TO PLAY", 0xFFFF, 1);
        text_c(px, 156, "PAD OR TOUCH MOVES   A FIRES", 0xA534, 1);
        break;
    case ST_PLAY:
        if (g_state_t < 2.f && g_inv_t > 0 && g_lives > 0) {
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


/* returns false to exit */
static bool pause_menu(void)
{
    static const char *const items[] = { "Resume", "Restart", "Exit" };
    papp_sem_take(g_pres_done, PAPP_WAIT_FOREVER);   /* nothing is being presented now */
    int r = papp_ui_menu("Voidmarch", items, 3, 0);
    papp_sem_give(g_pres_done);
    papp_input_flush();
    s_prev_btn = ~0u;          /* ignore the buttons still held */
    if (r == 1)
        new_game(0);
    return r != 2 && !papp_exit_requested();
}

static bool make_meshes(void)
{
    m_floor = e3d_mesh_floor(FIELD_X0, -FZ - 20.f, FIELD_X1, FZ + 20.f, 0, 14, 0x0E1630, 0x080D1E);
    if (!m_floor)
        return false;
    e3d_object_init(&o_floor, m_floor);
    o_floor.flags = E3D_FLOOR;
    m_rail_side = box((FIELD_X1 - FIELD_X0) * 0.5f, 1.5f, 1.8f);
    m_rail_back = box(1.8f, 1.5f, FZ + 13.8f);
    m_line = box(0.8f, 0.2f, FZ + 8.f);
    m_blk = bevel_box(BLK_HX, BLK_HZ, BLK_H - 0.5f, 1.8f, false);
    m_pshot = box(7.f, 1.5f, 1.5f);
    m_eshot = octa(5.f, 3.2f, 3.2f);
    m_part = tetra(2.4f);
    m_boom = e3d_mesh_sphere(10.f, 8, 6, 255, 255, 255);
    m_engine = e3d_mesh_sphere(2.6f, 6, 4, 255, 255, 255);
    {
        part_t p[3] = { { hull(), mat_at(0, 0, 0, 0, 0, 0, 1, 1, 1) },
                        { box(5.f, 1.f, 2.f), mat_at(-3.f, 3.f, -11.f, 0, 0, 0, 1, 1, 1) },
                        { box(5.f, 1.f, 2.f), mat_at(-3.f, 3.f, 11.f, 0, 0, 0, 1, 1, 1) } };
        m_ship = merge(p, 3, 20.f);
    }
    {
        part_t p[2] = { { e3d_mesh_sphere(12.f, 10, 4, 255, 255, 255), mat_at(0, 0, 0, 0, 0, 0, 1, 0.28f, 1) },
                        { e3d_mesh_sphere(6.f, 8, 4, 255, 255, 255), mat_at(0, 3.f, 0, 0, 0, 0, 1, 0.8f, 1) } };
        m_ufo = merge(p, 2, 40.f);
    }
    for (int pose = 0; pose < 2; pose++) {
        float s = pose ? 1.f : -1.f;
        /* spike: a gem with a ring of four points */
        {
            part_t p[3] = { { octa(5.f, 8.f, 5.f), mat_at(0, 0, 0, 0, 0, 0, 1, 1, 1) },
                            { octa(2.f, 1.6f, 9.f + s), mat_at(0, -1.f, 0, 0, 0, 0, 1, 1, 1) },
                            { octa(9.f - s, 1.6f, 2.f), mat_at(0, -1.f, 0, 0, 0, 0, 1, 1, 1) } };
            m_inv[KIND_SPIKE][pose] = merge(p, 3, 1.f);
        }
        /* wing: a body with two claws that open and close */
        {
            part_t p[5] = { { box(5.f, 4.f, 6.f), mat_at(0, 0, 0, 0, 0, 0, 1, 1, 1) },
                            { box(3.f, 1.5f, 4.f), mat_at(0, 1.f, -8.f, 30.f * s, 0, 0, 1, 1, 1) },
                            { box(3.f, 1.5f, 4.f), mat_at(0, 1.f, 8.f, -30.f * s, 0, 0, 1, 1, 1) },
                            { box(1.f, 3.f, 1.f), mat_at(1.f, 5.f, -3.f, 0, 0, 15.f * s, 1, 1, 1) },
                            { box(1.f, 3.f, 1.f), mat_at(1.f, 5.f, 3.f, 0, 0, -15.f * s, 1, 1, 1) } };
            m_inv[KIND_WING][pose] = merge(p, 5, 30.f);
        }
        /* dome: a cap on four legs that step */
        {
            part_t p[5] = { { e3d_mesh_sphere(8.f, 8, 4, 255, 255, 255), mat_at(0, 1.f, 0, 0, 0, 0, 1, 0.6f, 1) },
                            { box(1.2f, 3.5f, 1.2f), mat_at(4.f + s, -4.f, -6.f, 0, 0, 0, 1, 1, 1) },
                            { box(1.2f, 3.5f, 1.2f), mat_at(4.f - s, -4.f, 6.f, 0, 0, 0, 1, 1, 1) },
                            { box(1.2f, 3.5f, 1.2f), mat_at(-4.f - s, -4.f, -6.f, 0, 0, 0, 1, 1, 1) },
                            { box(1.2f, 3.5f, 1.2f), mat_at(-4.f + s, -4.f, 6.f, 0, 0, 0, 1, 1, 1) } };
            m_inv[KIND_DOME][pose] = merge(p, 5, 35.f);
        }
    }
    if (!m_rail_side || !m_rail_back || !m_line || !m_blk || !m_pshot || !m_eshot || !m_part || !m_boom || !m_engine ||
        !m_ship || !m_ufo)
        return false;
    for (int k = 0; k < NKINDS; k++)
        if (!m_inv[k][0] || !m_inv[k][1])
            return false;
    return true;
}

static void free_meshes(void)
{
    for (int k = 0; k < NKINDS; k++) {
        e3d_mesh_free(m_inv[k][0]);
        e3d_mesh_free(m_inv[k][1]);
    }
    e3d_mesh *all[] = { m_floor, m_rail_side, m_rail_back, m_line, m_blk, m_pshot, m_eshot, m_part, m_boom, m_engine,
                        m_ship, m_ufo };
    for (unsigned i = 0; i < sizeof(all) / sizeof(all[0]); i++)
        e3d_mesh_free(all[i]);
}

int main(int argc, char **argv)
{
    int start_wave = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "auto"))
            g_auto = true;
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
    papp_thread_cfg_t pcfg = { .name = "vm_present", .stack_size = 4096, .prio = PAPP_PRIO_HIGH, .core = 0 };
    papp_thread_t *pres = (g_surf && fb[0] && fb[1] && g_pres_go && g_pres_done)
                              ? papp_thread_create(presenter_main, NULL, &pcfg) : NULL;
    e3d_t *e = e3d_create(W, H, 2);
    if (!pres || !e || !make_meshes()) {
        papp_log("voidmarch: out of memory\n");
        return 1;
    }
    make_sky();
    e3d_set_focal(e, g_icon ? 230.f : FOCAL);
    e3d_set_background_rows(e, sky_row, NULL);
    e3d_params *par = e3d_params_get(e);
    par->shadows = true;
    par->shadow_y = 0.5f;
    par->shadow_dark = 0.40f;
    par->ambient = 0.44f;
    par->diffuse = 0.8f;
    par->specular = 0.35f;
    par->shininess = 16.f;

    g_snd_lock = papp_mutex_create(false);
    papp_audio_cfg_t acfg = { .rate = SR, .channels = 1, .format = PAPP_AUDIO_S16, .latency_ms = 60 };
    g_snd = g_snd_lock ? papp_audio_open(&acfg) : NULL;
    papp_thread_cfg_t acfg_t = { .name = "vm_audio", .stack_size = 4096, .prio = PAPP_PRIO_HIGH, .core = 1 };
    papp_thread_t *aud = g_snd ? papp_thread_create(audio_main, NULL, &acfg_t) : NULL;
    if (!aud && g_snd) {
        papp_audio_close(g_snd);
        g_snd = NULL;
    }

    hiscore_io(false);
    load_wave(start_wave);
    g_wave = start_wave;
    g_lives = 3;
    g_state = ST_TITLE;
    if (g_auto || g_icon)
        new_game(start_wave);
    papp_log("voidmarch: running%s, wave %d\n", g_auto ? " (auto)" : "", start_wave + 1);

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

        actions_t act = read_input(g_surf, dt);
        if (act.pause && g_state != ST_TITLE) {
            run = pause_menu();
            last = papp_time_us();
            continue;
        }
        bool go = act.start || act.touch_tap;
        if (g_state == ST_TITLE) {
            g_sz = 90.f * sinf(g_t * 0.8f);
            g_sx = SHIP_X0 + 20.f + 20.f * sinf(g_t * 0.5f);
            if (go)
                new_game(start_wave);
            else if (act.pause)
                run = pause_menu();
        } else if (g_state == ST_OVER && go && g_state_t > 1.f)
            new_game(0);
        if (g_auto)
            auto_play(dt, &act);
        if (act.fire && g_state == ST_PLAY)
            fire();
        if (g_icon) {
            /* a still: an invader blown up a moment ago, a shot on its way */
            static int icon_n;
            if (icon_n == 0) {
                g_f.x = 240.f;
                g_f.z = 0;
                kill_invader(0, 3);
                reset_bunkers();
                g_ps[0] = (pshot_t){ 175.f, -30.f, true };
                g_sx = 100.f;
                g_sz = -40.f;
                g_roll = -10.f;
            }
            dt = icon_n++ < 6 ? 0.02f : 0;
            g_inv_t = 0;
            g_ufo.alive = false;
        }
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
            papp_log("voidmarch: %.1f fps, wave %d, left %d, lives %d, score %d, ship %.0f/%.0f, objs %d, tris %d + %d "
                     "shadow, geom %d us, raster %d us, underruns %lu\n",
                     (double)fps, g_wave + 1, g_f.left, g_lives, g_score, (double)g_sx, (double)g_sz, st->objects,
                     st->tris_drawn, st->shadow_tris, st->t_geom_us, st->t_raster_us, (unsigned long)as.underruns);
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
