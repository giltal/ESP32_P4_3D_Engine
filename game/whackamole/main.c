/*
 * Whack-a-Mole 3D — a touch whack-a-mole game on the e3d engine.
 *
 * Twenty-five holes (5 x 5) in a wooden board on the lawn. Moles pop up and
 * duck back down; tap them before they hide. The round lasts 60 seconds and
 * gets faster as it goes, with more moles up at once near the end (up to 6).
 * Golden moles are worth five times as much, and hits in a row build a combo
 * multiplier (x2 at 5, x3 at 10, x4 at 15). A tap that misses, or a mole that
 * gets away, ends the combo. Several fingers at once work.
 *
 * Controls:
 *   touch                tap a mole to whack it; tap to start a round
 *   A / START            start a round (title and game-over screens)
 *   B, or the X button   exit (the button is in the bottom-right corner)
 *
 * The best score is kept in /sd/data/whackamole/best.txt.
 *
 * Arguments: `auto` = the computer plays (attract mode, for testing).
 */
#include "papp/papp.h"
#include "e3d.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

PAPP_MANIFEST(.id = "michael.whackamole", .name = "Whack-a-Mole 3D", .version = PAPP_APP_VERSION,
              .main_stack = 16384);

#define W 400
#define H 240

/* ── The board (world units; x = depth away from the camera, z = right, y = up) ── */
#define GRID       5
#define CELL       84.f         /* hole spacing */
#define CX         470.f        /* depth of the board's centre */
#define BOARD_Y    80.f         /* the board's top */
#define BORDER     24.f         /* board edge around the cells */
#define HOLE_R     25.f
#define RIM_W      6.f
#define RIM_H      4.f
#define PIPE_Y     4.f          /* bottom of each hole */
#define SEG        16           /* segments round a hole (a multiple of 8: the cell corners) */

#define MOLE_S      0.8f                        /* the mole model's scale */
#define MOLE_UP_Y   (BOARD_Y - 22.f * MOLE_S)   /* the mole's base when it is all the way up */
#define MOLE_DOWN_Y (PIPE_Y)                    /* ... and when it is hidden */
#define MOLE_TOP    (76.f * MOLE_S)             /* the top of its head above its base */

#define ROUND_S    60.f

/* Camera */
#define CAM_X     (CX - 400.f)
#define CAM_Y      600.f
#define FOCAL      330.f

#define TAP_R      34.f         /* how far (in frame pixels) a tap may land from a mole */

/* The exit button, bottom-right (frame pixels). Taps on it never whack. */
#define EXIT_W     22
#define EXIT_H     18
#define EXIT_X     (W - EXIT_W - 3)
#define EXIT_Y     (H - EXIT_H - 3)
#define EXIT_PAD   4            /* the touch area reaches a little past the drawn button */

static float hole_x(int r) { return CX + (float)(r - (GRID - 1) / 2) * CELL; }   /* row 0 is the near row */
static float hole_z(int c) { return (float)(c - (GRID - 1) / 2) * CELL; }

/* ── Sound: a few synthesised voices mixed on an audio thread (as in Shardwall) ── */
#define SR 22050
enum { WAVE_SQUARE, WAVE_TRI, WAVE_NOISE };

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

/* ── Mesh builder: parts added one by one, each triangle turned to face a given way ── */
typedef struct {
    float (*p)[3];
    e3d_tri *t;
    int nv, nt, cap_v, cap_t;
} mb_t;

static bool mb_init(mb_t *b, int cap_v, int cap_t)
{
    b->p = malloc(sizeof(float) * 3 * (size_t)cap_v);
    b->t = calloc((size_t)cap_t, sizeof(e3d_tri));
    b->nv = b->nt = 0;
    b->cap_v = cap_v;
    b->cap_t = cap_t;
    return b->p && b->t;
}

static int mb_v(mb_t *b, float x, float y, float z)
{
    if (b->nv >= b->cap_v)
        return 0;
    b->p[b->nv][0] = x;
    b->p[b->nv][1] = y;
    b->p[b->nv][2] = z;
    return b->nv++;
}

/* A triangle whose outward normal, (v0 - v1) x (v2 - v1), points along dir. */
static void mb_tri(mb_t *b, int a, int c1, int c2, uint32_t rgb, const float dir[3])
{
    if (b->nt >= b->cap_t)
        return;
    float *p0 = b->p[a], *p1 = b->p[c1], *p2 = b->p[c2];
    float u[3] = { p0[0] - p1[0], p0[1] - p1[1], p0[2] - p1[2] };
    float v[3] = { p2[0] - p1[0], p2[1] - p1[1], p2[2] - p1[2] };
    float n[3] = { u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0] };
    e3d_tri *t = &b->t[b->nt++];
    t->v[0] = (uint16_t)a;
    if (n[0] * dir[0] + n[1] * dir[1] + n[2] * dir[2] >= 0) {
        t->v[1] = (uint16_t)c1;
        t->v[2] = (uint16_t)c2;
    } else {
        t->v[1] = (uint16_t)c2;
        t->v[2] = (uint16_t)c1;
    }
    t->r = (uint8_t)(rgb >> 16);
    t->g = (uint8_t)(rgb >> 8);
    t->b = (uint8_t)rgb;
}

/* Facing away from (in = false) or towards (in = true) the point ctr. */
static void mb_tri_from(mb_t *b, int a, int c1, int c2, uint32_t rgb, const float ctr[3], bool in)
{
    float d[3];
    for (int k = 0; k < 3; k++) {
        d[k] = (b->p[a][k] + b->p[c1][k] + b->p[c2][k]) / 3.f - ctr[k];
        if (in)
            d[k] = -d[k];
    }
    mb_tri(b, a, c1, c2, rgb, d);
}

static void mb_quad(mb_t *b, int a, int c1, int c2, int c3, uint32_t rgb, const float dir[3])
{
    mb_tri(b, a, c1, c2, rgb, dir);
    mb_tri(b, a, c2, c3, rgb, dir);
}

