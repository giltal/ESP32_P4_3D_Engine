/*
 * Shardwall — a brick-breaker in 3D on the e3d engine.
 *
 * The wall is built of stacked towers of bricks (1 to 3 high) on a lit floor.
 * The ball rolls on the floor and only reaches the bottom brick: break it and
 * the bricks above drop down with a thud. Broken bricks burst into shards that
 * bounce on the floor, and everything casts a shadow from the lamp above.
 *
 * Paddle: the board's paddle knob, touch (the paddle follows your finger), the
 * D-pad or the left stick, whichever moved last.
 * Serve: A (or any face button), or tap the screen; it serves by itself after
 * a few seconds. Pause: START, SELECT or tap the top-right corner.
 * Save: the pause menu's "Save game" keeps the level, score, lives and what is
 * left of the wall in /sd/data/shardwall/save.txt; the title screen then offers
 * "Continue", and the pause menu "Load game". One save slot; it stays after a game over.
 *
 * Arguments: `auto` = the computer plays (attract mode, for testing),
 *            `level N` = start at level N (1..).
 */
#include "papp/papp.h"
#include "e3d.h"

#include <errno.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

PAPP_MANIFEST(.id = "giltal.shardwall", .name = "Shardwall", .version = PAPP_APP_VERSION, .main_stack = 16384);

#define W 400
#define H 240

/* ── Field (world units; x = depth away from the camera, z = right, y = up) ── */
#define FIELD_Z    200.f        /* inner faces of the side walls at z = +-FIELD_Z */
#define BACK_X     600.f        /* inner face of the back wall */
#define PADDLE_X   40.f         /* centre of the paddle */
#define PADDLE_HX  7.f
#define PADDLE_HZ  34.f
#define PADDLE_WIDE 54.f
#define BALL_R     7.f
#define BALL_Y     8.f
#define COLS       10
#define ROWS       6
#define TIERS      3
#define COL_W      38.f
#define ROW_D      24.f
#define ROW0_X     560.f        /* row 0 is the farthest */
#define BRICK_HZ   17.f
#define BRICK_HX   10.f
#define BRICK_H    15.f
#define MAX_BALLS  4
#define MAX_SHARDS 96
#define MAX_CAPS   4

/* Camera */
#define CAM_X     (-310.f)
#define CAM_Y      430.f
#define CAM_PITCH (-37.f)
#define FOCAL      425.f

/* ── Levels: one character per cell, '.' = empty, '1'..'3' = tower height. Row 0 is the far row. ── */
static const char *const levels[][ROWS] = {
    { "..........",
      "1111111111",
      "1111111111",
      "1111111111",
      "1111111111",
      ".........." },
    { "....33....",
      "...2222...",
      "..222222..",
      ".11111111.",
      "1111111111",
      ".........." },
    { "3.3.3.3.3.",
      ".2.2.2.2.2",
      "1.1.1.1.1.",
      ".1.1.1.1.1",
      "2.2.2.2.2.",
      ".........." },
    { "3333..3333",
      "2..2..2..2",
      "2..2..2..2",
      "1111..1111",
      "..........",
      "1111111111" },
    { "....33....",
      "...3223...",
      "..321123..",
      "...3223...",
      "....33....",
      "1........1" },
    { "3333333333",
      "3........3",
      "3.222222.3",
      "3.2.11.2.3",
      "3........3",
      "2222222222" },
};
#define NLEVELS ((int)(sizeof(levels) / sizeof(levels[0])))

/* ── Sound: a few synthesised voices mixed on an audio thread ────────────── */
#define SR 22050
enum { WAVE_SQUARE, WAVE_TRI, WAVE_NOISE };

typedef struct {
    int   left;                 /* samples to play, 0 = free */
    int   delay;                /* samples before it starts */
    int   total;
    float ph, f, fmul;          /* phase 0..1, frequency, per-sample frequency factor */
    float vol;
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
                float env = (float)v->left / (float)v->total;   /* linear fade out */
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

/* A box from y = 0 to h with bevelled top edges: hx, hz half sizes, bev the bevel.
 * bottom: whether to close the bottom (walls that are seen from below don't need it). */
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

/* ── Game state ──────────────────────────────────────────────────────────── */
typedef struct {
    int      n;                     /* bricks in the tower */
    uint32_t rgb[TIERS];
    float    y[TIERS], vy[TIERS];   /* bottom of each brick, its fall speed */
} cell_t;

typedef struct { float x, z, vx, vz; bool alive; } ball_t;
typedef struct { float p[3], v[3], rot[3], spin[3], life; uint32_t rgb; } shard_t;
enum { CAP_WIDE, CAP_SPLIT, CAP_SLOW, CAP_N };
typedef struct { float x, z; int kind; bool alive; } cap_t;

enum { ST_TITLE, ST_SERVE, ST_PLAY, ST_LOST, ST_CLEAR, ST_OVER };
enum { IN_NONE, IN_POT, IN_PAD, IN_TOUCH };

static cell_t  g_cell[ROWS][COLS];
static ball_t  g_ball[MAX_BALLS];
static shard_t g_shard[MAX_SHARDS];
static cap_t   g_cap[MAX_CAPS];
static int     g_state, g_level, g_lives, g_score, g_hiscore, g_bricks_left;
static float   g_state_t, g_speed, g_pz, g_phw = PADDLE_HZ, g_wide_t, g_shake, g_t, g_flash;
static bool    g_auto, g_icon;
static int     g_in = IN_NONE;

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

static float cell_x(int r) { return ROW0_X - (float)r * ROW_D; }
static float cell_z(int c) { return ((float)c - (COLS - 1) * 0.5f) * COL_W; }

static void load_level(int lv)
{
    const char *const *L = levels[lv % NLEVELS];
    float hue = 200.f + 47.f * (float)lv;
    g_bricks_left = 0;
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            cell_t *ce = &g_cell[r][c];
            ce->n = L[r][c] >= '1' && L[r][c] <= '3' ? L[r][c] - '0' : 0;
            for (int t = 0; t < TIERS; t++) {
                ce->rgb[t] = hsv(hue + (float)r * 26.f, 0.72f - 0.12f * (float)t, 0.80f + 0.08f * (float)t);
                ce->y[t] = (float)t * BRICK_H + 120.f + 40.f * (float)(r + c % 3);   /* they drop in */
                ce->vy[t] = 0;
            }
            g_bricks_left += ce->n;
        }
    for (int i = 0; i < MAX_CAPS; i++)
        g_cap[i].alive = false;
    g_speed = 330.f + 20.f * (float)(lv < 12 ? lv : 12);
    g_wide_t = 0;
}