static void mb_ellipsoid(mb_t *b, float cx, float cy, float cz, float rx, float ry, float rz,
                         int slices, int stacks, uint32_t rgb)
{
    const float ctr[3] = { cx, cy, cz };
    int top = mb_v(b, cx, cy + ry, cz);
    int ring0 = b->nv;
    for (int i = 1; i < stacks; i++) {
        float ph = (float)M_PI * (float)i / (float)stacks;
        for (int j = 0; j < slices; j++) {
            float th = 2.f * (float)M_PI * (float)j / (float)slices;
            mb_v(b, cx + rx * sinf(ph) * cosf(th), cy + ry * cosf(ph), cz + rz * sinf(ph) * sinf(th));
        }
    }
    int bot = mb_v(b, cx, cy - ry, cz);
    for (int j = 0; j < slices; j++) {
        int j1 = (j + 1) % slices;
        mb_tri_from(b, top, ring0 + j, ring0 + j1, rgb, ctr, false);
        int last = ring0 + (stacks - 2) * slices;
        mb_tri_from(b, bot, last + j, last + j1, rgb, ctr, false);
        for (int i = 0; i < stacks - 2; i++) {
            int a = ring0 + i * slices;
            int n = a + slices;
            mb_tri_from(b, a + j, a + j1, n + j1, rgb, ctr, false);
            mb_tri_from(b, a + j, n + j1, n + j, rgb, ctr, false);
        }
    }
}

/* A tube round the y axis at (cx, cz) from y0 to y1; faces outwards, or inwards when in. */
static void mb_tube(mb_t *b, float cx, float cz, float y0, float y1, float r, int seg, uint32_t rgb, bool in)
{
    int base = b->nv;
    for (int j = 0; j < seg; j++) {
        float th = 2.f * (float)M_PI * (float)j / (float)seg;
        mb_v(b, cx + r * cosf(th), y0, cz + r * sinf(th));
        mb_v(b, cx + r * cosf(th), y1, cz + r * sinf(th));
    }
    for (int j = 0; j < seg; j++) {
        int j1 = (j + 1) % seg;
        float th = 2.f * (float)M_PI * ((float)j + 0.5f) / (float)seg;
        float d[3] = { cosf(th), 0, sinf(th) };
        if (in) {
            d[0] = -d[0];
            d[2] = -d[2];
        }
        mb_quad(b, base + 2 * j, base + 2 * j + 1, base + 2 * j1 + 1, base + 2 * j1, rgb, d);
    }
}

/* A flat ring (or disc, r0 = 0) at height y, facing up or down. */
static void mb_ring(mb_t *b, float cx, float cz, float y, float r0, float r1, int seg, uint32_t rgb, bool up)
{
    const float d[3] = { 0, up ? 1.f : -1.f, 0 };
    int base = b->nv;
    if (r0 <= 0) {
        int c = mb_v(b, cx, y, cz);
        for (int j = 0; j < seg; j++) {
            float th = 2.f * (float)M_PI * (float)j / (float)seg;
            mb_v(b, cx + r1 * cosf(th), y, cz + r1 * sinf(th));
        }
        for (int j = 0; j < seg; j++)
            mb_tri(b, c, c + 1 + j, c + 1 + (j + 1) % seg, rgb, d);
        return;
    }
    for (int j = 0; j < seg; j++) {
        float th = 2.f * (float)M_PI * (float)j / (float)seg;
        mb_v(b, cx + r0 * cosf(th), y, cz + r0 * sinf(th));
        mb_v(b, cx + r1 * cosf(th), y, cz + r1 * sinf(th));
    }
    for (int j = 0; j < seg; j++) {
        int j1 = (j + 1) % seg;
        mb_quad(b, base + 2 * j, base + 2 * j + 1, base + 2 * j1 + 1, base + 2 * j1, rgb, d);
    }
}

static e3d_mesh *mb_finish(mb_t *b, float crease)
{
    e3d_mesh *m = e3d_mesh_create(b->nv, b->nt);
    if (m) {
        memcpy(m->pos, b->p, sizeof(float) * 3 * (size_t)b->nv);
        for (int i = 0; i < b->nt; i++) {
            m->tris[i].v[0] = b->t[i].v[0];
            m->tris[i].v[1] = b->t[i].v[1];
            m->tris[i].v[2] = b->t[i].v[2];
            m->tris[i].r = b->t[i].r;
            m->tris[i].g = b->t[i].g;
            m->tris[i].b = b->t[i].b;
        }
        e3d_mesh_finish(m, crease);
    }
    free(b->p);
    free(b->t);
    return m;
}


/* ── The meshes ──────────────────────────────────────────────────────────── */
#define C_TOP    0x78B848u   /* board top: turf */
#define C_SIDE   0x9A6234u   /* board sides: wood */
#define C_PIPE   0x3A2616u
#define C_PIT    0x120C08u
#define C_RIM    0x8C6038u

static e3d_mesh *m_ground, *m_board, *m_mole, *m_gold, *m_spark;

/* The board: a turf top with a round hole in each cell, a raised lip round each hole,
 * a dark pipe under it, and wooden sides down to the lawn. */
static e3d_mesh *make_board(void)
{
    mb_t b;
    if (!mb_init(&b, 9000, 9000))
        return NULL;
    const float up[3] = { 0, 1, 0 };
    float half = CELL * 0.5f;
    for (int r = 0; r < GRID; r++)
        for (int c = 0; c < GRID; c++) {
            float hx = hole_x(r), hz = hole_z(c);
            int base = b.nv;
            for (int j = 0; j < SEG; j++) {
                float th = 2.f * (float)M_PI * (float)j / (float)SEG;
                float cs = cosf(th), sn = sinf(th);
                float t = half / fmaxf(fabsf(cs), fabsf(sn));   /* out to the cell's square edge */
                mb_v(&b, hx + (HOLE_R + RIM_W) * cs, BOARD_Y, hz + (HOLE_R + RIM_W) * sn);
                mb_v(&b, hx + t * cs, BOARD_Y, hz + t * sn);
            }
            for (int j = 0; j < SEG; j++) {
                int j1 = (j + 1) % SEG;
                mb_quad(&b, base + 2 * j, base + 2 * j + 1, base + 2 * j1 + 1, base + 2 * j1, C_TOP, up);
            }
            mb_tube(&b, hx, hz, BOARD_Y, BOARD_Y + RIM_H, HOLE_R + RIM_W, SEG, C_RIM, false);
            mb_ring(&b, hx, hz, BOARD_Y + RIM_H, HOLE_R, HOLE_R + RIM_W, SEG, C_RIM, true);
            mb_tube(&b, hx, hz, PIPE_Y, BOARD_Y + RIM_H, HOLE_R, SEG, C_PIPE, true);
            mb_ring(&b, hx, hz, PIPE_Y, 0, HOLE_R, SEG, C_PIT, true);
        }
    /* the border round the cells, and the sides down to the lawn */
    float ix0 = hole_x(0) - half, ix1 = hole_x(GRID - 1) + half;
    float iz0 = hole_z(0) - half, iz1 = hole_z(GRID - 1) + half;
    float ox0 = ix0 - BORDER, ox1 = ix1 + BORDER, oz0 = iz0 - BORDER, oz1 = iz1 + BORDER;
    int o[4] = { mb_v(&b, ox0, BOARD_Y, oz0), mb_v(&b, ox1, BOARD_Y, oz0), mb_v(&b, ox1, BOARD_Y, oz1),
                 mb_v(&b, ox0, BOARD_Y, oz1) };
    int in[4] = { mb_v(&b, ix0, BOARD_Y, iz0), mb_v(&b, ix1, BOARD_Y, iz0), mb_v(&b, ix1, BOARD_Y, iz1),
                  mb_v(&b, ix0, BOARD_Y, iz1) };
    for (int k = 0; k < 4; k++)
        mb_quad(&b, o[k], o[(k + 1) % 4], in[(k + 1) % 4], in[k], C_SIDE, up);
    int g[4] = { mb_v(&b, ox0, 0, oz0), mb_v(&b, ox1, 0, oz0), mb_v(&b, ox1, 0, oz1), mb_v(&b, ox0, 0, oz1) };
    const float out[4][3] = { { 0, 0, -1 }, { 1, 0, 0 }, { 0, 0, 1 }, { -1, 0, 0 } };
    for (int k = 0; k < 4; k++)
        mb_quad(&b, o[k], o[(k + 1) % 4], g[(k + 1) % 4], g[k], C_SIDE, out[k]);
    return mb_finish(&b, 35.f);
}

/* The mole, base at y = 0, looking towards -x (the camera), scaled by s. */
static e3d_mesh *make_mole(uint32_t fur, uint32_t paw, float s)
{
    mb_t b;
    if (!mb_init(&b, 700, 1400))
        return NULL;
    mb_ellipsoid(&b, 0, 36 * s, 0, 25 * s, 38 * s, 25 * s, 12, 8, fur);                    /* body */
    mb_ellipsoid(&b, -21 * s, 42 * s, 0, 12 * s, 9 * s, 13 * s, 8, 5, 0xE8B890u);           /* snout */
    mb_ellipsoid(&b, -33 * s, 45 * s, 0, 5 * s, 5 * s, 5 * s, 6, 4, 0xF06080u);             /* nose */
    mb_ellipsoid(&b, -27 * s, 32 * s, 0, 3 * s, 5 * s, 6 * s, 6, 4, 0xFFFFF0u);             /* teeth */
    for (int k = -1; k <= 1; k += 2) {
        mb_ellipsoid(&b, -18 * s, 57 * s, 10 * s * k, 4.5f * s, 5 * s, 4.5f * s, 6, 4, 0x101010u);   /* eyes */
        mb_ellipsoid(&b, -2 * s, 70 * s, 17 * s * k, 4 * s, 7 * s, 7 * s, 6, 4, paw);                /* ears */
        mb_ellipsoid(&b, -27 * s, 25 * s, 19 * s * k, 9 * s, 5 * s, 8 * s, 6, 4, paw);               /* paws */
    }
    return mb_finish(&b, 60.f);
}

/* A little star burst piece: an octahedron. */
static e3d_mesh *make_spark(void)
{
    mb_t b;
    if (!mb_init(&b, 6, 8))
        return NULL;
    const float c[3] = { 0, 0, 0 };
    int v[6] = { mb_v(&b, 4, 0, 0), mb_v(&b, -4, 0, 0), mb_v(&b, 0, 4, 0),
                 mb_v(&b, 0, -4, 0), mb_v(&b, 0, 0, 4), mb_v(&b, 0, 0, -4) };
    for (int i = 0; i < 2; i++)
        for (int j = 2; j < 4; j++)
            for (int k = 4; k < 6; k++)
                mb_tri_from(&b, v[i], v[j], v[k], 0xFFFFFFu, c, false);
    return mb_finish(&b, 10.f);
}

/* ── Game state ──────────────────────────────────────────────────────────── */
enum { M_DOWN, M_RISE, M_UP, M_FALL, M_HIT };

typedef struct {
    int   state;
    float h;          /* 0 = hidden .. 1 = all the way up */
    float t;          /* time left in M_UP, or the squash time in M_HIT */
    float up_time;
    bool  gold;
} mole_t;

typedef struct {
    float p[3], v[3], rot, life;
    int32_t color;
    bool  alive;
} spark_t;

typedef struct {
    float p[3], life;
    char  text[16];
    uint16_t color;
} popup_t;

typedef struct {       /* a ring on the screen where a tap landed */
    float x, y, life;
    bool  hit;
} tapfx_t;

enum { G_TITLE, G_PLAY, G_OVER };

#define MAX_SPARKS 48
#define MAX_POPUPS 10
#define MAX_TAPS   8

static mole_t  g_mole[GRID][GRID];
static spark_t g_spark[MAX_SPARKS];
static popup_t g_popup[MAX_POPUPS];
static tapfx_t g_tapfx[MAX_TAPS];
static int     g_game = G_TITLE;
static float   g_time_left, g_spawn_t, g_t, g_shake, g_over_t;
static int     g_score, g_best, g_combo, g_hits, g_taps, g_escaped;
static bool    g_new_best, g_auto;