static void serve(void)
{
    memset(g_ball, 0, sizeof(g_ball));
    g_ball[0].alive = true;
    g_state = ST_SERVE;
    g_state_t = 0;
}

static void new_game(int level)
{
    g_level = level;
    g_lives = 3;
    g_score = 0;
    load_level(g_level);
    serve();
}

static void launch(void)
{
    float a = (frand() - 0.5f) * 0.7f;
    g_ball[0].vx = g_speed * cosf(a);
    g_ball[0].vz = g_speed * sinf(a);
    g_state = ST_PLAY;
    sfx(330, 660, 90, WAVE_SQUARE, 0.25f);
}

static void spawn_shards(float x, float y, float z, uint32_t rgb, int n)
{
    for (int k = 0, i = 0; k < MAX_SHARDS && i < n; k++) {
        shard_t *s = &g_shard[k];
        if (s->life > 0)
            continue;
        s->p[0] = x + (frand() - 0.5f) * 14.f;
        s->p[1] = y + frand() * BRICK_H;
        s->p[2] = z + (frand() - 0.5f) * 30.f;
        s->v[0] = (frand() - 0.3f) * 160.f;
        s->v[1] = 120.f + frand() * 200.f;
        s->v[2] = (frand() - 0.5f) * 260.f;
        for (int a = 0; a < 3; a++) {
            s->rot[a] = frand() * 360.f;
            s->spin[a] = (frand() - 0.5f) * 900.f;
        }
        s->life = 1.0f + frand() * 0.6f;
        s->rgb = rgb;
        i++;
    }
}