static float frand(void) { return (float)rand() / (float)RAND_MAX; }

/* ── Best score ──────────────────────────────────────────────────────────── */
#define BEST_PATH "/sd/data/whackamole/best.txt"

static void best_load(void)
{
    FILE *f = fopen(BEST_PATH, "r");
    if (f) {
        if (fscanf(f, "%d", &g_best) != 1)
            g_best = 0;
        fclose(f);
    }
}

static void best_save(void)
{
    mkdir("/sd/data", 0777);
    mkdir("/sd/data/whackamole", 0777);
    FILE *f = fopen(BEST_PATH, "w");
    if (f) {
        fprintf(f, "%d\n", g_best);
        fclose(f);
    } else
        papp_log("whackamole: could not save the best score\n");
}

/* ── Projection (for touch and the score pop-ups) ────────────────────────── */
static bool project(const e3d_camera *cam, const float p[3], float *sx, float *sy)
{
    float h = cam->heading * (float)M_PI / 180.f, pt = cam->pitch * (float)M_PI / 180.f;
    float f[3] = { cosf(pt) * cosf(h), sinf(pt), cosf(pt) * sinf(h) };
    float r[3] = { -sinf(h), 0, cosf(h) };
    float u[3] = { r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0] };
    float d[3] = { p[0] - cam->pos[0], p[1] - cam->pos[1], p[2] - cam->pos[2] };
    float z = d[0] * f[0] + d[1] * f[1] + d[2] * f[2];
    if (z < 1.f)
        return false;
    *sx = W * 0.5f + FOCAL * (d[0] * r[0] + d[1] * r[1] + d[2] * r[2]) / z;
    *sy = H * 0.5f - FOCAL * (d[0] * u[0] + d[1] * u[1] + d[2] * u[2]) / z;
    return true;
}

/* ── Game logic ──────────────────────────────────────────────────────────── */
static void reset_round(void)
{
    memset(g_mole, 0, sizeof(g_mole));
    memset(g_spark, 0, sizeof(g_spark));
    memset(g_popup, 0, sizeof(g_popup));
    g_time_left = ROUND_S;
    g_spawn_t = 0.8f;
    g_score = g_combo = g_hits = g_taps = g_escaped = 0;
    g_new_best = false;
}

static void add_popup(float x, float y, float z, const char *text, uint16_t color)
{
    int best = 0;
    for (int i = 0; i < MAX_POPUPS; i++)
        if (g_popup[i].life < g_popup[best].life)
            best = i;
    popup_t *p = &g_popup[best];
    p->p[0] = x;
    p->p[1] = y;
    p->p[2] = z;
    p->life = 0.9f;
    p->color = color;
    snprintf(p->text, sizeof(p->text), "%s", text);
}

static void add_tapfx(float x, float y, bool hit)
{
    int best = 0;
    for (int i = 0; i < MAX_TAPS; i++)
        if (g_tapfx[i].life < g_tapfx[best].life)
            best = i;
    g_tapfx[best] = (tapfx_t){ .x = x, .y = y, .life = 0.25f, .hit = hit };
}

static void burst(float x, float y, float z, int n, int32_t color, float speed)
{
    for (int i = 0; i < MAX_SPARKS && n > 0; i++) {
        spark_t *s = &g_spark[i];
        if (s->alive)
            continue;
        float a = frand() * 2.f * (float)M_PI, sp = (70.f + frand() * 100.f) * speed;
        *s = (spark_t){ .p = { x, y, z }, .v = { cosf(a) * sp, (140.f + frand() * 140.f) * speed, sinf(a) * sp },
                        .rot = frand() * 360.f, .life = 0.45f + frand() * 0.3f, .color = color, .alive = true };
        n--;
    }
}

static int moles_up(void)
{
    int n = 0;
    for (int r = 0; r < GRID; r++)
        for (int c = 0; c < GRID; c++)
            n += g_mole[r][c].state != M_DOWN;
    return n;
}

static void spawn_mole(float diff, bool quiet)
{
    int free_r[GRID * GRID], free_c[GRID * GRID], n = 0;
    for (int r = 0; r < GRID; r++)
        for (int c = 0; c < GRID; c++)
            if (g_mole[r][c].state == M_DOWN) {
                free_r[n] = r;
                free_c[n] = c;
                n++;
            }
    if (!n)
        return;
    int k = rand() % n;
    mole_t *m = &g_mole[free_r[k]][free_c[k]];
    m->state = M_RISE;
    m->h = 0;
    m->gold = frand() < 0.08f;
    m->up_time = (1.15f - 0.55f * diff) * (0.8f + 0.4f * frand());
    if (m->gold)
        m->up_time *= 0.75f;
    if (!quiet)
        sfx(m->gold ? 700.f : 420.f, m->gold ? 1400.f : 820.f, 70, WAVE_TRI, 0.16f);
}

/* the mole's base height */
static float mole_y(const mole_t *m)
{
    float h = m->h;
    h = h * h * (3.f - 2.f * h);   /* smooth in and out */
    return MOLE_DOWN_Y + (MOLE_UP_Y - MOLE_DOWN_Y) * h;
}

static void update_moles(float dt, bool playing, bool spawn)
{
    float diff = playing ? 1.f - g_time_left / ROUND_S : 0.3f;
    /* new moles: the gap between them shrinks and more may be up at once */
    g_spawn_t -= dt;
    int max_up = 2 + (int)(diff * 4.5f);
    if (spawn && g_spawn_t <= 0) {
        if (moles_up() < max_up)
            spawn_mole(diff, !playing);
        g_spawn_t = (0.8f - 0.5f * diff) * (0.6f + 0.6f * frand());
    }
    for (int r = 0; r < GRID; r++)
        for (int c = 0; c < GRID; c++) {
            mole_t *m = &g_mole[r][c];
            switch (m->state) {
            case M_RISE:
                m->h += dt / 0.16f;
                if (m->h >= 1.f) {
                    m->h = 1.f;
                    m->state = M_UP;
                    m->t = m->up_time;
                }
                break;
            case M_UP:
                m->t -= dt;
                if (m->t <= 0)
                    m->state = M_FALL;
                break;
            case M_FALL:
                m->h -= dt / 0.2f;
                if (m->h <= 0) {
                    m->h = 0;
                    m->state = M_DOWN;
                    if (playing) {   /* got away */
                        g_escaped++;
                        g_combo = 0;
                    }
                }
                break;
            case M_HIT:
                m->t += dt;
                if (m->t > 0.28f) {
                    m->h -= dt / 0.12f;
                    if (m->h <= 0) {
                        m->h = 0;
                        m->state = M_DOWN;
                    }
                }
                break;
            }
        }
}

static bool hittable(const mole_t *m)
{
    return (m->state == M_RISE || m->state == M_UP || m->state == M_FALL) && m->h > 0.3f;
}

/* Which hole a tap at (sx, sy) is for: the mole closest to the finger (its head or body,
 * whichever is nearer), or else the closest hole. Returns false if it is not near any. */
static bool pick(const e3d_camera *cam, float sx, float sy, int *pr, int *pc, bool *on_mole)
{
    float best = 1e9f;
    *pr = *pc = -1;
    *on_mole = false;
    for (int r = 0; r < GRID; r++)
        for (int c = 0; c < GRID; c++) {
            mole_t *m = &g_mole[r][c];
            bool up = hittable(m);
            float y0 = up ? mole_y(m) : BOARD_Y;
            /* along the mole's height (or just the hole's mouth): the nearest of a few points */
            float d2 = 1e9f;
            int n = up ? 3 : 1;
            for (int k = 0; k < n; k++) {
                float p[3] = { hole_x(r), up ? y0 + MOLE_TOP * (0.35f + 0.3f * (float)k) : y0, hole_z(c) }, x, y;
                if (!project(cam, p, &x, &y))
                    continue;
                float dx = x - sx, dy = y - sy;
                d2 = fminf(d2, dx * dx + dy * dy);
            }
            float lim = up ? TAP_R : TAP_R * 0.7f;
            if (d2 > lim * lim)
                continue;
            if (up)
                d2 *= 0.5f;   /* a mole wins over an empty hole beside it */
            if (d2 < best) {
                best = d2;
                *pr = r;
                *pc = c;
                *on_mole = up;
            }
        }
    return *pr >= 0;
}

/* A tap: hit the mole under it, or miss. */
static void whack(const e3d_camera *cam, float sx, float sy, bool playing)
{
    int r, c;
    bool on_mole;
    if (playing)
        g_taps++;
    if (!pick(cam, sx, sy, &r, &c, &on_mole) || !on_mole) {
        add_tapfx(sx, sy, false);
        sfx(150.f, 60.f, 80, WAVE_NOISE, 0.22f);
        if (r >= 0)
            burst(hole_x(r), BOARD_Y + RIM_H, hole_z(c), 4, 0x8C6038, 0.6f);   /* a puff of dirt */
        if (playing && g_combo > 0 && r >= 0)
            add_popup(hole_x(r), BOARD_Y + 50.f, hole_z(c), "MISS", PAPP_RGB565(255, 90, 80));
        if (playing)
            g_combo = 0;
        return;
    }
    mole_t *m = &g_mole[r][c];
    float x = hole_x(r), z = hole_z(c);
    m->state = M_HIT;
    m->t = 0;
    add_tapfx(sx, sy, true);
    burst(x - 8.f, mole_y(m) + MOLE_TOP, z, m->gold ? 14 : 8, m->gold ? 0xFFD040 : 0xFFF0A0, 1.f);
    g_shake = 0.12f;
    sfx(240.f, 80.f, 130, WAVE_SQUARE, 0.3f);
    sfx(1500.f, 500.f, 60, WAVE_NOISE, 0.18f);
    if (!playing)
        return;
    g_combo++;
    g_hits++;
    int mult = 1 + (g_combo >= 15 ? 3 : g_combo / 5);
    int pts = (m->gold ? 50 : 10) * mult;
    g_score += pts;
    char t[16];
    snprintf(t, sizeof(t), "+%d", pts);
    add_popup(x, BOARD_Y + 95.f, z, t, m->gold ? PAPP_RGB565(255, 220, 60) : 0xFFFF);
    if (m->gold) {
        sfx_at(880.f, 880.f, 70, WAVE_SQUARE, 0.14f, 60);
        sfx_at(1175.f, 1175.f, 70, WAVE_SQUARE, 0.14f, 130);
        sfx_at(1760.f, 1760.f, 120, WAVE_SQUARE, 0.14f, 200);
    }
    if (g_combo == 5 || g_combo == 10 || g_combo == 15)
        sfx_at(660.f, 1320.f, 150, WAVE_TRI, 0.16f, 80);
}

static void update_fx(float dt)
{
    for (int i = 0; i < MAX_SPARKS; i++) {
        spark_t *s = &g_spark[i];
        if (!s->alive)
            continue;
        s->life -= dt;
        s->v[1] -= 650.f * dt;
        for (int k = 0; k < 3; k++)
            s->p[k] += s->v[k] * dt;
        s->rot += 600.f * dt;
        if (s->life <= 0)
            s->alive = false;
    }
    for (int i = 0; i < MAX_POPUPS; i++)
        if (g_popup[i].life > 0) {
            g_popup[i].life -= dt;
            g_popup[i].p[1] += 50.f * dt;
        }
    for (int i = 0; i < MAX_TAPS; i++)
        if (g_tapfx[i].life > 0)
            g_tapfx[i].life -= dt;
    if (g_shake > 0)
        g_shake -= dt;
}