static void hiscore_io(bool save)
{
    const char *path = "/sd/data/shardwall/hiscore.txt";
    if (save) {
        mkdir("/sd/data", 0777);
        mkdir("/sd/data/shardwall", 0777);
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

/* ── Saved game: level, score, lives, ball speed and what is left of the wall ── */
#define SAVE_PATH "/sd/data/shardwall/save.txt"
static int   g_save_level = -1;      /* level of the saved game, -1 = none */
static float g_msg_t;                /* "GAME SAVED" on the screen while > 0 */
static const char *g_msg;

static void save_peek(void)
{
    g_save_level = -1;
    FILE *f = fopen(SAVE_PATH, "r");
    if (!f)
        return;
    int lv;
    if (fscanf(f, "SHARDWALL 1 level %d", &lv) == 1 && lv >= 0)
        g_save_level = lv;
    fclose(f);
}

/* false when there is nothing to save (game over) or the card cannot be written */
static bool save_game(void)
{
    int level = g_level;
    bool fresh = false;                  /* the wall is cleared: save the start of the next level */
    if (g_state == ST_OVER || g_state == ST_TITLE || g_lives <= 0)
        return false;
    if (g_state == ST_CLEAR || g_bricks_left <= 0) {
        level++;
        fresh = true;
    }
    mkdir("/sd/data", 0777);
    mkdir("/sd/data/shardwall", 0777);
    FILE *f = NULL;
    for (int tries = 0; !f && tries < 3; tries++) {   /* the card can be busy for a moment */
        f = fopen(SAVE_PATH, "w");
        if (!f) {
            papp_log("shardwall: save: open failed (errno %d, try %d)\n", errno, tries + 1);
            papp_delay_ms(60);
        }
    }
    if (!f)
        return false;
    /* the ball in play counts as a life: continuing serves it again */
    fprintf(f, "SHARDWALL 1 level %d score %d lives %d speed %d fresh %d\n", level, g_score, g_lives,
            (int)g_speed, fresh);
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            const cell_t *ce = &g_cell[r][c];
            fprintf(f, "%d %06lx %06lx %06lx\n", fresh ? 0 : ce->n, (unsigned long)ce->rgb[0],
                    (unsigned long)ce->rgb[1], (unsigned long)ce->rgb[2]);
        }
    bool wr = !ferror(f);
    bool ok = fclose(f) == 0 && wr;
    if (ok)
        g_save_level = level;
    papp_log("shardwall: save level %d score %d lives %d: %s (state %d, errno %d)\n", level + 1, g_score, g_lives,
             ok ? "ok" : "FAILED", g_state, errno);
    return ok;
}

static void serve(void);

static bool load_game(void)
{
    FILE *f = fopen(SAVE_PATH, "r");
    if (!f)
        return false;
    /* read it all first: a damaged file leaves the game being played as it is */
    int level, score, lives, speed, fresh, total = 0;
    static int n_in[ROWS][COLS];
    static uint32_t rgb_in[ROWS][COLS][TIERS];
    bool ok = fscanf(f, "SHARDWALL 1 level %d score %d lives %d speed %d fresh %d", &level, &score, &lives,
                     &speed, &fresh) == 5 && level >= 0 && lives > 0;
    for (int r = 0; r < ROWS && ok && !fresh; r++)
        for (int c = 0; c < COLS && ok; c++) {
            unsigned long rgb[TIERS];
            int n;
            if (fscanf(f, "%d %lx %lx %lx", &n, &rgb[0], &rgb[1], &rgb[2]) != 4 || n < 0 || n > TIERS) {
                ok = false;
                break;
            }
            n_in[r][c] = n;
            for (int t = 0; t < TIERS; t++)
                rgb_in[r][c][t] = (uint32_t)rgb[t] & 0xFFFFFF;
            total += n;
        }
    if (ok && !fresh && total <= 0)
        ok = false;
    fclose(f);
    if (!ok) {
        papp_log("shardwall: the saved game is damaged\n");
        return false;
    }
    load_level(level);                   /* colours, drop-in heights, speed */
    if (!fresh) {
        for (int r = 0; r < ROWS; r++)
            for (int c = 0; c < COLS; c++) {
                g_cell[r][c].n = n_in[r][c];
                memcpy(g_cell[r][c].rgb, rgb_in[r][c], sizeof(g_cell[r][c].rgb));
            }
        g_bricks_left = total;
        if (speed >= 300 && speed <= 600)
            g_speed = (float)speed;
    }
    g_level = level;
    g_score = score;
    g_lives = lives;
    serve();
    papp_log("shardwall: continued at level %d, score %d, lives %d\n", level + 1, score, lives);
    return true;
}

static void break_brick(int r, int c)
{
    cell_t *ce = &g_cell[r][c];
    if (ce->n <= 0)
        return;
    uint32_t rgb = ce->rgb[0];
    spawn_shards(cell_x(r), ce->y[0], cell_z(c), rgb, 7);
    /* the bricks above drop by one */
    for (int t = 0; t + 1 < ce->n; t++) {
        ce->rgb[t] = ce->rgb[t + 1];
        ce->y[t] = ce->y[t + 1];
        ce->vy[t] = ce->vy[t + 1];
    }
    ce->n--;
    g_bricks_left--;
    g_score += 10 + 5 * ce->n + 2 * (ROWS - 1 - r);
    static const float scale[6] = { 523.3f, 587.3f, 659.3f, 784.f, 880.f, 1046.5f };
    float f = scale[(ROWS - 1 - r) % 6] * (ce->n ? 0.75f : 1.f);
    sfx(f, f * 0.98f, 120, WAVE_TRI, 0.35f);
    sfx(1800, 400, 60, WAVE_NOISE, 0.12f);
    g_speed += 2.5f;
    if (g_speed > 600.f)
        g_speed = 600.f;
    /* a power-up now and then */
    if (frand() < 0.11f)
        for (int i = 0; i < MAX_CAPS; i++)
            if (!g_cap[i].alive) {
                g_cap[i] = (cap_t){ cell_x(r), cell_z(c), (int)(frand() * CAP_N) % CAP_N, true };
                break;
            }
}

static void ball_speed(ball_t *b, float s)
{
    float l = sqrtf(b->vx * b->vx + b->vz * b->vz);
    if (l < 1e-3f) {
        b->vx = s;
        b->vz = 0;
        return;
    }
    b->vx *= s / l;
    b->vz *= s / l;
    /* never let it run (almost) sideways forever */
    if (fabsf(b->vx) < 0.32f * s) {
        b->vx = (b->vx < 0 ? -0.32f : 0.32f) * s;
        b->vz = (b->vz < 0 ? -1.f : 1.f) * sqrtf(s * s - b->vx * b->vx);
    }
}

/* one small step of one ball; returns false when it is lost */
static bool ball_step(ball_t *b, float dt)
{
    b->x += b->vx * dt;
    b->z += b->vz * dt;
    if (b->z > FIELD_Z - BALL_R && b->vz > 0) { b->z = FIELD_Z - BALL_R; b->vz = -b->vz; sfx(220, 200, 30, WAVE_TRI, 0.2f); }
    if (b->z < -FIELD_Z + BALL_R && b->vz < 0) { b->z = -FIELD_Z + BALL_R; b->vz = -b->vz; sfx(220, 200, 30, WAVE_TRI, 0.2f); }
    if (b->x > BACK_X - BALL_R && b->vx > 0) { b->x = BACK_X - BALL_R; b->vx = -b->vx; sfx(180, 170, 30, WAVE_TRI, 0.2f); }
    if (b->x < -30.f)
        return false;

    /* paddle */
    float front = PADDLE_X + PADDLE_HX;
    if (b->vx < 0 && b->x - BALL_R <= front && b->x > PADDLE_X - PADDLE_HX && fabsf(b->z - g_pz) <= g_phw + BALL_R) {
        float u = (b->z - g_pz) / (g_phw + BALL_R);
        float a = u * 1.05f;                         /* up to 60 degrees off straight */
        float s = sqrtf(b->vx * b->vx + b->vz * b->vz);
        b->vx = s * cosf(a);
        b->vz = s * sinf(a);
        b->x = front + BALL_R;
        sfx(440, 660, 60, WAVE_SQUARE, 0.22f);
    }

    /* bricks: the cells around the ball */
    int r0 = (int)floorf((ROW0_X - b->x) / ROW_D + 0.5f);
    int c0 = (int)floorf(b->z / COL_W + (COLS - 1) * 0.5f + 0.5f);
    for (int r = r0 - 1; r <= r0 + 1; r++)
        for (int c = c0 - 1; c <= c0 + 1; c++) {
            if (r < 0 || r >= ROWS || c < 0 || c >= COLS || g_cell[r][c].n <= 0)
                continue;
            float cx = cell_x(r), cz = cell_z(c);
            float qx = fmaxf(cx - BRICK_HX, fminf(b->x, cx + BRICK_HX));
            float qz = fmaxf(cz - BRICK_HZ, fminf(b->z, cz + BRICK_HZ));
            float dx = b->x - qx, dz = b->z - qz, d2 = dx * dx + dz * dz;
            if (d2 >= BALL_R * BALL_R)
                continue;
            float nx, nz;
            if (d2 < 1e-4f) {           /* centre inside: push out the short way */
                float px = BRICK_HX + BALL_R - fabsf(b->x - cx), pz = BRICK_HZ + BALL_R - fabsf(b->z - cz);
                if (px < pz) { nx = b->x > cx ? 1.f : -1.f; nz = 0; }
                else         { nx = 0; nz = b->z > cz ? 1.f : -1.f; }
                qx = b->x; qz = b->z;
            } else {
                float d = sqrtf(d2);
                nx = dx / d;
                nz = dz / d;
            }
            float vn = b->vx * nx + b->vz * nz;
            if (vn < 0) {
                b->vx -= 2.f * vn * nx;
                b->vz -= 2.f * vn * nz;
            }
            b->x = qx + nx * (BALL_R + 0.01f);
            b->z = qz + nz * (BALL_R + 0.01f);
            break_brick(r, c);
            ball_speed(b, g_speed);
            return true;            /* one brick per step */
        }
    return true;
}

static void lose_ball(void)
{
    g_lives--;
    g_state = ST_LOST;
    g_state_t = 0;
    g_shake = 6.f;
    sfx(420, 70, 700, WAVE_SQUARE, 0.25f);
    sfx(300, 40, 500, WAVE_NOISE, 0.10f);
}

static void power_up(int kind)
{
    switch (kind) {
    case CAP_WIDE:
        g_wide_t = 12.f;
        sfx_at(392, 392, 80, WAVE_SQUARE, 0.2f, 0);
        sfx_at(587, 587, 120, WAVE_SQUARE, 0.2f, 80);
        break;
    case CAP_SPLIT: {
        ball_t *src = NULL;
        for (int i = 0; i < MAX_BALLS; i++)
            if (g_ball[i].alive) { src = &g_ball[i]; break; }
        if (!src)
            break;
        for (int k = 0, i = 0; i < MAX_BALLS && k < 2; i++)
            if (!g_ball[i].alive) {
                g_ball[i] = *src;
                float a = k ? 0.5f : -0.5f, c = cosf(a), s = sinf(a);
                g_ball[i].vx = src->vx * c - src->vz * s;
                g_ball[i].vz = src->vx * s + src->vz * c;
                ball_speed(&g_ball[i], g_speed);
                k++;
            }
        for (int i = 0; i < 3; i++)
            sfx_at(659.f * (1 + 0.5f * i), 659.f * (1 + 0.5f * i), 70, WAVE_TRI, 0.22f, i * 60);
        break;
    }
    case CAP_SLOW:
        g_speed = fmaxf(300.f, g_speed * 0.75f);
        for (int i = 0; i < MAX_BALLS; i++)
            if (g_ball[i].alive)
                ball_speed(&g_ball[i], g_speed);
        sfx(700, 300, 250, WAVE_TRI, 0.25f);
        break;
    }
}

static void update(float dt)
{
    g_t += dt;
    if (g_msg_t > 0)
        g_msg_t -= dt;
    g_state_t += dt;
    if (g_shake > 0) g_shake = fmaxf(0, g_shake - dt * 20.f);
    if (g_flash > 0) g_flash = fmaxf(0, g_flash - dt * 2.f);
    if (g_wide_t > 0) g_wide_t = fmaxf(0, g_wide_t - dt);
    float want = g_wide_t > 0 ? PADDLE_WIDE : PADDLE_HZ;
    g_phw += (want - g_phw) * fminf(1.f, dt * 8.f);
    float lim = FIELD_Z - g_phw;
    if (g_pz > lim) g_pz = lim;
    if (g_pz < -lim) g_pz = -lim;

    /* towers settle: gravity, landing on the floor or the brick below */
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            cell_t *ce = &g_cell[r][c];
            for (int t = 0; t < ce->n; t++) {
                float rest = t ? ce->y[t - 1] + BRICK_H : 0.f;
                if (ce->y[t] > rest + 0.01f || ce->vy[t] != 0) {
                    ce->vy[t] -= 1500.f * dt;
                    ce->y[t] += ce->vy[t] * dt;
                    if (ce->y[t] <= rest) {
                        ce->y[t] = rest;
                        if (ce->vy[t] < -160.f && g_state != ST_TITLE) {
                            if (g_state == ST_PLAY)
                                sfx(110, 60, 90, WAVE_TRI, 0.30f);
                            g_shake = fmaxf(g_shake, 1.2f);
                        }
                        ce->vy[t] = 0;
                    }
                }
            }
        }

    /* shards */
    for (int k = 0; k < MAX_SHARDS; k++) {
        shard_t *s = &g_shard[k];
        if (s->life <= 0)
            continue;
        s->life -= dt;
        s->v[1] -= 900.f * dt;
        for (int a = 0; a < 3; a++) {
            s->p[a] += s->v[a] * dt;
            s->rot[a] += s->spin[a] * dt;
        }
        if (s->p[1] < 3.f && s->v[1] < 0) {
            s->p[1] = 3.f;
            s->v[1] *= -0.45f;
            s->v[0] *= 0.7f;
            s->v[2] *= 0.7f;
            s->spin[0] *= 0.6f; s->spin[1] *= 0.6f; s->spin[2] *= 0.6f;
        }
        if (fabsf(s->p[2]) > FIELD_Z - 4.f) { s->p[2] = copysignf(FIELD_Z - 4.f, s->p[2]); s->v[2] = -s->v[2] * 0.5f; }
    }

    /* power-ups slide towards the paddle */
    for (int i = 0; i < MAX_CAPS; i++) {
        cap_t *cp = &g_cap[i];
        if (!cp->alive)
            continue;
        cp->x -= 130.f * dt;
        if (g_state == ST_PLAY && fabsf(cp->x - PADDLE_X) < PADDLE_HX + 8.f && fabsf(cp->z - g_pz) < g_phw + 8.f) {
            cp->alive = false;
            power_up(cp->kind);
        } else if (cp->x < -20.f)
            cp->alive = false;
    }

    switch (g_state) {
    case ST_TITLE:
    case ST_SERVE:
        g_ball[0].x = PADDLE_X + PADDLE_HX + BALL_R + 0.5f;
        g_ball[0].z = g_pz;
        if (g_state == ST_SERVE && g_state_t > (g_auto ? 0.8f : 4.f))
            launch();
        break;
    case ST_PLAY: {
        int alive = 0;
        for (int i = 0; i < MAX_BALLS; i++) {
            ball_t *b = &g_ball[i];
            if (!b->alive)
                continue;
            float s = sqrtf(b->vx * b->vx + b->vz * b->vz);
            int n = (int)(s * dt / 3.f) + 1;
            for (int k = 0; k < n && b->alive; k++)
                if (!ball_step(b, dt / (float)n))
                    b->alive = false;
            alive += b->alive;
        }
        if (!alive)
            lose_ball();
        else if (g_bricks_left <= 0) {
            g_state = ST_CLEAR;
            g_state_t = 0;
            g_flash = 1.f;
            g_score += 100 * (g_level + 1);
            static const float arp[5] = { 523.3f, 659.3f, 784.f, 1046.5f, 1318.5f };
            for (int i = 0; i < 5; i++)
                sfx_at(arp[i], arp[i], 160, WAVE_SQUARE, 0.2f, i * 90);
        }
        break;
    }
    case ST_LOST:
        if (g_state_t > 1.2f) {
            if (g_lives > 0)
                serve();
            else {
                g_state = ST_OVER;
                g_state_t = 0;
                if (g_score > g_hiscore) {
                    g_hiscore = g_score;
                    hiscore_io(true);
                }
                static const float dn[4] = { 392.f, 330.f, 262.f, 196.f };
                for (int i = 0; i < 4; i++)
                    sfx_at(dn[i], dn[i] * 0.97f, 220, WAVE_TRI, 0.3f, i * 200);
            }
        }
        break;
    case ST_CLEAR:
        if (g_state_t > 2.2f) {
            g_level++;
            load_level(g_level);
            serve();
        }
        break;
    case ST_OVER:
        if (g_auto && g_state_t > 3.f)
            new_game(0);
        break;
    }
    if (g_score > g_hiscore && g_state != ST_OVER)
        g_hiscore = g_score;
}