/* the computer plays: tap the mole that will hide soonest, after a human-ish pause */
static float s_auto_wait;
static void auto_play(const e3d_camera *cam, float dt, bool playing)
{
    s_auto_wait -= dt;
    if (s_auto_wait > 0)
        return;
    int br = -1, bc = -1;
    float best = 1e9f;
    for (int r = 0; r < GRID; r++)
        for (int c = 0; c < GRID; c++) {
            mole_t *m = &g_mole[r][c];
            if (!hittable(m) || m->state == M_RISE)
                continue;
            float left = m->state == M_UP ? m->t : 0.f;
            if (left < best) {
                best = left;
                br = r;
                bc = c;
            }
        }
    if (br < 0)
        return;
    mole_t *m = &g_mole[br][bc];
    float p[3] = { hole_x(br), mole_y(m) + MOLE_TOP * 0.6f, hole_z(bc) }, x, y;
    if (project(cam, p, &x, &y))
        whack(cam, x + (frand() - 0.5f) * 8.f, y + (frand() - 0.5f) * 8.f, playing);
    s_auto_wait = 0.25f + frand() * 0.35f;
}

/* ── Input ───────────────────────────────────────────────────────────────── */
#define MAX_TOUCH 5

typedef struct {
    bool  start, quit;
    int   ntaps;                 /* new fingers down this frame */
    float tx[MAX_TOUCH], ty[MAX_TOUCH];
} input_t;

static uint32_t s_prev_btn;
static int s_touch_n;
static float s_touch_x[MAX_TOUCH], s_touch_y[MAX_TOUCH];

static input_t read_input(papp_surface_t *surf)
{
    input_t in = { 0 };
    papp_gamepad_t pad = { 0 };
    uint32_t held = papp_input_gamepad(0, &pad) ? pad.buttons : 0;
    uint32_t press = held & ~s_prev_btn;
    s_prev_btn = held;
    if (press & PAPP_BTN_B)
        in.quit = true;
    if (press & (PAPP_BTN_START | PAPP_BTN_SELECT | PAPP_BTN_A))
        in.start = true;

    /* touch: a finger that was not down last frame is a tap. The order of the points may
     * change between frames, so a point counts as new when no finger was near it before. */
    papp_touch_point_t tp[MAX_TOUCH];
    int n = papp_disp_touch(surf, tp, MAX_TOUCH);
    if (n < 0)
        n = 0;
    for (int i = 0; i < n; i++) {
        float x = (float)tp[i].x, y = (float)tp[i].y;
        bool seen = false;
        for (int k = 0; k < s_touch_n && !seen; k++) {
            float dx = x - s_touch_x[k], dy = y - s_touch_y[k];
            seen = dx * dx + dy * dy < 30.f * 30.f;
        }
        if (!seen && x >= EXIT_X - EXIT_PAD && y >= EXIT_Y - EXIT_PAD) {
            in.quit = true;      /* the exit button */
            continue;
        }
        if (!seen) {
            in.tx[in.ntaps] = x;
            in.ty[in.ntaps] = y;
            in.ntaps++;
        }
    }
    for (int i = 0; i < n; i++) {
        s_touch_x[i] = (float)tp[i].x;
        s_touch_y[i] = (float)tp[i].y;
    }
    s_touch_n = n;
    if (in.ntaps)
        in.start = true;
    return in;
}

/* ── Drawing ─────────────────────────────────────────────────────────────── */
static void sky_row(uint16_t *row, int y, int w, void *ctx)
{
    int t = y * 255 / H;
    uint16_t c = PAPP_RGB565(90 + t / 6, 150 + t / 6, 230 - t / 10);
    for (int x = 0; x < w; x++)
        row[x] = c;
}

static void draw_mesh(e3d_t *e, const e3d_mesh *mesh, const e3d_mat *m, int32_t rgb, uint32_t flags)
{
    e3d_object o;
    e3d_object_init(&o, mesh);
    o.m = *m;
    o.color = rgb;
    o.flags = flags;
    e3d_draw(e, &o);
}

static void draw_scene(e3d_t *e)
{
    e3d_object o;
    e3d_object_init(&o, m_ground);
    o.flags = E3D_FLOOR;
    e3d_draw(e, &o);

    e3d_mat m;
    e3d_identity(&m);
    draw_mesh(e, m_board, &m, -1, 0);

    for (int r = 0; r < GRID; r++)
        for (int c = 0; c < GRID; c++) {
            mole_t *ml = &g_mole[r][c];
            if (ml->state == M_DOWN)
                continue;
            e3d_mat place, sq;
            e3d_move(&place, hole_x(r), mole_y(ml), hole_z(c));
            if (ml->state == M_HIT) {
                /* squashed flat by the blow, springing back a little */
                float u = fminf(1.f, ml->t / 0.28f);
                float s = 0.55f + 0.2f * u;
                e3d_scale(&sq, 1.f + (1.f - s) * 0.6f, s, 1.f + (1.f - s) * 0.6f);
            } else {
                /* a little wobble while it is up */
                float wob = ml->state == M_UP ? 6.f * sinf(g_t * 9.f + (float)(r * 7 + c)) : 0;
                e3d_rotate(&sq, wob, 0, 0);
            }
            e3d_mul(&place, &sq);
            draw_mesh(e, ml->gold ? m_gold : m_mole, &place, -1, E3D_NO_SHADOW);
        }

    for (int i = 0; i < MAX_SPARKS; i++) {
        spark_t *s = &g_spark[i];
        if (!s->alive)
            continue;
        e3d_mat mv, rr;
        e3d_move(&mv, s->p[0], s->p[1], s->p[2]);
        e3d_rotate(&rr, s->rot, s->rot * 0.7f, 0);
        e3d_mul(&mv, &rr);
        draw_mesh(e, m_spark, &mv, s->color, E3D_NO_SHADOW | E3D_UNLIT);
    }
}

static void text_sh(uint16_t *px, int stride, int x, int y, const char *s, uint16_t col, int scale)
{
    e3d_text(px, stride, W, H, x + scale, y + scale, s, 0x0000, scale);
    e3d_text(px, stride, W, H, x, y, s, col, scale);
}

static void text_c(uint16_t *px, int stride, int y, const char *s, uint16_t col, int scale)
{
    int w = (int)strlen(s) * 6 * scale - scale;
    text_sh(px, stride, (W - w) / 2, y, s, col, scale);
}