/* ── Input ───────────────────────────────────────────────────────────────── */
/* screen x on the paddle's line -> world z (the camera has no heading) */
static float paddle_depth(void)
{
    float a = -CAM_PITCH * (float)M_PI / 180.f;
    return cosf(a) * (PADDLE_X - CAM_X) - sinf(a) * (0.f - CAM_Y);
}

typedef struct {
    bool start, pause, touch_tap;
} actions_t;

static uint32_t s_prev_btn;
static int s_pot_ref = -1, s_touch_n;
static float s_pot_z, s_pot_mid = -1.f;   /* knob reading at the field's centre, -1 = not set */
#define POT_GAIN 2.5f

static actions_t read_input(papp_surface_t *surf, float dt)
{
    actions_t a = { 0 };
    papp_gamepad_t pad = { 0 };
    uint32_t held = papp_input_gamepad(0, &pad) ? pad.buttons : 0;
    uint32_t press = held & ~s_prev_btn;
    s_prev_btn = held;
    if (press & (PAPP_BTN_A | PAPP_BTN_B | PAPP_BTN_X | PAPP_BTN_Y | PAPP_BTN_UP | PAPP_BTN_R1 | PAPP_BTN_L1))
        a.start = true;
    if (press & (PAPP_BTN_START | PAPP_BTN_SELECT))
        a.pause = true;

    float lim = FIELD_Z - g_phw;
    /* the paddle knob: absolute, used once it moves */
    int pv = papp_input_analog(PAPP_ANALOG_PADDLE);
    if (pv >= 0) {
        if (s_pot_ref < 0)
            s_pot_ref = pv;
        /* switch to the knob only on a real turn (its ADC jitters a little) */
        if (g_in != IN_POT && abs(pv - s_pot_ref) > 3500)
            g_in = IN_POT;
        if (g_in != IN_POT)
            s_pot_ref = s_pot_ref + (pv - s_pot_ref) / 64;   /* follow slow drift */
        else {
            /* POT_GAIN: wall to wall takes 1/POT_GAIN of the knob's turn. Turning on past a wall
               slides the centre along, so the paddle answers at once when the knob turns back. */
            if (abs(pv - s_pot_ref) > 120)   /* a few ADC steps: no shimmer from its noise */
                s_pot_ref = pv;
            if (s_pot_mid < 0)
                s_pot_mid = (float)s_pot_ref - g_pz / lim * (65535.f * 0.5f / POT_GAIN);
            float z = ((float)s_pot_ref - s_pot_mid) / (65535.f * 0.5f / POT_GAIN) * lim;
            if (z > lim || z < -lim) {
                z = z > lim ? lim : -lim;
                s_pot_mid = (float)s_pot_ref - z / lim * (65535.f * 0.5f / POT_GAIN);
            }
            s_pot_z += (z - s_pot_z) * fminf(1.f, dt * 25.f);
            g_pz = s_pot_z;
        }
    }
    /* D-pad / stick: relative */
    float dir = 0;
    if (held & PAPP_BTN_LEFT) dir -= 1.f;
    if (held & PAPP_BTN_RIGHT) dir += 1.f;
    if (dir == 0 && (pad.lx > 7000 || pad.lx < -7000))
        dir = (float)pad.lx / 32767.f;
    if (dir != 0) {
        g_in = IN_PAD;
        g_pz += dir * 430.f * dt;
    }
    /* touch: the paddle follows the finger */
    papp_touch_point_t tp[2];
    int n = papp_disp_touch(surf, tp, 2);
    if (n > 0) {
        if (s_touch_n == 0) {
            if (tp[0].x > W - 48 && tp[0].y < 32)
                a.pause = true;
            else
                a.touch_tap = true;
        }
        if (!(tp[0].x > W - 48 && tp[0].y < 32)) {
            g_in = IN_TOUCH;
            float z = ((float)tp[0].x - W * 0.5f) * paddle_depth() / FOCAL;
            g_pz += (z - g_pz) * fminf(1.f, dt * 30.f);
        }
    }
    s_touch_n = n > 0 ? n : 0;
    if (g_in != IN_POT) {
        s_pot_z = g_pz;
        s_pot_mid = -1.f;        /* the knob picks up from where the paddle is */
    }
    return a;
}

/* the computer plays: follow the nearest ball coming back, hitting it off-centre */
static void auto_play(float dt)
{
    ball_t *best = NULL;
    for (int i = 0; i < MAX_BALLS; i++)
        if (g_ball[i].alive && (!best || (g_ball[i].vx < 0 && g_ball[i].x < best->x)))
            best = &g_ball[i];
    if (!best)
        return;
    /* aim at a brick that is left: pick the hit point on the paddle that sends the ball its way
     * (ignoring the side walls), changing the pick every few seconds */
    float target = best->z;
    int pick = (int)(g_t / 3.f), k = 0;
    for (int n = 0; n < ROWS * COLS; n++) {
        int idx = (pick * 7 + n) % (ROWS * COLS);
        if (g_cell[idx / COLS][idx % COLS].n > 0) {
            k = idx;
            break;
        }
    }
    float ang = atan2f(cell_z(k % COLS) - best->z, cell_x(k / COLS) - PADDLE_X);
    float u = fmaxf(-0.9f, fminf(0.9f, ang / 1.05f));
    target = best->z - u * (g_phw + BALL_R);
    float step = 520.f * dt, d = target - g_pz;
    g_pz += d > step ? step : d < -step ? -step : d;
}

/* ── Drawing ─────────────────────────────────────────────────────────────── */
static e3d_mesh *m_floor, *m_brick, *m_wall_side, *m_wall_back, *m_paddle, *m_ball, *m_shard, *m_cap;
static e3d_object o_floor;