static void dim(uint16_t *px, int stride, int y0, int y1)
{
    for (int y = y0; y < y1; y++)
        for (int x = 0; x < W; x++) {
            uint16_t c = px[y * stride + x];
            px[y * stride + x] = (uint16_t)((c >> 1) & 0x7BEF);
        }
}

/* A ring of radius r and thickness th round (cx, cy). */
static void ring(uint16_t *px, int stride, float cx, float cy, float r, float th, uint16_t col)
{
    int x0 = (int)(cx - r - 1), x1 = (int)(cx + r + 1), y0 = (int)(cy - r - 1), y1 = (int)(cy + r + 1);
    float ri = (r - th) * (r - th), ro = r * r;
    for (int y = y0 < 0 ? 0 : y0; y <= y1 && y < H; y++)
        for (int x = x0 < 0 ? 0 : x0; x <= x1 && x < W; x++) {
            float dx = (float)x + 0.5f - cx, dy = (float)y + 0.5f - cy, d = dx * dx + dy * dy;
            if (d >= ri && d <= ro)
                px[y * stride + x] = col;
        }
}

/* The exit button: a dark box with a light border and an X. */
static void draw_exit(uint16_t *px, int stride)
{
    const uint16_t edge = PAPP_RGB565(230, 230, 230);
    for (int y = EXIT_Y; y < EXIT_Y + EXIT_H; y++)
        for (int x = EXIT_X; x < EXIT_X + EXIT_W; x++) {
            bool border = y == EXIT_Y || y == EXIT_Y + EXIT_H - 1 || x == EXIT_X || x == EXIT_X + EXIT_W - 1;
            bool corner = (y == EXIT_Y || y == EXIT_Y + EXIT_H - 1) && (x == EXIT_X || x == EXIT_X + EXIT_W - 1);
            if (corner)
                continue;
            uint16_t c = px[y * stride + x];
            px[y * stride + x] = border ? edge : (uint16_t)((c >> 2) & 0x39E7);   /* a quarter as bright */
        }
    e3d_text(px, stride, W, H, EXIT_X + (EXIT_W - 9) / 2, EXIT_Y + (EXIT_H - 14) / 2, "X",
             PAPP_RGB565(255, 110, 100), 2);
}

static void draw_hud(uint16_t *px, int stride, const e3d_camera *cam)
{
    char t[48];
    const uint16_t yellow = PAPP_RGB565(255, 220, 60);

    for (int i = 0; i < MAX_TAPS; i++) {
        tapfx_t *f = &g_tapfx[i];
        if (f->life <= 0)
            continue;
        float u = 1.f - f->life / 0.25f;   /* 0 .. 1 */
        if (f->hit) {
            ring(px, stride, f->x, f->y, 8.f + 22.f * u, 3.f, yellow);
            ring(px, stride, f->x, f->y, 4.f + 12.f * u, 2.f, 0xFFFF);
        } else
            ring(px, stride, f->x, f->y, 5.f + 10.f * u, 2.f, PAPP_RGB565(230, 230, 230));
    }

    for (int i = 0; i < MAX_POPUPS; i++) {
        popup_t *p = &g_popup[i];
        float sx, sy;
        if (p->life <= 0 || !project(cam, p->p, &sx, &sy))
            continue;
        int w = (int)strlen(p->text) * 12;
        text_sh(px, stride, (int)sx - w / 2, (int)sy, p->text, p->color, 2);
    }

    if (g_game == G_PLAY) {
        snprintf(t, sizeof(t), "SCORE %d", g_score);
        text_sh(px, stride, 6, 6, t, 0xFFFF, 2);
        int secs = (int)ceilf(g_time_left);
        snprintf(t, sizeof(t), "TIME %d", secs);
        text_sh(px, stride, W - 6 - (int)strlen(t) * 12, 6, t,
                secs <= 10 ? PAPP_RGB565(255, 80, 70) : 0xFFFF, 2);
        if (g_combo >= 5) {
            int mult = 1 + (g_combo >= 15 ? 3 : g_combo / 5);
            snprintf(t, sizeof(t), "COMBO X%d", mult);
            text_c(px, stride, 26, t, yellow, 2);
        }
    } else if (g_game == G_TITLE) {
        dim(px, stride, 36, 150);
        text_c(px, stride, 48, "WHACK-A-MOLE", yellow, 4);
        text_c(px, stride, 86, "3D", 0xFFFF, 3);
        if (((int)(g_t * 2.f)) & 1)
            text_c(px, stride, 116, "TAP TO START", 0xFFFF, 2);
        snprintf(t, sizeof(t), "BEST %d", g_best);
        text_c(px, stride, 136, t, PAPP_RGB565(180, 220, 255), 1);
        text_sh(px, stride, 4, H - 10, "TAP THE MOLES", 0xFFFF, 1);
    } else {
        dim(px, stride, 30, 180);
        text_c(px, stride, 40, "TIME UP", PAPP_RGB565(255, 90, 80), 4);
        snprintf(t, sizeof(t), "SCORE %d", g_score);
        text_c(px, stride, 80, t, 0xFFFF, 3);
        if (g_new_best)
            text_c(px, stride, 108, "NEW BEST", yellow, 2);
        else {
            snprintf(t, sizeof(t), "BEST %d", g_best);
            text_c(px, stride, 108, t, PAPP_RGB565(180, 220, 255), 2);
        }
        int acc = g_taps ? g_hits * 100 / g_taps : 0;
        snprintf(t, sizeof(t), "HITS %d   ESCAPED %d   ACCURACY %d%%", g_hits, g_escaped, acc);
        text_c(px, stride, 134, t, 0xFFFF, 1);
        if (g_over_t > 1.2f && (((int)(g_t * 2.f)) & 1))
            text_c(px, stride, 154, "TAP TO PLAY AGAIN", 0xFFFF, 1);
    }
}