static void sky_row(uint16_t *row, int y, int w, void *ctx)
{
    int t = y * 255 / H;
    uint16_t c = PAPP_RGB565(10 + t / 8, 8 + t / 10, 30 + t / 4);
    for (int x = 0; x < w; x++)
        row[x] = c;
}

static void draw_obj(e3d_t *e, const e3d_mesh *mesh, const e3d_mat *m, int32_t rgb, uint32_t flags)
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
    e3d_camera *cam = e3d_cam(e);
    float sh = g_shake;
    *cam = (e3d_camera){ .pos = { CAM_X + (frand() - 0.5f) * sh, CAM_Y + (frand() - 0.5f) * sh, (frand() - 0.5f) * sh },
                         .pitch = CAM_PITCH };
    if (g_icon)
        *cam = (e3d_camera){ .pos = { 250.f, 165.f, -60.f }, .pitch = -27.f, .heading = 7.f };
    float glow = 1.f + 0.4f * g_flash;
    *e3d_light_get(e) = (e3d_light){ .pos = { 700.f + 60.f * sinf(g_t * 0.31f), 420.f, -260.f + 80.f * cosf(g_t * 0.23f) },
                                     .r = fminf(255, 245 * glow), .g = fminf(255, 236 * glow), .b = fminf(255, 220 * glow) };
    e3d_draw(e, &o_floor);

    e3d_mat m;
    /* walls */
    e3d_move(&m, (BACK_X - 60.f) * 0.5f, 0, -FIELD_Z - 7.f);
    draw_obj(e, m_wall_side, &m, 0x6878A0, 0);
    e3d_move(&m, (BACK_X - 60.f) * 0.5f, 0, FIELD_Z + 7.f);
    draw_obj(e, m_wall_side, &m, 0x6878A0, 0);
    e3d_move(&m, BACK_X + 7.f, 0, 0);
    draw_obj(e, m_wall_back, &m, 0x7080A8, 0);

    /* bricks */
    for (int r = 0; r < ROWS; r++)
        for (int c = 0; c < COLS; c++) {
            const cell_t *ce = &g_cell[r][c];
            for (int t = 0; t < ce->n; t++) {
                e3d_move(&m, cell_x(r), ce->y[t], cell_z(c));
                draw_obj(e, m_brick, &m, (int32_t)ce->rgb[t], 0);
            }
        }

    /* paddle: the box stretched to its width */
    e3d_mat s;
    e3d_move(&m, PADDLE_X, 0, g_pz);
    e3d_scale(&s, 1, 1, g_phw / PADDLE_HZ);
    e3d_mul(&m, &s);
    draw_obj(e, m_paddle, &m, g_wide_t > 0 ? 0x60E0A0 : 0xE8E8F0, 0);

    /* balls */
    for (int i = 0; i < MAX_BALLS; i++)
        if (g_ball[i].alive && g_state != ST_LOST) {
            e3d_move(&m, g_ball[i].x, BALL_Y, g_ball[i].z);
            draw_obj(e, m_ball, &m, 0xFFF4D8, 0);
        }

    /* power-ups: spinning gems */
    static const int32_t cap_rgb[CAP_N] = { 0x40F070, 0xF040D0, 0x40D0F8 };
    for (int i = 0; i < MAX_CAPS; i++)
        if (g_cap[i].alive) {
            e3d_mat rot;
            e3d_move(&m, g_cap[i].x, 14.f + 4.f * sinf(g_t * 6.f + (float)i), g_cap[i].z);
            e3d_rotate(&rot, 0, g_t * 220.f, 25.f);
            e3d_mul(&m, &rot);
            draw_obj(e, m_cap, &m, cap_rgb[g_cap[i].kind], E3D_FLAT);
        }

    /* shards */
    for (int k = 0; k < MAX_SHARDS; k++) {
        const shard_t *sd = &g_shard[k];
        if (sd->life <= 0)
            continue;
        e3d_mat rot;
        float sc = sd->life < 0.4f ? sd->life / 0.4f : 1.f;
        e3d_move(&m, sd->p[0], sd->p[1], sd->p[2]);
        e3d_rotate(&rot, sd->rot[0], sd->rot[1], sd->rot[2]);
        e3d_mul(&m, &rot);
        e3d_scale(&rot, sc, sc, sc);
        e3d_mul(&m, &rot);
        draw_obj(e, m_shard, &m, (int32_t)sd->rgb, E3D_FLAT | E3D_DOUBLE_SIDED);
    }
}