/* ── Main ────────────────────────────────────────────────────────────────── */
int main(int argc, char **argv)
{
    for (int i = 1; i < argc; i++)
        if (!strcmp(argv[i], "auto"))
            g_auto = true;
    srand((unsigned)papp_time_us());

    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 2 };
    papp_surface_t *surf = papp_disp_surface_create(&cfg);
    e3d_t *e = e3d_create(W, H, 2);
    m_ground = e3d_mesh_floor(-400, -1400, 1800, 1400, 0, 20, 0x4C9A34, 0x3F862B);
    m_board = make_board();
    m_mole = make_mole(0x7A5030u, 0xE8A0A0u, MOLE_S);
    m_gold = make_mole(0xF0B828u, 0xFFE070u, MOLE_S);
    m_spark = make_spark();
    if (!surf || !e || !m_ground || !m_board || !m_mole || !m_gold || !m_spark) {
        papp_log("whackamole: out of memory\n");
        return 1;
    }

    e3d_set_focal(e, FOCAL);
    e3d_set_background_rows(e, sky_row, NULL);
    *e3d_light_get(e) = (e3d_light){ .pos = { CX - 250, 900, -420 }, .r = 255, .g = 248, .b = 230 };
    e3d_params *par = e3d_params_get(e);
    par->ambient = 0.42f;
    par->specular = 0.45f;
    par->shininess = 16;
    par->shadows = true;
    par->shadow_y = 1.f;
    par->shadow_dark = 0.55f;

    /* sound */
    g_snd_lock = papp_mutex_create(false);
    papp_audio_cfg_t acfg = { .rate = SR, .channels = 1, .format = PAPP_AUDIO_S16, .latency_ms = 60 };
    g_snd = g_snd_lock ? papp_audio_open(&acfg) : NULL;
    papp_thread_cfg_t tcfg = { .name = "wm_audio", .stack_size = 4096, .prio = PAPP_PRIO_HIGH, .core = 1 };
    papp_thread_t *aud = g_snd ? papp_thread_create(audio_main, NULL, &tcfg) : NULL;
    if (g_snd && !aud) {
        papp_audio_close(g_snd);
        g_snd = NULL;
    }

    best_load();
    reset_round();
    if (g_auto)
        g_game = G_PLAY;

    e3d_camera *cam = e3d_cam(e);
    int64_t last = papp_time_us();
    int last_tick = -1;
    while (!papp_exit_requested()) {
        int64_t now = papp_time_us();
        float dt = (float)(now - last) * 1e-6f;
        last = now;
        if (dt > 0.05f)
            dt = 0.05f;
        g_t += dt;

        /* the camera (the taps are picked against this view, before the shake) */
        cam->pos[0] = CAM_X;
        cam->pos[1] = CAM_Y;
        cam->pos[2] = 0;
        {
            float d[3] = { CX + 20.f - cam->pos[0], BOARD_Y + 25.f - cam->pos[1], 0 };
            cam->heading = 0;
            cam->pitch = atan2f(d[1], d[0]) * 57.29578f;
            cam->tilt = 0;
        }

        input_t in = read_input(surf);
        if (in.quit)
            break;

        if (g_game == G_PLAY) {
            if (g_auto)
                auto_play(cam, dt, true);
            for (int i = 0; i < in.ntaps; i++)
                whack(cam, in.tx[i], in.ty[i], true);
            g_time_left -= dt;
            int secs = (int)ceilf(g_time_left);
            if (secs <= 5 && secs > 0 && secs != last_tick) {
                sfx(1000.f, 1000.f, 40, WAVE_SQUARE, 0.12f);
                last_tick = secs;
            }
            if (g_time_left <= 0) {
                g_time_left = 0;
                g_game = G_OVER;
                g_over_t = 0;
                last_tick = -1;
                for (int r = 0; r < GRID; r++)   /* everyone ducks */
                    for (int c = 0; c < GRID; c++)
                        if (g_mole[r][c].state == M_RISE || g_mole[r][c].state == M_UP)
                            g_mole[r][c].state = M_FALL;
                sfx_at(784.f, 784.f, 120, WAVE_SQUARE, 0.18f, 0);
                sfx_at(587.f, 587.f, 120, WAVE_SQUARE, 0.18f, 140);
                sfx_at(392.f, 300.f, 300, WAVE_SQUARE, 0.18f, 280);
                if (g_score > g_best) {
                    g_best = g_score;
                    g_new_best = true;
                    best_save();
                }
                papp_log("whackamole: score %d, hits %d of %d taps, escaped %d\n", g_score, g_hits, g_taps,
                         g_escaped);
            }
        } else if (g_game == G_TITLE) {
            /* attract: moles pop up and get whacked on their own */
            auto_play(cam, dt, false);
            if (in.start) {
                reset_round();
                g_game = G_PLAY;
                sfx(500.f, 1000.f, 120, WAVE_TRI, 0.2f);
            }
        } else {
            g_over_t += dt;
            if ((g_over_t > 1.2f && in.start) || (g_auto && g_over_t > 3.f)) {
                reset_round();
                g_game = G_PLAY;
                sfx(500.f, 1000.f, 120, WAVE_TRI, 0.2f);
            }
        }

        /* after time is up the last moles only go down */
        update_moles(dt, g_game == G_PLAY, g_game != G_OVER);
        update_fx(dt);

        /* a hit shakes the view a little */
        if (g_shake > 0) {
            float sh = g_shake * 30.f;
            cam->pos[1] += (frand() - 0.5f) * sh;
            cam->pos[2] += (frand() - 0.5f) * sh;
        }

        int stride;
        uint16_t *px = papp_disp_lock(surf, &stride);
        e3d_begin(e, px, stride);
        draw_scene(e);
        e3d_end(e);
        draw_hud(px, stride, cam);
        draw_exit(px, stride);
        papp_disp_present(surf);
    }

    if (aud) {
        g_snd_quit = true;
        papp_thread_join(aud, 1000, NULL);
    }
    if (g_snd)
        papp_audio_close(g_snd);
    if (g_snd_lock)
        papp_mutex_destroy(g_snd_lock);
    e3d_destroy(e);
    e3d_mesh_free(m_ground);
    e3d_mesh_free(m_board);
    e3d_mesh_free(m_mole);
    e3d_mesh_free(m_gold);
    e3d_mesh_free(m_spark);
    papp_disp_surface_destroy(surf);
    return 0;
}