static void text_c(uint16_t *px, int y, const char *s, uint16_t col, int scale)
{
    int w = (int)strlen(s) * 6 * scale - scale;
    e3d_text(px, W, W, H, (W - w) / 2 + 1, y + 1, s, 0x0000, scale);
    e3d_text(px, W, W, H, (W - w) / 2, y, s, col, scale);
}

static void draw_hud(uint16_t *px)
{
    char line[48];
    snprintf(line, sizeof(line), "SCORE %06d", g_score);
    e3d_text(px, W, W, H, 4, 3, line, 0xFFFF, 1);
    snprintf(line, sizeof(line), "HI %06d", g_hiscore);
    e3d_text(px, W, W, H, 4, 12, line, 0xBDF7, 1);
    snprintf(line, sizeof(line), "LEVEL %d", g_level + 1);
    text_c(px, 3, line, 0xFFE0, 1);
    for (int i = 0; i < g_lives - (g_state == ST_SERVE || g_state == ST_TITLE ? 1 : 0) && i < 8; i++)
        e3d_text(px, W, W, H, W - 64 - i * 8, 3, "O", 0xFFF0, 1);
    e3d_text(px, W, W, H, W - 16, 3, "II", 0xC618, 1);

    switch (g_state) {
    case ST_TITLE:
        text_c(px, 70, "SHARDWALL", 0xFFFF, 4);
        text_c(px, 112, "BREAK THE TOWERS", 0xFE60, 1);
        if (((int)(g_t * 2.f)) & 1)
            text_c(px, 150, "PRESS A OR TAP TO PLAY", 0xFFFF, 1);
        text_c(px, 166, "KNOB  TOUCH  OR  PAD MOVES THE PADDLE", 0xA534, 1);
        if (g_save_level >= 0) {
            snprintf(line, sizeof(line), "SAVED GAME  LEVEL %d", g_save_level + 1);
            text_c(px, 190, line, 0x07FF, 1);
        }
        break;
    case ST_SERVE:
        if (!g_auto && g_state_t < 3.5f && (((int)(g_state_t * 3.f)) & 1) == 0)
            text_c(px, 150, "PRESS A OR TAP TO SERVE", 0xFFFF, 1);
        break;
    case ST_CLEAR:
        text_c(px, 90, "LEVEL CLEAR", 0x07FF, 3);
        break;
    case ST_OVER:
        text_c(px, 86, "GAME OVER", 0xF9A6, 4);
        snprintf(line, sizeof(line), "SCORE %d", g_score);
        text_c(px, 126, line, 0xFFFF, 2);
        if (g_state_t > 1.f)
            text_c(px, 156, "PRESS A OR TAP", 0xBDF7, 1);
        break;
    }
    if (g_msg_t > 0 && g_msg)
        text_c(px, 60, g_msg, 0x07FF, 2);
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

static void note(const char *msg)
{
    g_msg = msg;
    g_msg_t = 2.f;
}

/* returns false to exit */
static bool pause_menu(void)
{
    enum { A_RESUME, A_SAVE, A_LOAD, A_SAVE_EXIT, A_RESTART, A_EXIT };
    char load[32];
    const char *items[6];
    int act[6], n = 0;
    items[n] = "Resume";         act[n++] = A_RESUME;
    items[n] = "Save game";      act[n++] = A_SAVE;
    if (g_save_level >= 0) {     /* only when there is a saved game */
        snprintf(load, sizeof(load), "Load game (level %d)", g_save_level + 1);
        items[n] = load;         act[n++] = A_LOAD;
    }
    items[n] = "Save and exit";  act[n++] = A_SAVE_EXIT;
    items[n] = "Restart";        act[n++] = A_RESTART;
    items[n] = "Exit";           act[n++] = A_EXIT;
    int r = menu("Shardwall", items, n);
    int a = r >= 0 && r < n ? act[r] : A_RESUME;
    if (a == A_SAVE || a == A_SAVE_EXIT) {
        bool ok = save_game();
        note(ok ? "GAME SAVED" : g_state == ST_OVER || g_lives <= 0 ? "NOTHING TO SAVE" : "COULD NOT SAVE");
        if (ok && a == A_SAVE_EXIT)
            return false;
    } else if (a == A_LOAD) {
        memset(g_shard, 0, sizeof(g_shard));   /* no shards left over from the game before */
        note(load_game() ? "GAME LOADED" : "SAVE DAMAGED");
    } else if (a == A_RESTART)
        new_game(0);
    return a != A_EXIT && !papp_exit_requested();
}

/* title screen: continue the saved game or start a new one */
static void start_menu(int start_level)
{
    if (g_save_level < 0) {
        new_game(start_level);
        return;
    }
    char cont[32];
    snprintf(cont, sizeof(cont), "Continue (level %d)", g_save_level + 1);
    const char *const items[] = { cont, "New game" };
    int r = menu("Shardwall", items, 2);
    if (r == 0) {
        if (!load_game()) {
            note("SAVE DAMAGED");
            new_game(start_level);
        }
    } else if (r == 1)
        new_game(start_level);
}

int main(int argc, char **argv)
{
    int start_level = 0;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "auto"))
            g_auto = true;
        else if (!strcmp(argv[i], "icon"))
            g_icon = true;   /* a still close-up for the launcher icon */
        else if (!strcmp(argv[i], "level") && i + 1 < argc)
            start_level = atoi(argv[++i]) - 1;
    }
    if (start_level < 0)
        start_level = 0;
    srand((unsigned)papp_time_us());

    papp_surface_cfg_t cfg = { .width = W, .height = H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 1 };
    g_surf = papp_disp_surface_create(&cfg);
    uint16_t *fb[2];
    for (int i = 0; i < 2; i++)
        fb[i] = papp_alloc(sizeof(uint16_t) * W * H, PAPP_MEM_LARGE | PAPP_MEM_DMA, 64);
    g_pres_go = papp_sem_create(0, 1);
    g_pres_done = papp_sem_create(1, 1);
    papp_thread_cfg_t pcfg = { .name = "sw_present", .stack_size = 4096, .prio = PAPP_PRIO_HIGH, .core = 0 };
    papp_thread_t *pres = (g_surf && fb[0] && fb[1] && g_pres_go && g_pres_done)
                              ? papp_thread_create(presenter_main, NULL, &pcfg) : NULL;
    e3d_t *e = e3d_create(W, H, 2);
    m_floor = e3d_mesh_floor(-120, -330, 720, 330, 0, 16, 0x283456, 0x1C2440);
    m_brick = bevel_box(BRICK_HX, BRICK_HZ, BRICK_H, 3.f, false);
    m_wall_side = bevel_box((BACK_X + 60.f) * 0.5f, 7.f, 26.f, 3.f, false);
    m_wall_back = bevel_box(7.f, FIELD_Z + 14.f, 26.f, 3.f, false);
    m_paddle = bevel_box(PADDLE_HX, PADDLE_HZ, 14.f, 3.5f, false);
    m_ball = e3d_mesh_sphere(BALL_R, 14, 10, 255, 255, 255);
    m_shard = tetra(4.5f);
    m_cap = e3d_mesh_sphere(8.f, 6, 3, 255, 255, 255);
    if (!pres || !e || !m_floor || !m_brick || !m_wall_side || !m_wall_back || !m_paddle || !m_ball || !m_shard || !m_cap) {
        papp_log("shardwall: out of memory\n");
        return 1;
    }
    e3d_object_init(&o_floor, m_floor);
    o_floor.flags = E3D_FLOOR;
    e3d_set_focal(e, FOCAL);
    e3d_set_background_rows(e, sky_row, NULL);
    e3d_params *par = e3d_params_get(e);
    par->shadows = true;
    par->shadow_y = 0.5f;
    par->shadow_dark = 0.40f;
    par->ambient = 0.46f;
    par->specular = 0.7f;
    par->shininess = 20.f;

    /* sound */
    g_snd_lock = papp_mutex_create(false);
    papp_audio_cfg_t acfg = { .rate = SR, .channels = 1, .format = PAPP_AUDIO_S16, .latency_ms = 60 };
    g_snd = g_snd_lock ? papp_audio_open(&acfg) : NULL;
    papp_thread_cfg_t acfg_t = { .name = "sw_audio", .stack_size = 4096, .prio = PAPP_PRIO_HIGH, .core = 1 };
    papp_thread_t *aud = g_snd ? papp_thread_create(audio_main, NULL, &acfg_t) : NULL;
    if (!aud && g_snd) {
        papp_audio_close(g_snd);
        g_snd = NULL;
    }

    hiscore_io(false);
    save_peek();
    load_level(start_level);
    g_level = start_level;
    g_lives = 3;
    g_state = ST_TITLE;
    if (g_auto)
        new_game(start_level);
    if (g_icon)
        load_level(4);
    papp_log("shardwall: running%s, level %d\n", g_auto ? " (auto)" : "", start_level + 1);

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
            g_pz = 90.f * sinf(g_t * 0.8f);
            if (go || act.pause) {
                start_menu(start_level);
                last = papp_time_us();
            }
        } else if (g_state == ST_SERVE && go && !g_auto)
            launch();
        else if (g_state == ST_OVER && go && g_state_t > 1.f)
            new_game(0);
        if (g_auto)
            auto_play(dt);
        update(dt);

        if (g_icon) {
            g_state = ST_PLAY;
            g_lives = 3;
            g_ball[0] = (ball_t){ .x = 440.f, .z = 30.f, .alive = true };
            g_pz = -400.f;
        }
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
            papp_log("shardwall: %.1f fps, lv %d, bricks %d, tris %d + %d shadow, geom %d us, raster %d us, input %d, "
                     "sound %s, played %lu, underruns %lu, sram %u (dma %u)\n",
                     (double)fps, g_level + 1, g_bricks_left, st->tris_drawn, st->shadow_tris, st->t_geom_us,
                     st->t_raster_us, g_in, g_snd ? "on" : "off", (unsigned long)as.played_frames,
                     (unsigned long)as.underruns, (unsigned)papp_mem_free_size(PAPP_MEM_FAST),
                     (unsigned)papp_mem_free_size(PAPP_MEM_FAST | PAPP_MEM_DMA));
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
    e3d_mesh_free(m_floor);
    e3d_mesh_free(m_brick);
    e3d_mesh_free(m_wall_side);
    e3d_mesh_free(m_wall_back);
    e3d_mesh_free(m_paddle);
    e3d_mesh_free(m_ball);
    e3d_mesh_free(m_shard);
    e3d_mesh_free(m_cap);
    papp_sem_destroy(g_pres_go);
    papp_sem_destroy(g_pres_done);
    papp_free(fb[0]);
    papp_free(fb[1]);
    papp_disp_surface_destroy(g_surf);
    return 0;
}
