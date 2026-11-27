/*
 * Cinderhorn — a 3D platformer on the e3d engine, in the spirit of the PS1-era
 * collect-a-thons: a small dragon runs, jumps, glides, charges and breathes
 * fire across islands in the sky.
 *
 * Three worlds: Meadow Isles (day, a blue sea), Sunset Mesas (red rock over a
 * sea of lava) and Frostfang Peaks (night, snow and an icy sea). In each, light
 * the four beacons with your fire, then the portal opens and takes you on.
 * Gems lie everywhere: on paths, on floating stones, in urns and in iron chests,
 * and the creatures drop them too. Your best gem count and time per world are
 * kept.
 *
 * The land is a height field (2 m cells) built from the world's features
 * (islands, plateaus, cliffs, ramps and pits), cut into 32 m chunks with three
 * levels of detail. The sea, the sky, the clouds and the far mountains are drawn
 * per row by the background function, so they cost no triangles.
 *
 * The frame is drawn at the display's own resolution by default (1024 x 600 on
 * the 7" board): Settings > Resolution has smaller sizes for a faster frame.
 *
 * Creatures: GRUBS (fire or charge), BEETLES (armoured: fire bounces off, charge
 * them), BRUTES (too big to charge: two blasts of fire).
 *
 * Controls:
 *   left stick / D-pad   move (relative to the camera)
 *   A                    jump; A again in the air: glide (A once more: drop)
 *   B                    breathe fire
 *   X / R2 (held)        charge
 *   L1 / R1, right stick turn the camera;  Y: camera behind you
 *   START / SELECT       pause
 *   Touch: the left half is a stick (where the finger lands), the right half's
 *          bottom row is JUMP / FIRE / DASH, the top-right corner pauses.
 *
 * Records and settings: /sd/data/cinderhorn/.
 * Arguments: `auto` = the computer plays (testing), `fps` = the frame rate,
 *            `level N`, `res N` (0 = the display's size .. 3 = half of it).
 */
#include "papp/papp.h"
#include "e3d.h"

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>

PAPP_MANIFEST(.id = "michael.cinderhorn", .name = "Cinderhorn", .version = PAPP_APP_VERSION, .main_stack = 16384);

/* ── Frame size ──────────────────────────────────────────────────────────────
 * k_res[0] is the display's own size; the others are 4/5, 5/8 and 1/2 of it. */
#define WMAX 1920
#define HMAX 1200
#define NRES 4
static int k_res[NRES][2];
static char k_res_name_buf[NRES][48];
static const char *k_res_name[NRES];
static int g_res = 0;
static int g_w = 1024, g_h = 600;
#define W g_w
#define H g_h
#define DEG (180.f / (float)M_PI)
#define RAD ((float)M_PI / 180.f)
#define NC(a) ((int)(sizeof(a) / sizeof(a[0])))

static void res_init(int nw, int nh)
{
    if (nw < 320 || nh < 200 || nw > WMAX || nh > HMAX) {
        nw = 1024;
        nh = 600;
    }
    static const int num[NRES] = { 8, 32, 5, 4 }, den[NRES] = { 8, 40, 8, 8 };
    for (int i = 0; i < NRES; i++) {
        int w = (nw * num[i] / den[i]) & ~7, h = (nh * num[i] / den[i]) & ~1;
        w = w < 9999 ? w : 9999;
        h = h < 9999 ? h : 9999;
        if (i == 0) {
            w = nw & ~7;
            h = nh & ~1;
        }
        k_res[i][0] = w;
        k_res[i][1] = h;
        snprintf(k_res_name_buf[i], sizeof(k_res_name_buf[i]), "%d x %d%s", w, h,
                 i == 0 ? " (full, sharpest)" : i == NRES - 1 ? " (fastest)" : "");
        k_res_name[i] = k_res_name_buf[i];
    }
}

static float frand(void) { return (float)rand() / (float)RAND_MAX; }
static float frand2(void) { return frand() * 2.f - 1.f; }
static float clampf(float v, float lo, float hi) { return v < lo ? lo : v > hi ? hi : v; }
static float lerpf(float a, float b, float t) { return a + (b - a) * t; }
static float smooth01(float t)
{
    t = clampf(t, 0, 1);
    return t * t * (3.f - 2.f * t);
}
static float wrap_pi(float a)
{
    while (a > (float)M_PI)
        a -= 2.f * (float)M_PI;
    while (a < -(float)M_PI)
        a += 2.f * (float)M_PI;
    return a;
}
/* turn a towards b by at most step (radians) */
static float turn_to(float a, float b, float step)
{
    float d = wrap_pi(b - a);
    if (d > step) d = step;
    if (d < -step) d = -step;
    return wrap_pi(a + d);
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

static void v3_set(float *o, float x, float y, float z) { o[0] = x; o[1] = y; o[2] = z; }
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

/* a small integer hash, for the patterns */
static uint32_t hash2(int x, int y)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u;
    h = (h ^ (h >> 13)) * 1274126177u;
    return h ^ (h >> 16);
}
static float hashf(int x, int y) { return (float)(hash2(x, y) & 0xFFFF) * (1.f / 65535.f); }
/* smooth value noise, about -1..1 */
static float vnoise(float x, float y)
{
    int ix = (int)floorf(x), iy = (int)floorf(y);
    float fx = x - (float)ix, fy = y - (float)iy;
    fx = fx * fx * (3.f - 2.f * fx);
    fy = fy * fy * (3.f - 2.f * fy);
    float a = hashf(ix, iy), b = hashf(ix + 1, iy), c = hashf(ix, iy + 1), d = hashf(ix + 1, iy + 1);
    return (lerpf(lerpf(a, b, fx), lerpf(c, d, fx), fy)) * 2.f - 1.f;
}

/* ── Sound: effects, the glide wind, the fire roar and a little tune per world ── */
#define SR 22050
enum { WAVE_SQUARE, WAVE_TRI, WAVE_NOISE, WAVE_SAW, WAVE_PULSE };
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
static volatile float s_wind_vol, s_fire_vol, s_charge_vol;
static volatile bool s_music_on = true;
static volatile int s_song;           /* the world's tune */
static volatile float s_music_vol = 1.f;

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

static void snd_jump(void) { sfx(330, 660, 140, WAVE_TRI, 0.20f); }
static void snd_glide(void) { sfx(500, 900, 220, WAVE_TRI, 0.12f); sfx(200, 400, 260, WAVE_NOISE, 0.08f); }
static void snd_land(void) { sfx(140, 70, 70, WAVE_NOISE, 0.12f); }
static void snd_gem(int value)
{
    float f = value >= 25 ? 1568.f : value >= 10 ? 1319.f : value >= 5 ? 1175.f : 1047.f;
    sfx(f, f, 60, WAVE_SQUARE, 0.09f);
    sfx_at(f * 1.5f, f * 1.5f, 120, WAVE_SQUARE, 0.08f, 55);
    if (value >= 10)
        sfx_at(f * 2.f, f * 2.f, 160, WAVE_TRI, 0.10f, 120);
}
static void snd_heart(void) { sfx(523, 523, 80, WAVE_TRI, 0.2f); sfx_at(784, 784, 80, WAVE_TRI, 0.2f, 80); sfx_at(1047, 1047, 160, WAVE_TRI, 0.2f, 160); }
static void snd_hurt(void) { sfx(500, 120, 260, WAVE_SQUARE, 0.18f); sfx(300, 80, 200, WAVE_NOISE, 0.15f); }
static void snd_pop(void) { sfx(700, 150, 180, WAVE_SQUARE, 0.14f); sfx(900, 200, 220, WAVE_NOISE, 0.16f); }
static void snd_clink(void) { sfx(2400, 2000, 70, WAVE_SQUARE, 0.12f); sfx_at(3000, 2600, 60, WAVE_SQUARE, 0.08f, 50); }
static void snd_thud(void) { sfx(120, 50, 200, WAVE_NOISE, 0.25f); sfx(90, 45, 180, WAVE_SQUARE, 0.12f); }
static void snd_break(void) { sfx(900, 200, 220, WAVE_NOISE, 0.25f); sfx(250, 90, 160, WAVE_SQUARE, 0.1f); }
static void snd_splash(void) { sfx(1200, 200, 500, WAVE_NOISE, 0.28f); sfx(200, 60, 400, WAVE_TRI, 0.15f); }
static void snd_beacon(void)
{
    static const float n[5] = { 523, 659, 784, 1047, 1319 };
    for (int i = 0; i < 5; i++)
        sfx_at(n[i], n[i], i == 4 ? 400 : 110, WAVE_TRI, 0.18f, i * 90);
    sfx(150, 600, 600, WAVE_NOISE, 0.15f);
}
static void snd_portal(void)
{
    for (int i = 0; i < 8; i++)
        sfx_at(400.f + 120.f * (float)i, 800.f + 200.f * (float)i, 160, WAVE_TRI, 0.13f, i * 70);
}
static void snd_swing(void) { sfx(220, 90, 260, WAVE_NOISE, 0.18f); }
static void snd_dash(void) { sfx(180, 360, 180, WAVE_SAW, 0.10f); }

/* The tunes: four bars of chords (root, as MIDI notes) and a 32-step lead (0 = rest, 1 = hold).
   Written for this game. */
typedef struct {
    int bpm;
    int chord[4];         /* root of each bar */
    int minor[4];         /* 1: a minor chord */
    signed char lead[32];
} song_t;
static const song_t k_song[3] = {
    /* Meadow Isles: bright, in F */
    { 118, { 53, 50, 46, 48 }, { 0, 1, 0, 0 },
      { 77, 1, 79, 81, 1, 79, 77, 74, 76, 1, 77, 79, 1, 1, 72, 0, 74, 1, 77, 76, 74, 1, 72, 70, 72, 1, 74, 76, 1, 1, 0, 0 } },
    /* Sunset Mesas: dusty, in D minor */
    { 104, { 50, 46, 48, 45 }, { 1, 0, 0, 1 },
      { 74, 1, 0, 77, 76, 74, 72, 1, 70, 1, 0, 72, 74, 1, 1, 0, 76, 1, 77, 76, 74, 1, 72, 74, 69, 1, 1, 0, 73, 1, 1, 0 } },
    /* Frostfang Peaks: still, in A minor */
    { 92, { 45, 41, 43, 40 }, { 1, 0, 0, 0 },
      { 81, 1, 1, 79, 76, 1, 1, 0, 77, 1, 76, 74, 72, 1, 1, 0, 79, 1, 1, 77, 74, 1, 1, 0, 76, 1, 1, 71, 1, 1, 0, 0 } },
};
static float midi_hz(int n) { return 440.f * powf(2.f, (float)(n - 69) / 12.f); }

static int audio_main(void *arg)
{
    static int16_t buf[256];
    float wv = 0, fv = 0, cv = 0, lp2 = 0, lp3 = 0, hp = 0;
    /* music state */
    float mus_t = 0;            /* samples into the current step */
    int step = 0;               /* 0..127: 4 bars of 32 sixteenth-ish steps... 8 steps per bar x 4 bars x 4 */
    float ph_b = 0, ph_l = 0, ph_a = 0, f_b = 110, f_l = 0, f_a = 220, env_l = 0, env_b = 0, env_a = 0;
    float lead_lp = 0;
    while (!g_snd_quit && !papp_exit_requested()) {
        papp_mutex_lock(g_snd_lock);
        float tw = s_wind_vol, tf = s_fire_vol, tc = s_charge_vol;
        const song_t *sg = &k_song[s_song < 0 || s_song > 2 ? 0 : s_song];
        float step_len = (float)SR * 60.f / (float)sg->bpm / 4.f;   /* sixteenth notes */
        bool mus = s_music_on;
        float mv = s_music_vol;
        for (int i = 0; i < 256; i++) {
            wv += (tw - wv) * 0.002f;
            fv += (tf - fv) * 0.01f;
            cv += (tc - cv) * 0.004f;
            g_noise = g_noise * 1664525u + 1013904223u;
            float nz = (float)(int32_t)g_noise * (1.f / 2147483648.f);
            float acc = 0;
            /* the wind of the glide: soft low-passed noise */
            lp2 += (nz - lp2) * 0.04f;
            acc += lp2 * wv * 2.2f;
            /* fire: a roar */
            lp3 += (nz - lp3) * 0.25f;
            hp += (lp3 - hp) * 0.02f;
            acc += (lp3 - hp) * fv * 0.9f;
            /* charge: low rumble */
            acc += lp2 * cv * 1.5f;
            /* music */
            if (mus) {
                if (mus_t <= 0) {
                    int bar = (step / 32) & 3, s32 = step & 31;
                    int root = sg->chord[bar];
                    /* bass: root on the beat, fifth on the off beat */
                    if ((step & 3) == 0) {
                        f_b = midi_hz(root - 12 + ((step & 4) ? 7 : 0));
                        env_b = 1.f;
                    }
                    /* arpeggio through the chord */
                    static const int arp_maj[4] = { 0, 4, 7, 12 }, arp_min[4] = { 0, 3, 7, 12 };
                    f_a = midi_hz(root + 12 + (sg->minor[bar] ? arp_min : arp_maj)[step & 3]);
                    env_a = 1.f;
                    /* lead: every other step plays the tune (half speed: 32 notes over 4 bars x 2) */
                    if ((step & 1) == 0) {
                        int n = sg->lead[(s32 / 2 + ((step / 32) & 1) * 16) & 31];
                        if (n > 1) {
                            f_l = midi_hz(n);
                            env_l = 1.f;
                        } else if (n == 0)
                            env_l *= 0.3f;
                    }
                    step = (step + 1) & 127;
                    mus_t += step_len;
                }
                mus_t -= 1.f;
                ph_b += f_b * (1.f / SR);
                if (ph_b >= 1.f) ph_b -= 1.f;
                ph_l += f_l * (1.f / SR);
                if (ph_l >= 1.f) ph_l -= 1.f;
                ph_a += f_a * (1.f / SR);
                if (ph_a >= 1.f) ph_a -= 1.f;
                env_b *= 0.99985f;
                env_l *= 0.99990f;
                env_a *= 0.9994f;
                float bass = (ph_b < 0.5f ? 4.f * ph_b - 1.f : 3.f - 4.f * ph_b) * env_b * 0.16f;
                float lead = (ph_l < 0.25f ? 1.f : -0.33f) * env_l * 0.07f;
                lead_lp += (lead - lead_lp) * 0.35f;
                float arp = (ph_a < 0.5f ? 4.f * ph_a - 1.f : 3.f - 4.f * ph_a) * env_a * 0.035f;
                acc += (bass + lead_lp + arp) * mv;
            }
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
                        : v->wave == WAVE_PULSE  ? (v->ph < 0.2f ? 1.f : -0.3f)
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
#define BMAX_V 16000
#define BMAX_T 16000
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
/* a triangle facing along n */
static void b_tri_n(int a, int b, int c, uint32_t rgb, const float *n)
{
    float ctr[3] = { (B.pos[a][0] + B.pos[b][0] + B.pos[c][0]) / 3.f - n[0], (B.pos[a][1] + B.pos[b][1] + B.pos[c][1]) / 3.f - n[1],
                     (B.pos[a][2] + B.pos[b][2] + B.pos[c][2]) / 3.f - n[2] };
    b_tri(a, b, c, rgb, ctr);
}
static void b_quad(int a, int b, int c, int d, uint32_t rgb, const float ctr[3])
{
    b_tri(a, b, c, rgb, ctr);
    b_tri(a, c, d, rgb, ctr);
}
static void b_quad_n(int a, int b, int c, int d, uint32_t rgb, const float *n)
{
    b_tri_n(a, b, c, rgb, n);
    b_tri_n(a, c, d, rgb, n);
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

/* a box between two corners */
static void b_box(float x0, float y0, float z0, float x1, float y1, float z1, uint32_t rgb)
{
    int v[8];
    for (int i = 0; i < 8; i++)
        v[i] = b_v((i & 1) ? x1 : x0, (i & 2) ? y1 : y0, (i & 4) ? z1 : z0);
    float c[3] = { (x0 + x1) * 0.5f, (y0 + y1) * 0.5f, (z0 + z1) * 0.5f };
    b_quad(v[0], v[1], v[5], v[4], shade_rgb(rgb, 0.8f), c);
    b_quad(v[2], v[3], v[7], v[6], shade_rgb(rgb, 1.08f), c);
    b_quad(v[0], v[1], v[3], v[2], rgb, c);
    b_quad(v[4], v[5], v[7], v[6], rgb, c);
    b_quad(v[0], v[2], v[6], v[4], shade_rgb(rgb, 0.92f), c);
    b_quad(v[1], v[3], v[7], v[5], shade_rgb(rgb, 0.92f), c);
}

/* A tube along x: ring k at (x, cy) with radii ry (up) and rz (across), n points, point 0 on top.
   Colours: the top half `top`, the lower third `bot`. Capped where the radius is not ~0. */
typedef struct { float x, y, ry, rz; } ring_t;
static void b_tube(const ring_t *r, int nr, int n, float zoff, uint32_t top, uint32_t bot, uint32_t (*colfn)(int k, int i, int n))
{
    int base = B.nv;
    for (int k = 0; k < nr; k++)
        for (int i = 0; i < n; i++) {
            float a = 2.f * (float)M_PI * (float)i / (float)n;
            b_v(r[k].x, r[k].y + r[k].ry * cosf(a), zoff + r[k].rz * sinf(a));
        }
    for (int k = 0; k + 1 < nr; k++) {
        float c[3] = { (r[k].x + r[k + 1].x) * 0.5f, (r[k].y + r[k + 1].y) * 0.5f, zoff };
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            float am = 2.f * (float)M_PI * ((float)i + 0.5f) / (float)n;
            uint32_t col = colfn ? colfn(k, i, n) : cosf(am) < -0.35f ? bot : top;
            int a = base + k * n + i, b = base + k * n + j, cc = base + (k + 1) * n + j, d = base + (k + 1) * n + i;
            b_quad(a, b, cc, d, col, c);
        }
    }
    for (int e = 0; e < 2; e++) {
        int k = e ? nr - 1 : 0;
        if (r[k].ry < 0.005f && r[k].rz < 0.005f)
            continue;
        float c[3] = { r[k].x + (e ? -0.05f : 0.05f) * (r[nr - 1].x > r[0].x ? 1.f : -1.f), r[k].y, zoff };
        for (int i = 1; i + 1 < n; i++)
            b_tri(base + k * n, base + k * n + i, base + k * n + i + 1, e ? top : bot, c);
    }
}

/* a solid of revolution round the y axis at (cx, cy, cz): profile points (radius, y), bottom to top,
   n sides; seg_rgb[k] colours the band from profile point k to k+1 */
static void b_lathe(const float (*pr)[2], int np, int n, const uint32_t *seg_rgb, float cx, float cy, float cz)
{
    int base = B.nv;
    for (int k = 0; k < np; k++)
        for (int i = 0; i < n; i++) {
            float a = 2.f * (float)M_PI * (float)i / (float)n;
            b_v(cx + pr[k][0] * cosf(a), cy + pr[k][1], cz + pr[k][0] * sinf(a));
        }
    for (int k = 0; k + 1 < np; k++) {
        float c[3] = { cx, cy + (pr[k][1] + pr[k + 1][1]) * 0.5f, cz };
        /* for an inward-facing band (the inside of a bowl) the "inside point" is outside */
        for (int i = 0; i < n; i++) {
            int j = (i + 1) % n;
            int a = base + k * n + i, b = base + k * n + j, cc = base + (k + 1) * n + j, d = base + (k + 1) * n + i;
            if (pr[k][0] < 0.001f)
                b_tri(a, cc, d, seg_rgb[k], c);
            else if (pr[k + 1][0] < 0.001f)
                b_tri(a, b, cc, seg_rgb[k], c);
            else
                b_quad(a, b, cc, d, seg_rgb[k], c);
        }
    }
    /* caps */
    if (pr[0][0] > 0.001f) {
        float c[3] = { cx, cy + pr[0][1] + 1.f, cz };
        for (int i = 1; i + 1 < n; i++)
            b_tri(base, base + i, base + i + 1, seg_rgb[0], c);
    }
    if (pr[np - 1][0] > 0.001f) {
        int t = base + (np - 1) * n;
        float c[3] = { cx, cy + pr[np - 1][1] - 1.f, cz };
        for (int i = 1; i + 1 < n; i++)
            b_tri(t, t + i, t + i + 1, seg_rgb[np - 2], c);
    }
}

/* an ellipsoid; stripe != 0 colours alternate stacks */
static void b_ellipsoid(float cx, float cy, float cz, float rx, float ry, float rz, int sl, int st, uint32_t rgb, uint32_t stripe)
{
    int base = B.nv;
    int top = b_v(cx, cy + ry, cz);
    for (int j = 1; j < st; j++) {
        float p = (float)M_PI * (float)j / (float)st;
        for (int i = 0; i < sl; i++) {
            float a = 2.f * (float)M_PI * (float)i / (float)sl;
            b_v(cx + rx * sinf(p) * cosf(a), cy + ry * cosf(p), cz + rz * sinf(p) * sinf(a));
        }
    }
    int bot = b_v(cx, cy - ry, cz);
    float c[3] = { cx, cy, cz };
    (void)base;
    for (int i = 0; i < sl; i++) {
        int j = (i + 1) % sl;
        b_tri(top, top + 1 + i, top + 1 + j, rgb, c);
        b_tri(bot, top + 1 + (st - 2) * sl + i, top + 1 + (st - 2) * sl + j, rgb, c);
    }
    for (int k = 0; k + 2 < st; k++)
        for (int i = 0; i < sl; i++) {
            int j = (i + 1) % sl;
            uint32_t col = stripe && (k & 1) ? stripe : rgb;
            b_quad(top + 1 + k * sl + i, top + 1 + k * sl + j, top + 1 + (k + 1) * sl + j, top + 1 + (k + 1) * sl + i, col, c);
        }
}

/* a cone from base centre b (radius r) to tip t, n sides */
static void b_cone(const float *b, float r, const float *t, int n, uint32_t rgb, uint32_t rgb2)
{
    float ax[3] = { t[0] - b[0], t[1] - b[1], t[2] - b[2] };
    v3_norm(ax);
    float u[3], v[3], ref[3] = { 0, 1, 0 };
    if (fabsf(ax[1]) > 0.9f)
        v3_set(ref, 1, 0, 0);
    v3_cross(u, ax, ref);
    v3_norm(u);
    v3_cross(v, ax, u);
    int base = B.nv;
    for (int i = 0; i < n; i++) {
        float a = 2.f * (float)M_PI * (float)i / (float)n, ca = cosf(a) * r, sa = sinf(a) * r;
        b_v(b[0] + u[0] * ca + v[0] * sa, b[1] + u[1] * ca + v[1] * sa, b[2] + u[2] * ca + v[2] * sa);
    }
    int tip = b_v(t[0], t[1], t[2]);
    float c[3] = { b[0] + (t[0] - b[0]) * 0.3f, b[1] + (t[1] - b[1]) * 0.3f, b[2] + (t[2] - b[2]) * 0.3f };
    for (int i = 0; i < n; i++)
        b_tri(base + i, base + (i + 1) % n, tip, (i & 1) ? rgb2 : rgb, c);
    for (int i = 1; i + 1 < n; i++)
        b_tri(base, base + i, base + i + 1, rgb2, c);
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

/* model -> world: translate, then turn by yaw (around y, from +x towards +z), pitch (nose up) and roll */
static e3d_mat mat_pose(const float *p, float yaw, float pitch, float roll, float s)
{
    float cy = cosf(yaw), sy = sinf(yaw), cp = cosf(pitch), sp = sinf(pitch), cr = cosf(roll), sr = sinf(roll);
    /* forward, up, right of the model in the world */
    float f[3] = { cy * cp, sp, sy * cp };
    float u0[3] = { -cy * sp, cp, -sy * sp };
    float r0[3] = { -sy, 0, cy };
    float u[3], r[3];
    for (int i = 0; i < 3; i++) {
        u[i] = u0[i] * cr + r0[i] * sr;
        r[i] = r0[i] * cr - u0[i] * sr;
    }
    e3d_mat m;
    for (int i = 0; i < 3; i++) {
        m.m[i][0] = f[i] * s;
        m.m[i][1] = u[i] * s;
        m.m[i][2] = r[i] * s;
        m.m[i][3] = p[i];
    }
    m.m[3][0] = m.m[3][1] = m.m[3][2] = 0;
    m.m[3][3] = 1;
    return m;
}
/* a local transform: move to (x, y, z) in the parent, then turn (yaw, pitch, roll) */
static e3d_mat mat_child(const e3d_mat *parent, float x, float y, float z, float yaw, float pitch, float roll)
{
    float o[3] = { 0, 0, 0 };
    e3d_mat c = mat_pose(o, yaw, pitch, roll, 1.f);
    c.m[0][3] = x;
    c.m[1][3] = y;
    c.m[2][3] = z;
    e3d_mat m = *parent;
    e3d_mul(&m, &c);
    return m;
}

/* ── Models ──────────────────────────────────────────────────────────────── */
/* the dragon: a deep ember orange with a cream belly, ivory horns and teal wings */
#define C_SCALE 0xE06A24
#define C_SCALE2 0xC2501A
#define C_BELLY 0xF4D9A0
#define C_HORN 0xF3E4B4
#define C_WING 0x2AA59A
#define C_WING2 0x1C7F78
#define C_CLAW 0x3A2A20

enum { DM_BODY, DM_HEAD, DM_WING_R, DM_WING_L, DM_LEG, DM_TAIL, DM_N };
static e3d_mesh *m_dragon[DM_N];
enum { EM_GRUB, EM_BEETLE, EM_BRUTE, EM_CLUB, EM_N };
static e3d_mesh *m_enemy[EM_N];
enum { GEM_AMBER, GEM_EMERALD, GEM_SAPPHIRE, GEM_STAR, NGEMS };
static const int k_gem_value[NGEMS] = { 1, 5, 10, 25 };
static const uint32_t k_gem_rgb[NGEMS][2] = { { 0xFFB020, 0xD07010 }, { 0x30E070, 0x10A040 }, { 0x40A0FF, 0x2050D0 }, { 0xFF70E0, 0xC030B0 } };
static e3d_mesh *m_gem[NGEMS], *m_berry, *m_urn, *m_chest, *m_pillar, *m_bowl, *m_fire, *m_portal_ring, *m_portal_disc, *m_portal_off;
static e3d_mesh *m_puff, *m_spark, *m_blob, *m_ring;

static uint32_t body_col(int k, int i, int n)
{
    float a = 2.f * (float)M_PI * ((float)i + 0.5f) / (float)n, c = cosf(a);
    if (c < -0.3f)
        return C_BELLY;
    if (c > 0.75f)
        return C_SCALE2;   /* a darker ridge along the back */
    return (k + i) & 1 ? C_SCALE : shade_rgb(C_SCALE, 0.94f);
}
static uint32_t tail_col(int k, int i, int n)
{
    float a = 2.f * (float)M_PI * ((float)i + 0.5f) / (float)n;
    return cosf(a) < -0.3f ? C_BELLY : (k & 1) ? C_SCALE : C_SCALE2;
}

static e3d_mesh *make_dragon_part(int part)
{
    b_reset();
    if (part == DM_BODY) {
        /* torso: the hips at x -0.42, the chest at 0.34; the feet are at y 0 */
        static const ring_t r[] = { { -0.48f, 0.50f, 0.10f, 0.10f }, { -0.40f, 0.50f, 0.20f, 0.19f }, { -0.20f, 0.49f, 0.24f, 0.23f },
                                    { 0.02f, 0.50f, 0.25f, 0.23f },  { 0.22f, 0.53f, 0.23f, 0.21f }, { 0.36f, 0.58f, 0.17f, 0.15f },
                                    { 0.42f, 0.62f, 0.08f, 0.08f } };
        b_tube(r, NC(r), 12, 0, C_SCALE, C_BELLY, body_col);
        /* small spines along the back */
        for (int i = 0; i < 3; i++) {
            float x = -0.26f + 0.2f * (float)i, y = 0.73f;
            float b0[3] = { x + 0.04f, y - 0.03f, 0 }, t[3] = { x - 0.05f, y + 0.08f, 0 };
            b_cone(b0, 0.035f, t, 4, C_HORN, shade_rgb(C_HORN, 0.8f));
        }
    } else if (part == DM_HEAD) {
        /* neck and a big head, from the neck's base (0, 0, 0) at the chest */
        static const ring_t r[] = { { -0.02f, -0.02f, 0.13f, 0.12f }, { 0.06f, 0.12f, 0.11f, 0.10f }, { 0.13f, 0.24f, 0.13f, 0.12f },
                                    { 0.21f, 0.33f, 0.19f, 0.17f },  { 0.35f, 0.34f, 0.18f, 0.165f }, { 0.48f, 0.29f, 0.12f, 0.125f },
                                    { 0.58f, 0.26f, 0.085f, 0.095f }, { 0.62f, 0.25f, 0.04f, 0.05f } };
        b_tube(r, NC(r), 12, 0, C_SCALE, C_BELLY, body_col);
        for (int s = -1; s <= 1; s += 2) {
            float fs = (float)s;
            /* horns: swept back and up */
            float hb[3] = { 0.27f, 0.47f, 0.09f * fs }, ht[3] = { 0.04f, 0.64f, 0.17f * fs };
            b_cone(hb, 0.055f, ht, 6, C_HORN, shade_rgb(C_HORN, 0.85f));
            /* a small frill behind the jaw */
            float fb[3] = { 0.20f, 0.30f, 0.15f * fs }, ft[3] = { 0.05f, 0.33f, 0.25f * fs };
            b_cone(fb, 0.045f, ft, 4, C_WING, C_WING2);
            /* big eyes with dark pupils, looking forward */
            float ex = 0.39f, ey = 0.41f, ez = 0.13f * fs;
            b_ellipsoid(ex, ey, ez, 0.07f, 0.065f, 0.04f, 7, 5, 0xFFFFF0, 0);
            b_ellipsoid(ex + 0.045f, ey + 0.005f, ez + 0.016f * fs, 0.028f, 0.042f, 0.024f, 6, 4, 0x182028, 0);
            b_ellipsoid(ex + 0.06f, ey + 0.025f, ez + 0.02f * fs, 0.01f, 0.012f, 0.008f, 4, 3, 0xFFFFFF, 0);
        }
        /* a darker mouth line */
        b_box(0.42f, 0.205f, -0.085f, 0.54f, 0.22f, 0.085f, 0x6A2410);
    } else if (part == DM_WING_R || part == DM_WING_L) {
        float s = part == DM_WING_R ? 1.f : -1.f;
        /* the membrane, out along +z (right), swept back, from the shoulder at the origin */
        static const float P[][3] = { { 0.10f, 0.0f, 0.02f },  { 0.02f, 0.10f, 0.45f }, { -0.10f, 0.16f, 0.88f },
                                      { -0.30f, 0.05f, 0.70f }, { -0.36f, 0.02f, 0.45f }, { -0.36f, 0.0f, 0.18f },
                                      { -0.24f, 0.0f, 0.02f } };
        int v[7];
        for (int i = 0; i < 7; i++)
            v[i] = b_v(P[i][0], P[i][1], P[i][2] * s);
        float n[3] = { 0, 1, 0 };
        b_tri_n(v[0], v[1], v[6], C_WING, n);
        b_tri_n(v[1], v[5], v[6], C_WING2, n);
        b_tri_n(v[1], v[4], v[5], C_WING, n);
        b_tri_n(v[1], v[2], v[3], C_WING2, n);
        b_tri_n(v[1], v[3], v[4], C_WING, n);
        /* the arm bone along the leading edge and the finger tip */
        float a0[3] = { 0.10f, 0.01f, 0.0f }, a1[3] = { 0.02f, 0.12f, 0.46f * s }, a2[3] = { -0.10f, 0.17f, 0.90f * s };
        b_cone(a0, 0.035f, a1, 4, C_SCALE, C_SCALE2);
        b_cone(a1, 0.025f, a2, 4, C_SCALE, C_SCALE2);
    } else if (part == DM_LEG) {
        /* from the hip (0, 0, 0) down to the foot at y -0.42 */
        static const float pr[][2] = { { 0.0f, -0.34f }, { 0.07f, -0.34f }, { 0.08f, -0.2f }, { 0.12f, -0.03f }, { 0.0f, 0.08f } };
        static const uint32_t col[4] = { C_SCALE2, C_SCALE2, C_SCALE, C_SCALE };
        b_lathe(pr, 5, 8, col, 0, 0, 0);
        /* the foot and three claws */
        b_box(-0.07f, -0.40f, -0.075f, 0.12f, -0.32f, 0.075f, C_SCALE2);
        for (int c = -1; c <= 1; c++) {
            float cb[3] = { 0.11f, -0.38f, 0.045f * (float)c }, ct[3] = { 0.18f, -0.40f, 0.055f * (float)c };
            b_cone(cb, 0.02f, ct, 3, C_HORN, C_HORN);
        }
    } else if (part == DM_TAIL) {
        /* from the tail's base backwards (-x), curling up a little */
        static const ring_t r[] = { { 0.0f, 0.0f, 0.11f, 0.10f },    { -0.18f, -0.02f, 0.09f, 0.08f }, { -0.36f, -0.02f, 0.07f, 0.06f },
                                    { -0.54f, 0.02f, 0.05f, 0.045f }, { -0.70f, 0.08f, 0.035f, 0.03f }, { -0.82f, 0.15f, 0.02f, 0.02f } };
        b_tube(r, NC(r), 8, 0, C_SCALE, C_BELLY, tail_col);
        /* the arrow tip: a flat diamond */
        int a = b_v(-0.80f, 0.15f, 0), b = b_v(-0.92f, 0.19f, 0.10f), c = b_v(-1.08f, 0.24f, 0), d = b_v(-0.92f, 0.19f, -0.10f);
        int t = b_v(-0.92f, 0.23f, 0), u = b_v(-0.92f, 0.15f, 0);
        float ctr[3] = { -0.92f, 0.19f, 0 };
        b_tri(a, b, t, C_HORN, ctr);
        b_tri(b, c, t, shade_rgb(C_HORN, 0.9f), ctr);
        b_tri(c, d, t, C_HORN, ctr);
        b_tri(d, a, t, shade_rgb(C_HORN, 0.9f), ctr);
        b_tri(a, b, u, shade_rgb(C_HORN, 0.8f), ctr);
        b_tri(b, c, u, shade_rgb(C_HORN, 0.75f), ctr);
        b_tri(c, d, u, shade_rgb(C_HORN, 0.8f), ctr);
        b_tri(d, a, u, shade_rgb(C_HORN, 0.75f), ctr);
    }
    return b_bake(part == DM_WING_R || part == DM_WING_L ? 20.f : 55.f);
}

static e3d_mesh *make_enemy(int type)
{
    b_reset();
    if (type == EM_GRUB) {
        /* a fat caterpillar: three segments, eyes on stalks; faces +x */
        b_ellipsoid(-0.45f, 0.30f, 0, 0.32f, 0.28f, 0.30f, 8, 5, 0x7AC23A, 0xE8D040);
        b_ellipsoid(-0.05f, 0.36f, 0, 0.34f, 0.34f, 0.34f, 9, 6, 0x86CE40, 0xE8D040);
        b_ellipsoid(0.32f, 0.44f, 0, 0.28f, 0.30f, 0.28f, 8, 5, 0x96D84A, 0);
        for (int s = -1; s <= 1; s += 2) {
            float fs = (float)s;
            float b0[3] = { 0.36f, 0.66f, 0.10f * fs }, t[3] = { 0.42f, 0.98f, 0.20f * fs };
            b_cone(b0, 0.03f, t, 4, 0x405020, 0x405020);
            b_ellipsoid(0.43f, 1.0f, 0.21f * fs, 0.08f, 0.08f, 0.08f, 6, 4, 0xFFFFFF, 0);
            b_ellipsoid(0.50f, 1.0f, 0.22f * fs, 0.035f, 0.045f, 0.04f, 5, 3, 0x101010, 0);
        }
        b_box(0.55f, 0.34f, -0.12f, 0.60f, 0.40f, 0.12f, 0x3A1A10);   /* mouth */
    } else if (type == EM_BEETLE) {
        /* an armoured beetle: a steel-blue shell with a gold seam and a horn */
        b_ellipsoid(0, 0.46f, 0, 0.62f, 0.38f, 0.48f, 10, 6, 0x3A5A9A, 0x2A4478);
        b_box(-0.62f, 0.78f, -0.03f, 0.55f, 0.86f, 0.03f, 0xE0B040);
        b_ellipsoid(0.62f, 0.36f, 0, 0.22f, 0.18f, 0.22f, 7, 4, 0x20242C, 0);
        float hb[3] = { 0.75f, 0.42f, 0 }, ht[3] = { 1.05f, 0.75f, 0 };
        b_cone(hb, 0.07f, ht, 5, 0xE8E0C8, 0xB0A890);
        for (int s = -1; s <= 1; s += 2)
            for (int l = -1; l <= 1; l++) {
                float x = 0.3f * (float)l, z = 0.42f * (float)s;
                b_box(x - 0.04f, 0.0f, z - 0.04f + 0.12f * (float)s, x + 0.04f, 0.32f, z + 0.04f + 0.12f * (float)s, 0x181818);
            }
        for (int s = -1; s <= 1; s += 2)
            b_ellipsoid(0.78f, 0.44f, 0.11f * (float)s, 0.04f, 0.05f, 0.05f, 5, 3, 0xFF4020, 0);
    } else if (type == EM_BRUTE) {
        /* a big lumbering brute: a round belly, a small head with tusks, long arms */
        uint32_t skin = 0x8A7A4A, skin2 = 0x6E5E36, cloth = 0x7A3A28;
        b_ellipsoid(0, 1.25f, 0, 0.75f, 0.85f, 0.78f, 10, 7, skin, 0);
        b_ellipsoid(0, 0.75f, 0, 0.72f, 0.38f, 0.74f, 10, 4, cloth, 0);       /* the loincloth band */
        b_ellipsoid(0.38f, 2.12f, 0, 0.36f, 0.34f, 0.36f, 8, 5, skin2, 0);   /* head */
        for (int s = -1; s <= 1; s += 2) {
            float fs = (float)s;
            float tb[3] = { 0.66f, 2.0f, 0.14f * fs }, tt[3] = { 0.86f, 2.28f, 0.2f * fs };
            b_cone(tb, 0.05f, tt, 4, 0xF4F0E0, 0xD8D0C0);
            b_ellipsoid(0.66f, 2.24f, 0.14f * fs, 0.06f, 0.05f, 0.06f, 5, 3, 0xFFE040, 0);
            /* legs */
            b_box(-0.25f, 0.0f, 0.18f * fs - 0.2f, 0.25f, 0.6f, 0.18f * fs + 0.2f, skin2);
            b_box(-0.15f, 0.0f, 0.18f * fs - 0.22f, 0.40f, 0.14f, 0.18f * fs + 0.22f, 0x3A2A1A);
        }
        /* the left arm (the club is a separate mesh in the right hand) */
        b_box(0.1f, 0.8f, -0.95f, 0.38f, 1.75f, -0.68f, skin2);
        b_box(0.1f, 0.8f, 0.68f, 0.38f, 1.75f, 0.95f, skin2);
    } else if (type == EM_CLUB) {
        /* from the grip (origin) up +y */
        static const float pr[][2] = { { 0.0f, -0.1f }, { 0.07f, -0.08f }, { 0.08f, 0.5f }, { 0.20f, 1.0f }, { 0.22f, 1.2f }, { 0.0f, 1.3f } };
        static const uint32_t col[5] = { 0x6A4A2A, 0x6A4A2A, 0x7A5A34, 0x86643A, 0x86643A };
        b_lathe(pr, 6, 7, col, 0, 0, 0);
    }
    return b_bake(55.f);
}

static e3d_mesh *make_gem(int type)
{
    b_reset();
    /* a cut gem: a low crown and a long pavilion, 6 sides */
    int n = 6;
    int top = b_v(0, 0.28f, 0), bot = b_v(0, -0.42f, 0), ring = B.nv;
    for (int i = 0; i < n; i++) {
        float a = 2.f * (float)M_PI * (float)i / (float)n;
        b_v(0.30f * cosf(a), 0.08f, 0.30f * sinf(a));
    }
    float c[3] = { 0, 0, 0 };
    for (int i = 0; i < n; i++) {
        int j = (i + 1) % n;
        b_tri(top, ring + i, ring + j, (i & 1) ? k_gem_rgb[type][0] : shade_rgb(k_gem_rgb[type][0], 1.25f), c);
        b_tri(bot, ring + i, ring + j, (i & 1) ? k_gem_rgb[type][1] : k_gem_rgb[type][0], c);
    }
    if (type == GEM_STAR) {
        /* a bigger, star-shaped one: points out of the ring */
        for (int i = 0; i < n; i++) {
            float a = 2.f * (float)M_PI * ((float)i + 0.5f) / (float)n;
            float b0[3] = { 0.18f * cosf(a), 0.06f, 0.18f * sinf(a) }, t[3] = { 0.5f * cosf(a), 0.06f, 0.5f * sinf(a) };
            b_cone(b0, 0.08f, t, 3, k_gem_rgb[type][0], 0xFFFFFF);
        }
    }
    return b_bake(5.f);
}

static bool make_models(void)
{
    for (int i = 0; i < DM_N; i++)
        if (!(m_dragon[i] = make_dragon_part(i)))
            return false;
    for (int i = 0; i < EM_N; i++)
        if (!(m_enemy[i] = make_enemy(i)))
            return false;
    for (int i = 0; i < NGEMS; i++)
        if (!(m_gem[i] = make_gem(i)))
            return false;
    /* a heart berry: red with a leaf */
    b_reset();
    b_ellipsoid(0, 0.25f, 0, 0.26f, 0.24f, 0.26f, 8, 6, 0xE02040, 0xFF5070);
    {
        float lb[3] = { 0, 0.45f, 0 }, lt[3] = { 0.22f, 0.6f, 0 };
        b_cone(lb, 0.07f, lt, 3, 0x40C040, 0x309030);
    }
    m_berry = b_bake(40.f);
    /* an urn (fire or charge breaks it) */
    b_reset();
    {
        static const float pr[][2] = { { 0.0f, 0.0f }, { 0.28f, 0.0f }, { 0.42f, 0.30f }, { 0.44f, 0.55f }, { 0.30f, 0.85f }, { 0.22f, 0.95f }, { 0.30f, 1.05f }, { 0.0f, 1.05f } };
        static const uint32_t col[7] = { 0x9A4A28, 0xB85A30, 0xD8B060, 0xB85A30, 0xB85A30, 0xD8B060, 0x5A2A18 };
        b_lathe(pr, 8, 10, col, 0, 0, 0);
    }
    m_urn = b_bake(50.f);
    /* an iron chest (charge it) */
    b_reset();
    b_box(-0.55f, 0, -0.4f, 0.55f, 0.65f, 0.4f, 0x5A5E66);
    b_box(-0.58f, 0.62f, -0.43f, 0.58f, 0.82f, 0.43f, 0x6A6E78);
    for (int i = -1; i <= 1; i += 2)
        b_box(0.25f * (float)i - 0.06f, -0.01f, -0.44f, 0.25f * (float)i + 0.06f, 0.84f, 0.44f, 0xD0A030);
    b_box(0.56f, 0.40f, -0.08f, 0.62f, 0.60f, 0.08f, 0xD0A030);
    m_chest = b_bake(30.f);
    /* the beacon: a stone pillar and a bowl */
    b_reset();
    {
        static const float pr[][2] = { { 0.75f, 0.0f }, { 0.75f, 0.3f }, { 0.48f, 0.4f }, { 0.42f, 2.0f }, { 0.55f, 2.15f }, { 0.55f, 2.3f }, { 0.0f, 2.3f } };
        static const uint32_t col[6] = { 0x8A8478, 0x9A9488, 0xA8A294, 0x8A8478, 0x9A9488, 0x7A7468 };
        b_lathe(pr, 7, 8, col, 0, 0, 0);
    }
    m_pillar = b_bake(40.f);
    b_reset();
    {
        static const float pr[][2] = { { 0.3f, 2.3f }, { 0.85f, 2.55f }, { 0.95f, 2.85f }, { 0.0f, 2.75f } };
        static const uint32_t col[3] = { 0x6A5A44, 0x8A7A5A, 0x2A2018 };
        b_lathe(pr, 4, 10, col, 0, 0, 0);
    }
    m_bowl = b_bake(40.f);
    /* the beacon's fire: three tongues (drawn unlit) */
    b_reset();
    for (int i = 0; i < 3; i++) {
        float a = 2.1f * (float)i;
        float b0[3] = { 0.25f * cosf(a), 0, 0.25f * sinf(a) }, t[3] = { 0.1f * cosf(a), 1.2f - 0.25f * (float)i, 0.1f * sinf(a) };
        b_cone(b0, 0.32f, t, 5, i == 0 ? 0xFFE060 : 0xFFA020, 0xFF6010);
    }
    m_fire = b_bake(80.f);
    /* the portal: an arch of 11 stones round a disc */
    b_reset();
    for (int i = 0; i < 11; i++) {
        float a = (float)M_PI * (-0.1f + 1.2f * (float)i / 10.f);
        float x = 2.2f * cosf(a), y = 2.2f * sinf(a) + 1.6f;
        int v[8];
        float ca = cosf(a), sa = sinf(a);
        for (int k = 0; k < 8; k++) {
            float rr = (k & 1) ? 0.55f : -0.1f, tt = (k & 2) ? 0.42f : -0.42f, zz = (k & 4) ? 0.4f : -0.4f;
            /* radial (rr), tangential (tt) and depth (zz); the portal faces +x */
            v[k] = b_v(zz, y + sa * rr + ca * tt, x + ca * rr - sa * tt);
        }
        float c[3] = { 0, y + sa * 0.22f, x + ca * 0.22f };
        uint32_t st = (i & 1) ? 0x9A9488 : 0xB0AA9C;
        b_quad(v[0], v[1], v[3], v[2], st, c);
        b_quad(v[4], v[5], v[7], v[6], st, c);
        b_quad(v[0], v[1], v[5], v[4], shade_rgb(st, 0.85f), c);
        b_quad(v[2], v[3], v[7], v[6], shade_rgb(st, 0.85f), c);
        b_quad(v[0], v[2], v[6], v[4], shade_rgb(st, 0.75f), c);
        b_quad(v[1], v[3], v[7], v[5], shade_rgb(st, 1.05f), c);
    }
    b_box(-0.9f, -0.2f, -3.0f, 0.9f, 0.15f, 3.0f, 0x8A8478);   /* the step */
    m_portal_ring = b_bake(30.f);
    /* the portal's swirl: a disc of 24 slices in two colours (drawn unlit); dim until the portal opens */
    for (int k = 0; k < 2; k++) {
        b_reset();
        int c0 = b_v(0, 1.75f, 0), ring = B.nv;
        for (int i = 0; i < 24; i++) {
            float a = 2.f * (float)M_PI * (float)i / 24.f;
            b_v(0, 1.75f + 2.05f * sinf(a), 2.05f * cosf(a));
        }
        float n[3] = { 1, 0, 0 };
        for (int i = 0; i < 24; i++) {
            uint32_t c = k == 0 ? ((i & 1) ? 0xB060FF : 0x40E0FF) : ((i & 1) ? 0x2A3248 : 0x343E58);
            if (k == 0 && (i % 6) == 0)
                c = 0xFFFFFF;
            b_tri_n(c0, ring + i, ring + (i + 1) % 24, c, n);
        }
        if (k == 0)
            m_portal_disc = b_bake(1.f);
        else
            m_portal_off = b_bake(1.f);
    }
    /* a fire puff: an octahedron */
    b_reset();
    {
        int v[6] = { b_v(0.5f, 0, 0), b_v(-0.5f, 0, 0), b_v(0, 0.5f, 0), b_v(0, -0.5f, 0), b_v(0, 0, 0.5f), b_v(0, 0, -0.5f) };
        float ctr[3] = { 0, 0, 0 };
        for (int i = 0; i < 8; i++)
            b_tri(v[i & 1], v[2 + ((i >> 1) & 1)], v[4 + ((i >> 2) & 1)], (i & 1) ? 0xFFFFFF : 0xD0D0D0, ctr);
    }
    m_puff = b_bake(10.f);
    /* a spark: a tetrahedron */
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
    /* the shadow: a dark 10-sided disc */
    b_reset();
    {
        int c0 = b_v(0, 0, 0), ring = B.nv;
        for (int i = 0; i < 10; i++) {
            float a = 2.f * (float)M_PI * (float)i / 10.f;
            b_v(cosf(a), 0, sinf(a));
        }
        float n[3] = { 0, 1, 0 };
        for (int i = 0; i < 10; i++)
            b_tri_n(c0, ring + i, ring + (i + 1) % 10, 0x101010, n);
    }
    m_blob = b_bake(10.f);
    /* a ring (the sparkle round a collected gem) */
    b_reset();
    {
        int base = B.nv;
        for (int i = 0; i < 12; i++) {
            float a = 2.f * (float)M_PI * (float)i / 12.f;
            b_v(cosf(a), 0, sinf(a));
            b_v(0.75f * cosf(a), 0, 0.75f * sinf(a));
        }
        float n[3] = { 0, 1, 0 };
        for (int i = 0; i < 12; i++) {
            int j = (i + 1) % 12;
            b_quad_n(base + 2 * i, base + 2 * j, base + 2 * j + 1, base + 2 * i + 1, 0xFFFFFF, n);
        }
    }
    m_ring = b_bake(10.f);
    return m_berry && m_urn && m_chest && m_pillar && m_bowl && m_fire && m_portal_ring && m_portal_disc && m_portal_off && m_puff &&
           m_spark && m_blob && m_ring;
}

static void free_models(void)
{
    for (int i = 0; i < DM_N; i++)
        e3d_mesh_free(m_dragon[i]);
    for (int i = 0; i < EM_N; i++)
        e3d_mesh_free(m_enemy[i]);
    for (int i = 0; i < NGEMS; i++)
        e3d_mesh_free(m_gem[i]);
    e3d_mesh *all[] = { m_berry, m_urn, m_chest, m_pillar, m_bowl, m_fire, m_portal_ring, m_portal_disc, m_portal_off, m_puff, m_spark, m_blob, m_ring };
    for (int i = 0; i < NC(all); i++)
        e3d_mesh_free(all[i]);
}

/* ── The worlds ──────────────────────────────────────────────────────────────
 * Each world is a list of features that make its height field (metres; x and z on the
 * plan, y up, the sea at y = 0), floating stones, gems, creatures, urns, chests, berries,
 * four beacons, the start and the portal. */
#define SEA 0.f
enum { F_ISLE, F_RAMP, F_PIT };
typedef struct { uint8_t k; float a[7]; } feat_t;
/* ISLE: cx cz rx rz h edge (share of the radius that slopes)
   RAMP: x0 z0 x1 z1 halfwidth h0 h1
   PIT:  cx cz r h edge */
typedef struct { float x, z, y, r, bob, period; } plat_def_t;       /* y: the top */
typedef struct { float x0, z0, x1, z1; int n; uint8_t type; float y0, y1; } gemline_t;   /* y0 = 0: on the ground */
typedef struct { float x, z; uint8_t kind; } thing_t;
enum { T_GRUB, T_BEETLE, T_BRUTE, T_URN, T_CHEST, T_BERRY };

enum { SEA_WATER, SEA_LAVA, SEA_ICE };
typedef struct {
    uint32_t sky_top, sky_mid, sky_hz, fog, cloud, cloud2, mount, mount2, sun;
    uint32_t sea, sea_hi, shallow;
    int sea_kind;
    uint32_t grass, grass2, cliff, cliff2, sand, path, path2, high;
    float high_y;                    /* above this the land is the high colour (snow) */
    uint32_t light_rgb;
    float light_dir[3];              /* towards the light */
    float ambient, fog_density;
    bool night;
    uint32_t leaf, leaf2, trunk;
    int tree_kind;                   /* 0 round trees, 1 rock spires, 2 pines */
    int sun_az, sun_up, sun_r;       /* the sun (or moon) on the sky: azimuth degrees, height and radius in rows at 240 */
} look_t;

typedef struct {
    const char *name;
    const look_t *look;
    const feat_t *feat; int nfeat;
    const plat_def_t *plat; int nplat;
    const gemline_t *gems; int ngems;
    const thing_t *things; int nthings;
    float beacon[4][2];
    float spawn[3];                  /* x, z, yaw */
    float portal[3];
    int ntrees;
    uint32_t seed;
} level_t;

/* ── World 1: Meadow Isles ── */
static const look_t k_look_meadow = {
    .sky_top = 0x2E6FD8, .sky_mid = 0x5FA0F0, .sky_hz = 0xBFE0F8, .fog = 0xB8D8F0, .cloud = 0xFFFFFF, .cloud2 = 0xDCE8F8,
    .mount = 0x7090B8, .mount2 = 0x98B4D4, .sun = 0xFFF8D0,
    .sea = 0x1E78C0, .sea_hi = 0x8CD8F8, .shallow = 0x3CB0D0, .sea_kind = SEA_WATER,
    .grass = 0x58B030, .grass2 = 0x6CC23A, .cliff = 0xA08060, .cliff2 = 0x8A6C4E, .sand = 0xE8D498, .path = 0xC8A070,
    .path2 = 0xB88E60, .high = 0x80C850, .high_y = 99.f,
    .light_rgb = 0xFFF4E0, .light_dir = { 0.45f, 0.8f, 0.35f }, .ambient = 0.46f, .fog_density = 0.0038f, .night = false,
    .leaf = 0x3E9A2A, .leaf2 = 0x56B236, .trunk = 0x7A5230, .tree_kind = 0, .sun_az = 40, .sun_up = 120, .sun_r = 11,
};
static const feat_t k_feat_meadow[] = {
    { F_ISLE, { 0, 0, 72, 60, 1.5f, 0.28f } },
    { F_ISLE, { 28, -12, 20, 17, 6.5f, 0.10f } },
    { F_ISLE, { 32, -14, 9, 9, 11.f, 0.22f } },
    { F_RAMP, { -4, -14, 11, -12, 2.6f, 1.5f, 6.6f } },
    { F_RAMP, { 20, 0, 31, -8, 2.4f, 6.5f, 11.1f } },
    { F_ISLE, { -80, 35, 15, 13, 2.5f, 0.30f } },
    { F_ISLE, { 50, -62, 13, 12, 4.f, 0.30f } },
    { F_ISLE, { 88, 30, 16, 14, 2.f, 0.30f } },
    { F_ISLE, { -38, 32, 10, 8, 3.0f, 0.5f } },      /* a gentle knoll */
    { F_PIT, { -22, 25, 9, -3.f, 0.5f } },           /* a pond */
};
static const plat_def_t k_plat_meadow[] = {
    { -50.5f, 31, 1.2f, 2.4f, 0, 0 }, { -57, 33, 1.9f, 2.3f, 0, 0 }, { -63.5f, 35, 2.6f, 2.3f, 0.5f, 3.5f },
    { 53, 25, 1.2f, 2.4f, 0, 0 }, { 59.5f, 26, 1.9f, 2.3f, 0.8f, 3.0f }, { 66, 27, 2.5f, 2.3f, 0.8f, 3.6f }, { 72, 28, 2.6f, 2.3f, 0, 0 },
    { -22, 25, 0.9f, 2.0f, 0, 0 },                   /* the stone in the pond */
};
static const gemline_t k_gems_meadow[] = {
    { -12, -2, 10, -2, 6, GEM_AMBER, 0, 0 },
    { -3, -14, 10, -12, 5, GEM_AMBER, 0, 0 },
    { 20, 0, 30, -7, 4, GEM_AMBER, 0, 0 },
    { 32, -14, 32, -14, 1, GEM_STAR, 0, 0 },
    { 18, -18, 36, -6, 4, GEM_EMERALD, 0, 0 },
    { -50.5f, 31, -63.5f, 35, 3, GEM_EMERALD, 0, 0 },
    { -53.7f, 32, -60.3f, 34, 2, GEM_AMBER, 3.6f, 4.3f },
    { -78, 32, -84, 38, 3, GEM_SAPPHIRE, 0, 0 },
    { 40, -28, 48, -48, 4, GEM_AMBER, 9.f, 6.f },
    { 50, -62, 50, -62, 1, GEM_STAR, 0, 0 },
    { 53, 25, 72, 28, 4, GEM_SAPPHIRE, 0, 0 },
    { 82, 28, 94, 34, 4, GEM_AMBER, 0, 0 },
    { -30, 16, -14, 16, 5, GEM_AMBER, 0, 0 },
    { -22, 25, -22, 25, 1, GEM_SAPPHIRE, 0, 0 },
    { 0, 30, 30, 30, 6, GEM_AMBER, 0, 0 },
    { -40, -30, -10, -40, 6, GEM_AMBER, 0, 0 },
    { -50, 0, -50, 20, 4, GEM_AMBER, 0, 0 },
    { -38, 32, -38, 32, 1, GEM_EMERALD, 0, 0 },
    { 45, 5, 55, -10, 4, GEM_AMBER, 0, 0 },
    { 10, 45, 30, 45, 3, GEM_EMERALD, 0, 0 },
};
static const thing_t k_things_meadow[] = {
    { -5, 10, T_GRUB }, { 5, 25, T_GRUB }, { -35, -25, T_GRUB }, { 15, 32, T_GRUB }, { -40, 5, T_GRUB }, { 40, 25, T_GRUB },
    { 50, -10, T_GRUB },
    { -10, -35, T_BEETLE }, { 45, 10, T_BEETLE }, { -80, 30, T_BEETLE },
    { 26, -18, T_BRUTE }, { 87, 25, T_BRUTE },
    { -40, -15, T_URN }, { 0, 42, T_URN }, { 18, -16, T_URN }, { -84, 41, T_URN }, { 54, -66, T_URN }, { 30, 40, T_URN },
    { 36, -10, T_CHEST }, { 93, 35, T_CHEST }, { -30, 36, T_CHEST },
    { 10, 5, T_BERRY }, { -40, 0, T_BERRY }, { 84, 24, T_BERRY }, { 28, -6, T_BERRY },
};
static const level_t k_level_meadow = {
    "MEADOW ISLES", &k_look_meadow, k_feat_meadow, NC(k_feat_meadow), k_plat_meadow, NC(k_plat_meadow),
    k_gems_meadow, NC(k_gems_meadow), k_things_meadow, NC(k_things_meadow),
    { { 34, -16 }, { -82, 37 }, { 51, -63 }, { 90, 31 } },
    { -20, -2, 0 }, { -30, -2, 0 }, 80, 11,
};

/* ── World 2: Sunset Mesas ── */
static const look_t k_look_mesa = {
    .sky_top = 0x3A2A6A, .sky_mid = 0xC0506A, .sky_hz = 0xFFB070, .fog = 0xE89068, .cloud = 0xFFC8A0, .cloud2 = 0xE08A80,
    .mount = 0x6A3050, .mount2 = 0x9A4A5A, .sun = 0xFFE8A0,
    .sea = 0xC03008, .sea_hi = 0xFFC030, .shallow = 0xFF6A10, .sea_kind = SEA_LAVA,
    .grass = 0xD89A58, .grass2 = 0xC88A4C, .cliff = 0xB0482A, .cliff2 = 0x983A22, .sand = 0x5A3020, .path = 0xA88A6A,
    .path2 = 0x98785A, .high = 0xE0B070, .high_y = 99.f,
    .light_rgb = 0xFFD8B0, .light_dir = { -0.6f, 0.45f, 0.45f }, .ambient = 0.44f, .fog_density = 0.0042f, .night = false,
    .leaf = 0x4A8A3A, .leaf2 = 0x3A7A30, .trunk = 0x8A5A3A, .tree_kind = 1, .sun_az = 200, .sun_up = 26, .sun_r = 18,
};
static const feat_t k_feat_mesa[] = {
    { F_ISLE, { 0, 0, 75, 62, 1.0f, 0.22f } },
    { F_ISLE, { -32, -22, 15, 14, 6.f, 0.07f } },
    { F_ISLE, { 22, -34, 13, 16, 10.f, 0.07f } },
    { F_ISLE, { 38, 22, 16, 13, 5.f, 0.08f } },
    { F_ISLE, { -36, 30, 10, 10, 14.f, 0.10f } },
    { F_ISLE, { 70, -62, 11, 10, 3.f, 0.25f } },
    { F_RAMP, { -8, -6, -22, -15, 2.6f, 1.0f, 6.1f } },
    { F_RAMP, { -20, -26, 12, -33, 2.2f, 6.0f, 10.1f } },
    { F_RAMP, { 18, 10, 28, 18, 2.6f, 1.0f, 5.1f } },
    { F_PIT, { 5, 30, 10, -4.f, 0.4f } },
};
static const plat_def_t k_plat_mesa[] = {
    { -20, 22, 2.6f, 2.6f, 0, 0 }, { -18, 30, 4.0f, 2.6f, 0, 0 }, { -22, 38, 5.5f, 2.6f, 0, 0 }, { -29, 44, 7.0f, 2.6f, 0, 0 },
    { -38, 46, 8.5f, 2.6f, 0, 0 }, { -46, 41, 10.0f, 2.6f, 0, 0 }, { -49, 33, 11.5f, 2.6f, 0.4f, 4.f }, { -47, 24, 13.0f, 2.6f, 0, 0 },
    { 5, 30, 2.2f, 2.5f, 0.5f, 3.f },
};
static const gemline_t k_gems_mesa[] = {
    { -5, 0, 15, 0, 6, GEM_AMBER, 0, 0 },
    { -8, -6, -21, -14, 5, GEM_AMBER, 0, 0 },
    { -18, -26, 10, -32, 7, GEM_AMBER, 0, 0 },
    { -32, -22, -32, -22, 1, GEM_SAPPHIRE, 0, 0 },
    { 22, -34, 22, -34, 1, GEM_STAR, 0, 0 },
    { 18, 10, 28, 18, 4, GEM_AMBER, 0, 0 },
    { 38, 22, 44, 18, 2, GEM_EMERALD, 0, 0 },
    { -20, 22, -47, 24, 8, GEM_EMERALD, 0, 0 },
    { -36, 30, -36, 30, 1, GEM_STAR, 0, 0 },
    { 32, -44, 62, -58, 5, GEM_AMBER, 8.5f, 4.5f },
    { 70, -62, 70, -62, 1, GEM_SAPPHIRE, 0, 0 },
    { 5, 30, 5, 30, 1, GEM_SAPPHIRE, 0, 0 },
    { -50, -5, -50, 15, 4, GEM_AMBER, 0, 0 },
    { 30, 45, 55, 40, 5, GEM_AMBER, 0, 0 },
    { -20, 45, 0, 50, 4, GEM_AMBER, 0, 0 },
    { 50, -5, 60, 10, 3, GEM_EMERALD, 0, 0 },
};
static const thing_t k_things_mesa[] = {
    { 10, 8, T_GRUB }, { -15, 10, T_GRUB }, { 30, -10, T_GRUB }, { -45, 0, T_GRUB }, { 50, 0, T_GRUB }, { 20, 45, T_GRUB },
    { 0, -20, T_BEETLE }, { -55, 15, T_BEETLE }, { 55, 35, T_BEETLE }, { 15, -30, T_BEETLE },
    { -30, -20, T_BRUTE }, { 40, 25, T_BRUTE }, { 25, -38, T_BRUTE }, { -10, 40, T_BRUTE },
    { -35, -25, T_URN }, { 45, 20, T_URN }, { -50, 20, T_URN }, { 60, 15, T_URN }, { 68, -64, T_URN },
    { 26, -30, T_CHEST }, { -33, 33, T_CHEST }, { 0, 50, T_CHEST },
    { -10, -10, T_BERRY }, { 40, 30, T_BERRY }, { -40, 45, T_BERRY }, { 20, -40, T_BERRY },
};
static const level_t k_level_mesa = {
    "SUNSET MESAS", &k_look_mesa, k_feat_mesa, NC(k_feat_mesa), k_plat_mesa, NC(k_plat_mesa),
    k_gems_mesa, NC(k_gems_mesa), k_things_mesa, NC(k_things_mesa),
    { { 25, -38 }, { -36, 28 }, { 42, 25 }, { 72, -63 } },
    { -5, 8, -1.5708f }, { -5, 16, -1.5708f }, 46, 23,
};

/* ── World 3: Frostfang Peaks ── */
static const look_t k_look_frost = {
    .sky_top = 0x050A20, .sky_mid = 0x102050, .sky_hz = 0x30508A, .fog = 0x2A4070, .cloud = 0x4A5A8A, .cloud2 = 0x3A4A78,
    .mount = 0x1A2A50, .mount2 = 0x2A3E6A, .sun = 0xF0F4FF,
    .sea = 0x10305A, .sea_hi = 0xB8D8FF, .shallow = 0x8AB4D8, .sea_kind = SEA_ICE,
    .grass = 0xE8F0FA, .grass2 = 0xD4E2F2, .cliff = 0x5A6A88, .cliff2 = 0x4A587A, .sand = 0xA0B4CC, .path = 0x8A8AA0,
    .path2 = 0x7A7A92, .high = 0xF8FCFF, .high_y = 12.f,
    .light_rgb = 0xB8C8F0, .light_dir = { 0.3f, 0.75f, -0.5f }, .ambient = 0.42f, .fog_density = 0.0055f, .night = true,
    .leaf = 0x1E5A48, .leaf2 = 0xE8F0FA, .trunk = 0x4A3424, .tree_kind = 2, .sun_az = 300, .sun_up = 95, .sun_r = 9,
};
static const feat_t k_feat_frost[] = {
    { F_ISLE, { 0, 0, 70, 66, 2.f, 0.25f } },
    { F_ISLE, { 8, -6, 32, 30, 6.f, 0.07f } },
    { F_ISLE, { 12, -8, 23, 21, 10.5f, 0.08f } },
    { F_ISLE, { 14, -10, 10, 10, 15.f, 0.12f } },
    { F_RAMP, { -34, 8, -18, -4, 2.6f, 2.0f, 6.1f } },
    { F_RAMP, { 0, 16, 14, 7, 2.4f, 6.0f, 10.6f } },
    { F_RAMP, { 30, 0, 20, -10, 2.2f, 10.5f, 15.1f } },
    { F_ISLE, { -50, -68, 12, 12, 3.f, 0.3f } },
    { F_ISLE, { 72, 20, 12, 12, 3.f, 0.3f } },
    { F_PIT, { -30, 40, 8, -3.f, 0.5f } },
};
static const plat_def_t k_plat_frost[] = {
    { -37, -50, 1.2f, 2.4f, 0, 0 }, { -43, -56, 1.9f, 2.3f, 0.5f, 3.2f },
    { -30, 40, 1.0f, 2.2f, 0, 0 },
    { 40, 48, 3.5f, 2.4f, 0, 0 }, { 48, 54, 5.0f, 2.2f, 0.8f, 4.f }, { 56, 58, 6.5f, 2.2f, 0, 0 },
};
static const gemline_t k_gems_frost[] = {
    { -40, 14, -20, 14, 5, GEM_AMBER, 0, 0 },
    { -33, 7, -19, -3, 5, GEM_AMBER, 0, 0 },
    { 0, 16, 13, 8, 4, GEM_AMBER, 0, 0 },
    { 29, 0, 21, -9, 4, GEM_EMERALD, 0, 0 },
    { 14, -10, 14, -10, 1, GEM_STAR, 0, 0 },
    { -37, -50, -43, -56, 2, GEM_SAPPHIRE, 0, 0 },
    { -50, -68, -50, -68, 1, GEM_STAR, 0, 0 },
    { 38, -2, 60, 14, 5, GEM_AMBER, 9.5f, 5.f },
    { 72, 20, 72, 20, 1, GEM_SAPPHIRE, 0, 0 },
    { 40, 48, 56, 58, 3, GEM_EMERALD, 0, 0 },
    { -30, 40, -30, 40, 1, GEM_SAPPHIRE, 0, 0 },
    { 0, 40, 30, 35, 5, GEM_AMBER, 0, 0 },
    { -50, -20, -50, 10, 5, GEM_AMBER, 0, 0 },
    { -10, -35, 20, -40, 5, GEM_AMBER, 0, 0 },
    { 40, -30, 45, -10, 3, GEM_EMERALD, 0, 0 },
};
static const thing_t k_things_frost[] = {
    { -30, 20, T_GRUB }, { -10, 30, T_GRUB }, { 20, 35, T_GRUB }, { -45, -10, T_GRUB }, { 40, -20, T_GRUB },
    { -20, -30, T_BEETLE }, { 45, 30, T_BEETLE }, { -10, 5, T_BEETLE }, { 70, 18, T_BEETLE },
    { 5, 8, T_BRUTE }, { 22, -2, T_BRUTE }, { -48, -66, T_BRUTE }, { 0, -25, T_BRUTE },
    { -40, 30, T_URN }, { 25, 40, T_URN }, { -5, -14, T_URN }, { 74, 24, T_URN }, { 56, 58, T_URN },
    { 12, -12, T_CHEST }, { -53, -70, T_CHEST }, { 30, -35, T_CHEST },
    { -25, 10, T_BERRY }, { 10, 20, T_BERRY }, { 50, -25, T_BERRY }, { 16, -6, T_BERRY },
};
static const level_t k_level_frost = {
    "FROSTFANG PEAKS", &k_look_frost, k_feat_frost, NC(k_feat_frost), k_plat_frost, NC(k_plat_frost),
    k_gems_frost, NC(k_gems_frost), k_things_frost, NC(k_things_frost),
    { { 16, -13 }, { -52, -70 }, { 73, 21 }, { -5, 18 } },
    { -40, 20, 0 }, { -48, 20, 0 }, 90, 37,
};

static const level_t *const k_levels[] = { &k_level_meadow, &k_level_mesa, &k_level_frost };
#define NLEVELS NC(k_levels)
static int g_level;
static const level_t *g_lv;
static const look_t *g_look;

/* ── The height field ────────────────────────────────────────────────────── */
#define CELL  2.f
#define GN    128                    /* cells across */
#define GV    (GN + 1)
#define HALF  (GN * CELL * 0.5f)     /* 128 m */
#define CHC   16                     /* cells per chunk side (32 m) */
#define NCH   (GN / CHC)             /* 8 x 8 chunks */
static float   *g_hf;                /* GV x GV heights */
static uint8_t *g_hm;                /* GV x GV: 1 = a ramp (path) */

static float feat_height(float x, float z, uint8_t *mat)
{
    const level_t *L = g_lv;
    float h = -7.f;
    uint8_t m = 0;
    for (int i = 0; i < L->nfeat; i++) {
        const feat_t *f = &L->feat[i];
        if (f->k == F_ISLE) {
            float dx = (x - f->a[0]) / f->a[2], dz = (z - f->a[1]) / f->a[3];
            float d = sqrtf(dx * dx + dz * dz);
            if (d >= 1.f)
                continue;
            float t = smooth01((1.f - d) / f->a[5]);
            float hi = lerpf(-7.f, f->a[4], t);
            if (hi > h) {
                h = hi;
                m = 0;
            }
        } else if (f->k == F_RAMP) {
            float ax = f->a[2] - f->a[0], az = f->a[3] - f->a[1], len2 = ax * ax + az * az;
            float t = ((x - f->a[0]) * ax + (z - f->a[1]) * az) / len2;
            float len = sqrtf(len2);
            float out = t < 0 ? -t * len : t > 1 ? (t - 1) * len : 0;
            float tc = clampf(t, 0, 1);
            float px = f->a[0] + ax * tc, pz = f->a[1] + az * tc;
            float lat = sqrtf((x - px) * (x - px) + (z - pz) * (z - pz) - out * out);
            if (!(lat == lat)) lat = 0;
            float hr = lerpf(f->a[5], f->a[6], tc) - fmaxf(0, lat - f->a[4]) * 3.5f - out * 3.5f;
            if (hr > h) {
                h = hr;
                m = lat < f->a[4] && out < 0.5f ? 1 : 0;
            }
        }
    }
    for (int i = 0; i < L->nfeat; i++) {
        const feat_t *f = &L->feat[i];
        if (f->k != F_PIT)
            continue;
        float dx = x - f->a[0], dz = z - f->a[1];
        float d = sqrtf(dx * dx + dz * dz) / f->a[2];
        if (d >= 1.f)
            continue;
        float t = smooth01((1.f - d) / f->a[4]);
        float hp = lerpf(h, f->a[3], t);
        if (hp < h) {
            h = hp;
            m = 0;
        }
    }
    /* gentle bumps on the land (not on the paths) */
    if (m == 0 && h > 0.4f) {
        float k = smooth01((h - 0.4f) / 1.2f);
        h += (vnoise(x * 0.13f, z * 0.13f) * 0.30f + vnoise(x * 0.37f, z * 0.37f) * 0.08f) * k;
    }
    *mat = m;
    return h;
}

static void build_heights(void)
{
    for (int j = 0; j < GV; j++)
        for (int i = 0; i < GV; i++) {
            float x = -HALF + (float)i * CELL, z = -HALF + (float)j * CELL;
            uint8_t m;
            float h = feat_height(x, z, &m);
            /* the edge of the map is always sea */
            float e = fmaxf(fabsf(x), fabsf(z));
            if (e > HALF - 8.f)
                h = fminf(h, -7.f + (HALF - e));
            g_hf[j * GV + i] = h;
            g_hm[j * GV + i] = m;
        }
}

static inline float hf(int i, int j)
{
    if (i < 0) i = 0;
    if (j < 0) j = 0;
    if (i > GN) i = GN;
    if (j > GN) j = GN;
    return g_hf[j * GV + i];
}

/* the height of the land at (x, z), on the same triangles the mesh draws */
static float ground_h(float x, float z)
{
    float fx = (x + HALF) / CELL, fz = (z + HALF) / CELL;
    if (fx < 0 || fz < 0 || fx >= GN || fz >= GN)
        return -7.f;
    int i = (int)fx, j = (int)fz;
    float u = fx - (float)i, v = fz - (float)j;
    float h00 = hf(i, j), h10 = hf(i + 1, j), h01 = hf(i, j + 1), h11 = hf(i + 1, j + 1);
    if (u + v < 1.f)
        return h00 + (h10 - h00) * u + (h01 - h00) * v;
    return h11 + (h01 - h11) * (1.f - u) + (h10 - h11) * (1.f - v);
}
/* the land's normal (y up) */
static void ground_n(float x, float z, float *n)
{
    float e = 0.6f;
    float hx = ground_h(x + e, z) - ground_h(x - e, z), hz = ground_h(x, z + e) - ground_h(x, z - e);
    v3_set(n, -hx, 2.f * e, -hz);
    v3_norm(n);
}

/* ── Floating stones ─────────────────────────────────────────────────────── */
#define MAX_PLAT 16
typedef struct { float x, z, y0, y, r, bob, period, ph, dy; e3d_mesh *mesh; } plat_t;
static plat_t g_plat[MAX_PLAT];
static int g_nplat;
#define PLAT_THICK 1.4f

static e3d_mesh *make_platform(float r)
{
    b_reset();
    const look_t *lk = g_look;
    float pr[][2] = { { 0.0f, -2.6f }, { r * 0.45f, -1.6f }, { r * 0.92f, -0.55f }, { r, -0.15f }, { r * 0.96f, 0.0f }, { 0.0f, 0.0f } };
    uint32_t col[5] = { lk->cliff2, lk->cliff, lk->cliff, lk->grass, lk->grass2 };
    if (lk->sea_kind == SEA_LAVA) {
        col[3] = lk->path;
        col[4] = lk->path2;
    }
    b_lathe((const float (*)[2])pr, 6, 10, col, 0, 0, 0);
    return b_bake(50.f);
}

/* ── The land's meshes, in chunks ───────────────────────────────────────── */
#define NLOD 3
typedef struct {
    e3d_mesh *land[NLOD];
    e3d_mesh *scen[2];
    float c[3], r;
} chunk_t;
static chunk_t g_chunk[NCH * NCH];

/* colour of the land's triangle with corners at heights h[3] and the normal's y ny */
static uint32_t land_rgb(int i, int j, const float *h, float ny, int path)
{
    const look_t *lk = g_look;
    float hmin = fminf(h[0], fminf(h[1], h[2])), hmax = fmaxf(h[0], fmaxf(h[1], h[2])), ha = (h[0] + h[1] + h[2]) / 3.f;
    if (hmax <= SEA + 0.02f)
        return lk->shallow;
    uint32_t c;
    float n = hashf(i * 7 + 3, j * 13 + 5);
    if (ny < 0.72f) {
        int band = (int)floorf(ha / 1.3f);
        c = (band & 1) ? lk->cliff : lk->cliff2;
        c = shade_rgb(c, 0.92f + 0.14f * n);
    } else if (hmin < SEA + 0.5f) {
        c = shade_rgb(lk->sand, 0.95f + 0.08f * n);
    } else if (path) {
        c = ((i + j) & 1) ? lk->path : lk->path2;
    } else if (ha > lk->high_y) {
        c = shade_rgb(lk->high, 0.96f + 0.06f * n);
    } else {
        c = ((i + j) & 1) ? lk->grass : lk->grass2;
        c = shade_rgb(c, 0.95f + 0.08f * n);
    }
    return c;
}

/* the drawn height of a corner: under the sea, the land is pressed flat at the surface (the shallows) */
static float draw_h(int i, int j) { float h = hf(i, j); return h < SEA ? SEA - 0.02f : h; }

static e3d_mesh *build_land(int ci, int cj, int step)
{
    b_reset();
    int i0 = ci * CHC, j0 = cj * CHC, n = CHC / step;
    int base = B.nv;
    for (int b = 0; b <= n; b++)
        for (int a = 0; a <= n; a++) {
            int i = i0 + a * step, j = j0 + b * step;
            b_v(-HALF + (float)i * CELL, draw_h(i, j), -HALF + (float)j * CELL);
        }
    for (int b = 0; b < n; b++)
        for (int a = 0; a < n; a++) {
            int i = i0 + a * step, j = j0 + b * step;
            int v00 = base + b * (n + 1) + a, v10 = v00 + 1, v01 = v00 + n + 1, v11 = v01 + 1;
            float r00 = hf(i, j), r10 = hf(i + step, j), r01 = hf(i, j + step), r11 = hf(i + step, j + step);
            /* under deep water: nothing (the sea is the background) */
            if (r00 < -2.5f && r10 < -2.5f && r01 < -2.5f && r11 < -2.5f)
                continue;
            int path = g_hm[j * GV + i] + g_hm[j * GV + i + step] + g_hm[(j + step) * GV + i] >= 2;
            for (int t = 0; t < 2; t++) {
                int va = t ? v11 : v00, vb = v10, vc = v01;
                float *pa = B.pos[va], *pb = B.pos[vb], *pc = B.pos[vc];
                float e1[3] = { pb[0] - pa[0], pb[1] - pa[1], pb[2] - pa[2] }, e2[3] = { pc[0] - pa[0], pc[1] - pa[1], pc[2] - pa[2] };
                float nn[3];
                v3_cross(nn, e1, e2);
                if (nn[1] < 0)
                    v3_set(nn, -nn[0], -nn[1], -nn[2]);
                v3_norm(nn);
                float hh[3] = { pa[1], pb[1], pc[1] };
                uint32_t col = land_rgb(i / step * step + t, j, hh, nn[1], path);
                b_tri_n(va, vb, vc, col, nn);
            }
        }
    /* skirts on the coarser meshes: a strip down from each edge hides the cracks to a finer neighbour */
    if (step > 1) {
        for (int e = 0; e < 4; e++)
            for (int k = 0; k < n; k++) {
                int ia, ja, ib, jb;
                if (e == 0) { ia = i0 + k * step; ja = j0; ib = ia + step; jb = j0; }
                else if (e == 1) { ia = i0 + k * step; ja = j0 + CHC; ib = ia + step; jb = ja; }
                else if (e == 2) { ia = i0; ja = j0 + k * step; ib = i0; jb = ja + step; }
                else { ia = i0 + CHC; ja = j0 + k * step; ib = ia; jb = ja + step; }
                float ha = draw_h(ia, ja), hb = draw_h(ib, jb);
                if (ha <= SEA && hb <= SEA)
                    continue;
                float xa = -HALF + (float)ia * CELL, za = -HALF + (float)ja * CELL, xb = -HALF + (float)ib * CELL, zb = -HALF + (float)jb * CELL;
                int a0 = b_v(xa, ha, za), b0 = b_v(xb, hb, zb), a1 = b_v(xa, ha - 1.5f, za), b1 = b_v(xb, hb - 1.5f, zb);
                float nn[3] = { e == 2 ? -1.f : e == 3 ? 1.f : 0, 0, e == 0 ? -1.f : e == 1 ? 1.f : 0 };
                b_quad_n(a0, b0, b1, a1, g_look->cliff2, nn);
            }
    }
    return b_bake(48.f);
}

/* ── Trees and rocks ─────────────────────────────────────────────────────── */
#define MAX_TREES 120
typedef struct { float x, z, y, s; uint8_t kind; } tree_t;
static tree_t g_tree[MAX_TREES];
static int g_ntree;

/* a tree at (x, y, z), scale s; far = the simpler shape */
static void b_tree(const tree_t *t, bool far)
{
    const look_t *lk = g_look;
    float x = t->x, y = t->y, z = t->z, s = t->s;
    int sides = far ? 4 : 6;
    if (t->kind == 0) {
        /* a round tree: a trunk and two blobs of leaves */
        static const float pr[][2] = { { 0.28f, -0.3f }, { 0.22f, 1.6f }, { 0.0f, 1.9f } };
        uint32_t col[2] = { lk->trunk, lk->trunk };
        float p2[3][2];
        for (int i = 0; i < 3; i++) {
            p2[i][0] = pr[i][0] * s;
            p2[i][1] = pr[i][1] * s;
        }
        b_lathe((const float (*)[2])p2, 3, sides, col, x, y, z);
        b_ellipsoid(x, y + 2.6f * s, z, 1.5f * s, 1.25f * s, 1.5f * s, far ? 5 : 8, far ? 3 : 5, lk->leaf, far ? 0 : lk->leaf2);
        if (!far)
            b_ellipsoid(x + 0.4f * s, y + 3.5f * s, z - 0.2f * s, 0.95f * s, 0.85f * s, 0.95f * s, 7, 4, lk->leaf2, 0);
    } else if (t->kind == 1) {
        /* a desert column cactus with two arms */
        uint32_t c1 = lk->leaf, c2 = lk->leaf2;
        static const float pr[][2] = { { 0.32f, -0.2f }, { 0.34f, 2.6f }, { 0.22f, 3.0f }, { 0.0f, 3.1f } };
        float p2[4][2];
        for (int i = 0; i < 4; i++) {
            p2[i][0] = pr[i][0] * s;
            p2[i][1] = pr[i][1] * s;
        }
        uint32_t col[3] = { c1, c2, c1 };
        b_lathe((const float (*)[2])p2, 4, sides, col, x, y, z);
        if (!far)
            for (int k = -1; k <= 1; k += 2) {
                float fk = (float)k;
                b_box(x - 0.15f * s, y + 1.2f * s, z + fk * 0.25f * s, x + 0.15f * s, y + 1.45f * s, z + fk * 0.75f * s, c1);
                b_box(x - 0.14f * s, y + 1.2f * s, z + fk * 0.55f * s, x + 0.14f * s, y + (fk > 0 ? 2.2f : 1.9f) * s, z + fk * 0.8f * s, c2);
            }
    } else {
        /* a pine: a trunk and three tiers, snow on the tips */
        static const float pr[][2] = { { 0.22f, -0.3f }, { 0.18f, 1.0f }, { 0.0f, 1.1f } };
        float p2[3][2];
        for (int i = 0; i < 3; i++) {
            p2[i][0] = pr[i][0] * s;
            p2[i][1] = pr[i][1] * s;
        }
        uint32_t col[2] = { lk->trunk, lk->trunk };
        b_lathe((const float (*)[2])p2, 3, 4, col, x, y, z);
        for (int k = 0; k < (far ? 2 : 3); k++) {
            float yb = y + (0.8f + 1.1f * (float)k) * s, r = (1.6f - 0.42f * (float)k) * s;
            float b0[3] = { x, yb, z }, tp[3] = { x, yb + 1.9f * s, z };
            b_cone(b0, r, tp, sides + 1, lk->leaf, k == 2 || far ? lk->leaf2 : shade_rgb(lk->leaf, 1.2f));
        }
    }
}

/* a boulder */
static void b_rock(float x, float y, float z, float s, uint32_t seed)
{
    int base = B.nv;
    b_ellipsoid(x, y + 0.35f * s, z, 0.9f * s, 0.7f * s, 0.8f * s, 6, 4, g_look->cliff, g_look->cliff2);
    for (int i = base; i < B.nv; i++) {
        float k = 0.8f + 0.4f * hashf((int)seed + i, i * 3);
        B.pos[i][0] = x + (B.pos[i][0] - x) * k;
        B.pos[i][2] = z + (B.pos[i][2] - z) * k;
    }
}

static bool near_key_point(float x, float z, float d)
{
    const level_t *L = g_lv;
    float d2 = d * d;
#define NEAR2(px, pz) (((x) - (px)) * ((x) - (px)) + ((z) - (pz)) * ((z) - (pz)) < d2)
    for (int i = 0; i < 4; i++)
        if (NEAR2(L->beacon[i][0], L->beacon[i][1]))
            return true;
    if (NEAR2(L->spawn[0], L->spawn[1]) || NEAR2(L->portal[0], L->portal[1]))
        return true;
    for (int i = 0; i < L->nthings; i++)
        if (NEAR2(L->things[i].x, L->things[i].z))
            return true;
    for (int i = 0; i < L->ngems; i++) {
        const gemline_t *g = &L->gems[i];
        for (int k = 0; k < g->n; k++) {
            float t = g->n > 1 ? (float)k / (float)(g->n - 1) : 0;
            if (NEAR2(lerpf(g->x0, g->x1, t), lerpf(g->z0, g->z1, t)))
                return true;
        }
    }
    for (int i = 0; i < L->nplat; i++)
        if (NEAR2(L->plat[i].x, L->plat[i].z))
            return true;
#undef NEAR2
    return false;
}

static void place_trees(void)
{
    uint32_t r = g_lv->seed * 2654435761u + 17u;
#define RND() ((r = r * 1664525u + 1013904223u), (float)(r >> 8) * (1.f / 16777216.f))
    g_ntree = 0;
    for (int tries = 0; tries < 4000 && g_ntree < g_lv->ntrees && g_ntree < MAX_TREES; tries++) {
        float x = (RND() * 2.f - 1.f) * (HALF - 12.f), z = (RND() * 2.f - 1.f) * (HALF - 12.f);
        float h = ground_h(x, z), n[3];
        ground_n(x, z, n);
        int i = (int)((x + HALF) / CELL), j = (int)((z + HALF) / CELL);
        if (h < SEA + 1.0f || n[1] < 0.9f || g_hm[j * GV + i] || g_hm[j * GV + i + 1] || g_hm[(j + 1) * GV + i])
            continue;
        if (near_key_point(x, z, 4.5f))
            continue;
        bool clash = false;
        for (int k = 0; k < g_ntree && !clash; k++)
            clash = (g_tree[k].x - x) * (g_tree[k].x - x) + (g_tree[k].z - z) * (g_tree[k].z - z) < 25.f;
        if (clash)
            continue;
        tree_t *t = &g_tree[g_ntree++];
        t->x = x;
        t->z = z;
        t->y = h;
        t->s = 0.85f + 0.4f * RND();
        t->kind = (uint8_t)(RND() < 0.2f ? 9 : g_look->tree_kind);   /* 9: a boulder */
    }
#undef RND
}

static bool build_chunks(void)
{
    for (int cj = 0; cj < NCH; cj++)
        for (int ci = 0; ci < NCH; ci++) {
            chunk_t *k = &g_chunk[cj * NCH + ci];
            memset(k, 0, sizeof(*k));
            float hmin = 1e9f, hmax = -1e9f;
            for (int j = cj * CHC; j <= (cj + 1) * CHC; j++)
                for (int i = ci * CHC; i <= (ci + 1) * CHC; i++) {
                    float h = draw_h(i, j);
                    hmin = fminf(hmin, h);
                    hmax = fmaxf(hmax, h);
                }
            k->c[0] = -HALF + ((float)ci + 0.5f) * CHC * CELL;
            k->c[2] = -HALF + ((float)cj + 0.5f) * CHC * CELL;
            k->c[1] = (hmin + hmax) * 0.5f;
            float hr = CHC * CELL * 0.5f;
            k->r = sqrtf(2.f * hr * hr + (hmax - hmin) * (hmax - hmin) * 0.25f);
            bool any_land = hmax > SEA + 0.05f;
            for (int l = 0; l < NLOD; l++) {
                if (!any_land && hmax <= SEA - 0.01f) {
                    k->land[l] = NULL;
                    continue;
                }
                k->land[l] = build_land(ci, cj, 1 << l);
            }
            /* the trees and rocks standing in this chunk */
            for (int far = 0; far < 2; far++) {
                b_reset();
                for (int t = 0; t < g_ntree; t++) {
                    const tree_t *tr = &g_tree[t];
                    int ti = (int)((tr->x + HALF) / (CHC * CELL)), tj = (int)((tr->z + HALF) / (CHC * CELL));
                    if (ti != ci || tj != cj)
                        continue;
                    if (tr->kind == 9)
                        b_rock(tr->x, tr->y, tr->z, tr->s * 1.3f, (uint32_t)t * 97u);
                    else
                        b_tree(tr, far != 0);
                }
                k->scen[far] = B.nt ? b_bake(50.f) : NULL;
            }
        }
    return true;
}

static void free_world(void)
{
    for (int i = 0; i < NCH * NCH; i++) {
        for (int l = 0; l < NLOD; l++)
            e3d_mesh_free(g_chunk[i].land[l]);
        for (int l = 0; l < 2; l++)
            e3d_mesh_free(g_chunk[i].scen[l]);
        memset(&g_chunk[i], 0, sizeof(g_chunk[i]));
    }
    for (int i = 0; i < g_nplat; i++) {
        e3d_mesh_free(g_plat[i].mesh);
        g_plat[i].mesh = NULL;
    }
    g_nplat = 0;
}

/* ── Game state ──────────────────────────────────────────────────────────── */
enum { ST_TITLE, ST_PLAY, ST_DONE };
static int g_state;
static float g_state_t, g_time, g_clock;      /* time in the state, play time, total time */
static bool g_auto, g_show_fps;

#define MAX_GEMS 360
typedef struct { float p[3], v[3]; uint8_t type, state; float t; } gem_t;   /* state: 0 none, 1 resting, 2 popping, 3 flying in */
static gem_t g_gem[MAX_GEMS];
static int g_gem_total, g_gem_got;            /* values */
static uint8_t g_ai_skip[MAX_GEMS];           /* gems the computer player gave up on */

#define MAX_EN 24
typedef struct {
    float p[3], home[2], yaw, vy;
    uint8_t type, alive;
    int hp, state, flame_id;
    float t, st, hit_t, dead_t, anim;
    bool burnt;
} enemy_t;
static enemy_t g_en[MAX_EN];
static int g_nen;
static const float k_en_r[3] = { 0.7f, 0.85f, 1.0f }, k_en_h[3] = { 1.1f, 1.1f, 2.4f };

#define MAX_BOX 12
typedef struct { float p[3]; uint8_t kind, alive; float hit_t; int flame_id; } box_t;
static box_t g_box[MAX_BOX];
static int g_nbox;
#define MAX_BERRY 8
typedef struct { float p[3]; bool alive; } berry_t;
static berry_t g_berry[MAX_BERRY];
static int g_nberry;
typedef struct { float p[3]; bool lit; float t; } beacon_t;
static beacon_t g_beacon[4];
static int g_lit;
static float g_portal_p[3], g_portal_yaw, g_portal_t;

#define MAX_PART 160
enum { PK_FIRE, PK_SPARK, PK_DUST, PK_RING, PK_SMOKE };
typedef struct { float p[3], v[3]; float life, life0, size, grav; uint32_t rgb; uint8_t kind; } part_t;
static part_t g_part[MAX_PART];

/* the dragon */
enum { M_WALK, M_AIR, M_GLIDE, M_DEAD };
typedef struct {
    float p[3], v[3], yaw;
    int mode, plat, hp;
    bool ground, charging;
    float coyote, flame_t, flame_cd, inv_t, walk_ph, wing, ground_t, dead_t, head, squash, charge_spd, air_t;
    float safe[3];
    int flame_id;
} player_t;
static player_t P;
#define HP_MAX 4
#define WALK_SPD  8.5f
#define CHARGE_SPD 15.5f
#define JUMP_V    10.f
#define GRAV      26.f
#define GLIDE_SPD 10.5f
#define GLIDE_SINK 2.4f
#define STEP_UP   0.6f
#define PR        0.55f

/* the camera */
static float g_cam_p[3], g_cam_yaw, g_cam_pitch_a = 0.3f, g_cam_manual_t, g_shake;
static float g_cam_h, g_cam_pt;              /* the e3d camera's heading and pitch, degrees */
static float g_focal = 290.f;                /* in 400-pixel terms */
static bool g_cam_invert;

/* messages across the screen */
static char g_msg[64];
static float g_msg_t;
static void note(const char *m, float t)
{
    snprintf(g_msg, sizeof(g_msg), "%s", m);
    g_msg_t = t;
}

/* records */
typedef struct { int best_gems; float best_time; bool done; } record_t;
static record_t g_rec[NLEVELS];
static int g_unlocked = 1;

static void spawn_part(const float *p, const float *v, float life, float size, uint32_t rgb, uint8_t kind, float grav)
{
    int best = 0;
    for (int i = 0; i < MAX_PART; i++) {
        if (g_part[i].life <= 0) {
            best = i;
            break;
        }
        if (g_part[i].life < g_part[best].life)
            best = i;
    }
    part_t *q = &g_part[best];
    memcpy(q->p, p, sizeof(q->p));
    if (v)
        memcpy(q->v, v, sizeof(q->v));
    else
        v3_set(q->v, 0, 0, 0);
    q->life = q->life0 = life;
    q->size = size;
    q->rgb = rgb;
    q->kind = kind;
    q->grav = grav;
}
static void burst(const float *p, int n, uint32_t rgb, float speed, float size)
{
    for (int i = 0; i < n; i++) {
        float v[3] = { frand2() * speed, frand() * speed * 1.2f, frand2() * speed };
        spawn_part(p, v, 0.5f + 0.4f * frand(), size * (0.7f + 0.6f * frand()), rgb, PK_SPARK, 14.f);
    }
}

/* the highest surface under (x, z) not above feet + 0.5: the land or a floating stone */
static float support_h(float x, float z, float feet, int *plat)
{
    float h = ground_h(x, z);
    int pi = -1;
    for (int i = 0; i < g_nplat; i++) {
        const plat_t *q = &g_plat[i];
        float dx = x - q->x, dz = z - q->z;
        if (dx * dx + dz * dz < q->r * q->r && q->y <= feet + 0.5f && q->y > h) {
            h = q->y;
            pi = i;
        }
    }
    if (plat)
        *plat = pi;
    return h;
}

static void add_gem(float x, float y, float z, int type, bool pop)
{
    for (int i = 0; i < MAX_GEMS; i++) {
        gem_t *g = &g_gem[i];
        if (g->state)
            continue;
        v3_set(g->p, x, y, z);
        g->type = (uint8_t)type;
        g->t = frand() * 6.f;
        if (pop) {
            g->state = 2;
            float a = frand() * 6.283f, s = 2.f + 2.5f * frand();
            v3_set(g->v, cosf(a) * s, 7.f + 3.f * frand(), sinf(a) * s);
        } else {
            g->state = 1;
            v3_set(g->v, 0, 0, 0);
        }
        return;
    }
}
static void drop_gems(const float *p, int amber, int emerald, int sapphire, int star)
{
    int n[4] = { amber, emerald, sapphire, star };
    for (int t = 0; t < 4; t++)
        for (int k = 0; k < n[t]; k++)
            add_gem(p[0], p[1] + 0.8f, p[2], t, true);
}

static void respawn_at(const float *p, float yaw)
{
    memcpy(P.p, p, sizeof(P.p));
    P.p[1] = support_h(p[0], p[2], p[1] + 2.f, &P.plat) + 0.02f;
    v3_set(P.v, 0, 0, 0);
    P.yaw = yaw;
    P.mode = M_WALK;
    P.ground = true;
    P.charging = false;
    P.inv_t = 1.2f;
    P.dead_t = 0;
    g_cam_yaw = yaw;
    float d = 7.f;
    v3_set(g_cam_p, P.p[0] - cosf(yaw) * d, P.p[1] + 3.f, P.p[2] - sinf(yaw) * d);
}

/* ── Building a world ── */
static bool level_build(int li)
{
    g_level = li;
    g_lv = k_levels[li];
    g_look = g_lv->look;
    s_song = li;
    if (!g_hf)
        g_hf = papp_alloc(sizeof(float) * GV * GV, PAPP_MEM_LARGE, 4);
    if (!g_hm)
        g_hm = papp_alloc(GV * GV, PAPP_MEM_LARGE, 4);
    if (!g_hf || !g_hm || !b_init())
        return false;
    free_world();
    build_heights();
    /* floating stones */
    g_nplat = 0;
    for (int i = 0; i < g_lv->nplat && i < MAX_PLAT; i++) {
        const plat_def_t *d = &g_lv->plat[i];
        plat_t *q = &g_plat[g_nplat++];
        q->x = d->x;
        q->z = d->z;
        q->y0 = q->y = d->y;
        q->r = d->r;
        q->bob = d->bob;
        q->period = d->period > 0 ? d->period : 1.f;
        q->ph = (float)i * 1.7f;
        q->dy = 0;
        q->mesh = make_platform(d->r);
        if (!q->mesh)
            return false;
    }
    place_trees();
    bool ok = build_chunks();
    b_free();
    return ok;
}

static void level_reset(void)
{
    const level_t *L = g_lv;
    memset(g_gem, 0, sizeof(g_gem));
    memset(g_part, 0, sizeof(g_part));
    memset(g_ai_skip, 0, sizeof(g_ai_skip));
    g_gem_total = g_gem_got = 0;
    /* gems on the lines */
    for (int i = 0; i < L->ngems; i++) {
        const gemline_t *g = &L->gems[i];
        for (int k = 0; k < g->n; k++) {
            float t = g->n > 1 ? (float)k / (float)(g->n - 1) : 0;
            float x = lerpf(g->x0, g->x1, t), z = lerpf(g->z0, g->z1, t);
            float y = g->y0 > 0 ? lerpf(g->y0, g->y1, t) : support_h(x, z, 100.f, NULL) + 0.85f;
            add_gem(x, y, z, g->type, false);
            g_gem_total += k_gem_value[g->type];
        }
    }
    /* creatures, urns, chests, berries */
    g_nen = g_nbox = g_nberry = 0;
    for (int i = 0; i < L->nthings; i++) {
        const thing_t *t = &L->things[i];
        float y = support_h(t->x, t->z, 100.f, NULL);
        if (t->kind <= T_BRUTE && g_nen < MAX_EN) {
            enemy_t *e = &g_en[g_nen++];
            memset(e, 0, sizeof(*e));
            v3_set(e->p, t->x, y, t->z);
            e->home[0] = t->x;
            e->home[1] = t->z;
            e->type = t->kind;
            e->alive = 1;
            e->hp = t->kind == T_BRUTE ? 2 : 1;
            e->yaw = frand() * 6.283f;
            e->t = frand() * 3.f;
            e->flame_id = -1;
            static const int val[3][4] = { { 2, 0, 0, 0 }, { 1, 1, 0, 0 }, { 2, 0, 1, 0 } };
            for (int k = 0; k < 4; k++)
                g_gem_total += val[t->kind][k] * k_gem_value[k];
        } else if ((t->kind == T_URN || t->kind == T_CHEST) && g_nbox < MAX_BOX) {
            box_t *b = &g_box[g_nbox++];
            v3_set(b->p, t->x, y, t->z);
            b->kind = t->kind;
            b->alive = 1;
            b->hit_t = 0;
            b->flame_id = -1;
            g_gem_total += t->kind == T_URN ? 3 * 1 + 5 : 3 * 1 + 5 + 10;
        } else if (t->kind == T_BERRY && g_nberry < MAX_BERRY) {
            berry_t *b = &g_berry[g_nberry++];
            v3_set(b->p, t->x, y + 0.2f, t->z);
            b->alive = true;
        }
    }
    for (int i = 0; i < 4; i++) {
        beacon_t *b = &g_beacon[i];
        v3_set(b->p, L->beacon[i][0], support_h(L->beacon[i][0], L->beacon[i][1], 100.f, NULL), L->beacon[i][1]);
        b->lit = false;
        b->t = 0;
    }
    g_lit = 0;
    v3_set(g_portal_p, L->portal[0], ground_h(L->portal[0], L->portal[1]), L->portal[1]);
    g_portal_yaw = L->portal[2];
    g_portal_t = 0;
    memset(&P, 0, sizeof(P));
    P.hp = HP_MAX;
    P.flame_id = 0;
    float sp[3] = { L->spawn[0], 0, L->spawn[1] };
    sp[1] = ground_h(sp[0], sp[2]);
    memcpy(P.safe, sp, sizeof(sp));
    respawn_at(sp, L->spawn[2]);
    P.inv_t = 0;
    g_time = 0;
    g_msg_t = 0;
}

static void start_play(void)
{
    level_reset();
    g_state = ST_PLAY;
    g_state_t = 0;
    note(g_lv->name, 3.f);
}
static void start_attract(void)
{
    level_reset();
    g_state = ST_TITLE;
    g_state_t = 0;
}

/* ── Input ── */
typedef struct {
    float mx, my;            /* the stick: right, forward */
    bool jump, jump_held, fire, charge, center, pause, start, tap, any;
    float cam;               /* camera turn, -1..1 */
} input_t;

/* ── Hurting and dying ── */
static void hurt_player(const float *from, int dmg)
{
    if (P.inv_t > 0 || P.mode == M_DEAD)
        return;
    P.hp -= dmg;
    P.inv_t = 1.6f;
    snd_hurt();
    g_shake = 0.35f;
    float dx = P.p[0] - from[0], dz = P.p[2] - from[2], l = sqrtf(dx * dx + dz * dz) + 1e-3f;
    P.v[0] = dx / l * 7.f;
    P.v[2] = dz / l * 7.f;
    P.v[1] = 6.f;
    P.mode = M_AIR;
    P.ground = false;
    P.charging = false;
    if (P.hp <= 0) {
        P.mode = M_DEAD;
        P.dead_t = 1.6f;
        burst(P.p, 14, C_SCALE, 4.f, 0.35f);
    }
}

static void fall_in_sea(void)
{
    P.mode = M_DEAD;
    P.dead_t = 1.3f;
    P.inv_t = 0;
    P.hp--;
    snd_splash();
    uint32_t c = g_look->sea_kind == SEA_LAVA ? 0xFFA020 : 0xE0F0FF;
    float p[3] = { P.p[0], SEA, P.p[2] };
    for (int i = 0; i < 18; i++) {
        float v[3] = { frand2() * 3.f, 5.f + frand() * 5.f, frand2() * 3.f };
        spawn_part(p, v, 0.8f + 0.4f * frand(), 0.3f + 0.25f * frand(), c, PK_SPARK, 18.f);
    }
}

/* ── Creatures ── */
static void kill_enemy(enemy_t *e, bool by_fire)
{
    e->alive = 0;
    e->dead_t = 0.8f;
    e->burnt = by_fire;
    snd_pop();
    float c[3] = { e->p[0], e->p[1] + k_en_h[e->type] * 0.5f, e->p[2] };
    burst(c, 12, by_fire ? 0x302018 : 0xFFFFFF, 4.f, 0.3f);
    if (e->type == T_GRUB)
        drop_gems(e->p, 2, 0, 0, 0);
    else if (e->type == T_BEETLE)
        drop_gems(e->p, 1, 1, 0, 0);
    else
        drop_gems(e->p, 2, 0, 1, 0);
    /* sometimes a berry */
    if (frand() < 0.25f && g_nberry < MAX_BERRY) {
        berry_t *b = &g_berry[g_nberry++];
        v3_set(b->p, e->p[0], e->p[1] + 0.2f, e->p[2]);
        b->alive = true;
    }
}

static bool enemy_can_go(const enemy_t *e, float x, float z)
{
    float h = ground_h(x, z);
    if (h < SEA + 0.3f || fabsf(h - e->p[1]) > 0.8f)
        return false;
    float dx = x - e->home[0], dz = z - e->home[1];
    return dx * dx + dz * dz < 16.f * 16.f;
}

static void enemy_step(enemy_t *e, float dt)
{
    if (!e->alive) {
        if (e->dead_t > 0)
            e->dead_t -= dt;
        return;
    }
    e->t += dt;
    e->st += dt;
    if (e->hit_t > 0)
        e->hit_t -= dt;
    float dx = P.p[0] - e->p[0], dz = P.p[2] - e->p[2], d = sqrtf(dx * dx + dz * dz);
    float to_p = atan2f(dz, dx);
    bool sees = d < 11.f && fabsf(P.p[1] - e->p[1]) < 3.f && P.mode != M_DEAD;
    float spd = 0;
    if (e->type == T_GRUB) {
        if (sees && d > 0.8f) {
            e->yaw = turn_to(e->yaw, to_p, 3.f * dt);
            spd = 2.4f;
        } else {
            if (e->st > 2.5f) {
                e->st = 0;
                e->yaw += frand2() * 2.f;
            }
            spd = 1.1f;
        }
    } else if (e->type == T_BEETLE) {
        /* 0 wander, 1 wind up, 2 rush, 3 rest */
        if (e->state == 0) {
            spd = 1.3f;
            if (e->st > 3.f) {
                e->st = 0;
                e->yaw += frand2() * 2.f;
            }
            if (sees && d < 9.f) {
                e->state = 1;
                e->st = 0;
            }
        } else if (e->state == 1) {
            e->yaw = turn_to(e->yaw, to_p, 5.f * dt);
            if (e->st > 0.55f) {
                e->state = 2;
                e->st = 0;
                sfx(300, 600, 150, WAVE_SAW, 0.08f);
            }
        } else if (e->state == 2) {
            spd = 7.5f;
            if (e->st > 1.1f) {
                e->state = 3;
                e->st = 0;
            }
        } else if (e->st > 1.3f) {
            e->state = 0;
            e->st = 0;
        }
    } else {
        /* the brute: 0 guard, 1 walk to the player, 2 wind up, 3 swing, 4 recover */
        float hx = e->p[0] - e->home[0], hz = e->p[2] - e->home[1];
        bool far_home = hx * hx + hz * hz > 11.f * 11.f;
        if (e->state <= 1) {
            if (sees && d < 10.f && !far_home) {
                e->state = 1;
                e->yaw = turn_to(e->yaw, to_p, 2.5f * dt);
                spd = d > 2.6f ? 2.1f : 0;
                if (d < 2.9f) {
                    e->state = 2;
                    e->st = 0;
                }
            } else {
                e->state = 0;
                if (hx * hx + hz * hz > 2.f) {
                    e->yaw = turn_to(e->yaw, atan2f(-hz, -hx), 2.f * dt);
                    spd = 1.5f;
                } else
                    e->yaw = turn_to(e->yaw, to_p, 1.f * dt);
            }
        } else if (e->state == 2) {
            e->yaw = turn_to(e->yaw, to_p, 2.f * dt);
            if (e->st > 0.7f) {
                e->state = 3;
                e->st = 0;
                snd_swing();
            }
        } else if (e->state == 3) {
            if (e->st > 0.12f && e->st - dt <= 0.12f) {
                /* the blow lands in front */
                float fx = cosf(e->yaw), fz = sinf(e->yaw);
                if (d < 3.4f && (dx * fx + dz * fz) > 0.2f * d && fabsf(P.p[1] - e->p[1]) < 2.f) {
                    float c[3] = { e->p[0], e->p[1], e->p[2] };
                    hurt_player(c, 1);
                }
                g_shake = fmaxf(g_shake, d < 8.f ? 0.15f : 0);
                snd_thud();
            }
            if (e->st > 0.35f) {
                e->state = 4;
                e->st = 0;
            }
        } else if (e->st > 0.9f) {
            e->state = 1;
            e->st = 0;
        }
    }
    if (spd > 0) {
        float nx = e->p[0] + cosf(e->yaw) * spd * dt, nz = e->p[2] + sinf(e->yaw) * spd * dt;
        if (enemy_can_go(e, nx, nz)) {
            e->p[0] = nx;
            e->p[2] = nz;
        } else {
            e->yaw += 2.5f + frand();
            if (e->type == T_BEETLE && e->state == 2) {
                e->state = 3;
                e->st = 0;
            }
        }
        e->anim += spd * dt;
    }
    e->p[1] = ground_h(e->p[0], e->p[2]);
    /* touching the dragon */
    float r = k_en_r[e->type] + PR;
    if (d < r && P.p[1] < e->p[1] + k_en_h[e->type] && P.p[1] > e->p[1] - 1.f && P.mode != M_DEAD) {
        if (P.charging && e->type != T_BRUTE) {
            kill_enemy(e, false);
            g_shake = 0.12f;
        } else if (P.charging) {
            /* bounced off the brute */
            P.charging = false;
            P.v[0] = -cosf(P.yaw) * 9.f;
            P.v[2] = -sinf(P.yaw) * 9.f;
            P.v[1] = 5.f;
            P.mode = M_AIR;
            P.ground = false;
            snd_thud();
            g_shake = 0.25f;
        } else if (e->type != T_BRUTE || e->state != 3) {
            float c[3] = { e->p[0], e->p[1], e->p[2] };
            hurt_player(c, 1);
        }
    }
}

/* the fire hits whatever is in front of the dragon's mouth */
static bool in_flame(const float *mouth, const float *target, float reach)
{
    float d[3] = { target[0] - mouth[0], target[1] - mouth[1], target[2] - mouth[2] };
    float l = sqrtf(d[0] * d[0] + d[2] * d[2]);
    if (l > reach || fabsf(d[1]) > 2.6f)
        return false;
    if (l < 1.2f)
        return true;
    return (d[0] * cosf(P.yaw) + d[2] * sinf(P.yaw)) / l > 0.74f;
}

static void break_box(box_t *b)
{
    b->alive = 0;
    snd_break();
    float c[3] = { b->p[0], b->p[1] + 0.5f, b->p[2] };
    burst(c, 14, b->kind == T_URN ? 0xB85A30 : 0x6A6E78, 5.f, 0.35f);
    if (b->kind == T_URN)
        drop_gems(b->p, 3, 1, 0, 0);
    else
        drop_gems(b->p, 3, 1, 1, 0);
}

static void light_beacon(int i)
{
    beacon_t *b = &g_beacon[i];
    b->lit = true;
    b->t = 0;
    g_lit++;
    snd_beacon();
    float c[3] = { b->p[0], b->p[1] + 2.8f, b->p[2] };
    burst(c, 20, 0xFFC040, 6.f, 0.35f);
    char m[48];
    if (g_lit == 4) {
        note("THE PORTAL IS OPEN", 4.f);
        snd_portal();
    } else {
        snprintf(m, sizeof(m), "BEACON LIT  %d / 4", g_lit);
        note(m, 2.5f);
    }
}

static void fire_hits(void)
{
    float mouth[3] = { P.p[0] + cosf(P.yaw) * 0.8f, P.p[1] + 0.9f, P.p[2] + sinf(P.yaw) * 0.8f };
    for (int i = 0; i < g_nen; i++) {
        enemy_t *e = &g_en[i];
        if (!e->alive || e->flame_id == P.flame_id)
            continue;
        float c[3] = { e->p[0], e->p[1] + k_en_h[e->type] * 0.4f, e->p[2] };
        if (!in_flame(mouth, c, 5.2f + k_en_r[e->type]))
            continue;
        e->flame_id = P.flame_id;
        if (e->type == T_BEETLE) {
            snd_clink();
            e->hit_t = 0.3f;
            burst(c, 5, 0xFFFFC0, 3.f, 0.15f);
        } else if (--e->hp <= 0)
            kill_enemy(e, true);
        else {
            e->hit_t = 0.4f;
            snd_thud();
        }
    }
    for (int i = 0; i < g_nbox; i++) {
        box_t *b = &g_box[i];
        if (!b->alive || b->flame_id == P.flame_id)
            continue;
        float c[3] = { b->p[0], b->p[1] + 0.5f, b->p[2] };
        if (!in_flame(mouth, c, 5.5f))
            continue;
        b->flame_id = P.flame_id;
        if (b->kind == T_URN)
            break_box(b);
        else {
            snd_clink();
            b->hit_t = 0.3f;
        }
    }
    for (int i = 0; i < 4; i++) {
        beacon_t *b = &g_beacon[i];
        if (b->lit)
            continue;
        float c[3] = { b->p[0], b->p[1] + 2.2f, b->p[2] };
        if (in_flame(mouth, c, 5.5f))
            light_beacon(i);
    }
}

/* ── The dragon ── */
static bool blocked(float x, float z, float feet)
{
    if (fabsf(x) > HALF - 3.f || fabsf(z) > HALF - 3.f)
        return true;
    return support_h(x, z, feet, NULL) > feet + STEP_UP;
}

/* pushed out of trunks, pillars, urns and chests */
static bool push_out(float cx, float cz, float r, float base, float top)
{
    if (P.p[1] > top || P.p[1] + 1.2f < base)
        return false;
    float dx = P.p[0] - cx, dz = P.p[2] - cz, d2 = dx * dx + dz * dz, rr = r + PR;
    if (d2 >= rr * rr || d2 < 1e-6f)
        return false;
    float d = sqrtf(d2);
    P.p[0] = cx + dx / d * rr;
    P.p[2] = cz + dz / d * rr;
    return true;
}

static void player_step(const input_t *in, float dt)
{
    if (P.inv_t > 0)
        P.inv_t -= dt;
    if (P.flame_cd > 0)
        P.flame_cd -= dt;
    if (P.mode == M_DEAD) {
        P.dead_t -= dt;
        if (P.dead_t <= 0) {
            if (P.hp <= 0) {
                P.hp = HP_MAX;
                note("TRY AGAIN", 1.5f);
            }
            respawn_at(P.safe, P.yaw);
        }
        return;
    }
    /* the stick, turned into the world by the camera */
    float cy = cosf(g_cam_yaw), sy = sinf(g_cam_yaw);
    float wx = cy * in->my - sy * in->mx, wz = sy * in->my + cy * in->mx;
    float mag = sqrtf(wx * wx + wz * wz);
    if (mag > 1.f) {
        wx /= mag;
        wz /= mag;
        mag = 1.f;
    }
    float want = atan2f(wz, wx);
    /* fire */
    if (in->fire && P.flame_cd <= 0) {
        P.flame_t = 0.42f;
        P.flame_cd = 0.55f;
        P.flame_id++;
    }
    if (P.flame_t > 0) {
        P.flame_t -= dt;
        float fx = cosf(P.yaw), fz = sinf(P.yaw);
        for (int k = 0; k < 2; k++) {
            float m[3] = { P.p[0] + fx * 0.95f, P.p[1] + 0.85f + (P.mode == M_GLIDE ? -0.1f : 0), P.p[2] + fz * 0.85f };
            float sp = 12.f + 3.f * frand();
            float v[3] = { fx * sp + frand2() * 2.2f + P.v[0] * 0.8f, frand2() * 1.2f + 0.5f, fz * sp + frand2() * 2.2f + P.v[2] * 0.8f };
            spawn_part(m, v, 0.33f + 0.08f * frand(), 0.35f, 0xFFE070, PK_FIRE, -2.f);
        }
        fire_hits();
    }
    /* charge */
    bool want_charge = in->charge && P.mode == M_WALK;
    if (want_charge && !P.charging) {
        P.charging = true;
        P.charge_spd = fmaxf(WALK_SPD, sqrtf(P.v[0] * P.v[0] + P.v[2] * P.v[2]));
        snd_dash();
    }
    if (!in->charge && P.mode == M_WALK)
        P.charging = false;

    if (P.mode == M_WALK) {
        if (P.charging) {
            if (mag > 0.25f)
                P.yaw = turn_to(P.yaw, want, 3.0f * dt);
            P.charge_spd = fminf(CHARGE_SPD, P.charge_spd + 30.f * dt);
            P.v[0] = cosf(P.yaw) * P.charge_spd;
            P.v[2] = sinf(P.yaw) * P.charge_spd;
            if (frand() < 0.5f) {
                float p[3] = { P.p[0] - cosf(P.yaw) * 0.6f, P.p[1] + 0.1f, P.p[2] - sinf(P.yaw) * 0.6f };
                float v[3] = { frand2(), 1.5f, frand2() };
                spawn_part(p, v, 0.45f, 0.35f, g_look->sea_kind == SEA_ICE ? 0xE8F0FF : 0xC8B090, PK_DUST, -1.f);
            }
        } else {
            float k = fminf(1.f, 11.f * dt);
            P.v[0] += (wx * WALK_SPD - P.v[0]) * k;
            P.v[2] += (wz * WALK_SPD - P.v[2]) * k;
            if (mag > 0.1f)
                P.yaw = turn_to(P.yaw, want, 13.f * dt);
        }
        if ((in->jump) && (P.ground || P.coyote > 0)) {
            P.v[1] = JUMP_V;
            P.mode = M_AIR;
            P.ground = false;
            P.coyote = 0;
            P.air_t = 0;
            snd_jump();
        }
    } else if (P.mode == M_AIR) {
        P.air_t += dt;
        float spd = sqrtf(P.v[0] * P.v[0] + P.v[2] * P.v[2]);
        float top = fmaxf(WALK_SPD, spd * 0.995f);
        float k = fminf(1.f, 3.5f * dt);
        if (mag > 0.1f) {
            P.v[0] += (wx * top - P.v[0]) * k;
            P.v[2] += (wz * top - P.v[2]) * k;
            P.yaw = turn_to(P.yaw, want, 7.f * dt);
        }
        if (in->jump && P.air_t > 0.08f) {
            P.mode = M_GLIDE;
            snd_glide();
            P.charging = false;
        }
    } else if (P.mode == M_GLIDE) {
        if (mag > 0.2f)
            P.yaw = turn_to(P.yaw, want, 2.4f * dt);
        float spd = GLIDE_SPD * (mag > 0.2f && (wx * cosf(P.yaw) + wz * sinf(P.yaw)) < -0.5f ? 0.65f : 1.f);
        float k = fminf(1.f, 4.f * dt);
        P.v[0] += (cosf(P.yaw) * spd - P.v[0]) * k;
        P.v[2] += (sinf(P.yaw) * spd - P.v[2]) * k;
        if (in->jump) {
            P.mode = M_AIR;
            P.air_t = 1.f;
        }
    }
    /* gravity */
    if (!P.ground) {
        P.v[1] -= GRAV * dt;
        if (P.mode == M_GLIDE && P.v[1] < -GLIDE_SINK)
            P.v[1] = -GLIDE_SINK;
        if (P.v[1] < -30.f)
            P.v[1] = -30.f;
    }
    /* move across, blocked by cliffs */
    float nx = P.p[0] + P.v[0] * dt, nz = P.p[2] + P.v[2] * dt;
    if (!blocked(nx, nz, P.p[1])) {
        P.p[0] = nx;
        P.p[2] = nz;
    } else if (!blocked(nx, P.p[2], P.p[1])) {
        P.p[0] = nx;
        P.v[2] *= 0.5f;
    } else if (!blocked(P.p[0], nz, P.p[1])) {
        P.p[2] = nz;
        P.v[0] *= 0.5f;
    } else {
        if (P.charging) {
            snd_thud();
            g_shake = 0.2f;
            P.charging = false;
            P.v[0] = -cosf(P.yaw) * 4.f;
            P.v[2] = -sinf(P.yaw) * 4.f;
        } else {
            P.v[0] = P.v[2] = 0;
        }
    }
    /* solid things */
    for (int i = 0; i < g_ntree; i++) {
        const tree_t *t = &g_tree[i];
        float r = t->kind == 9 ? 1.0f * t->s * 1.3f : 0.35f * t->s;
        if (push_out(t->x, t->z, r, t->y, t->y + (t->kind == 9 ? 1.2f * t->s : 6.f)) && P.charging && t->kind != 9) {
            P.charging = false;
            snd_thud();
        }
    }
    for (int i = 0; i < 4; i++)
        push_out(g_beacon[i].p[0], g_beacon[i].p[2], 0.75f, g_beacon[i].p[1], g_beacon[i].p[1] + 2.8f);
    for (int i = 0; i < g_nbox; i++) {
        box_t *b = &g_box[i];
        if (!b->alive)
            continue;
        if (push_out(b->p[0], b->p[2], b->kind == T_URN ? 0.45f : 0.65f, b->p[1], b->p[1] + 1.f) && P.charging)
            break_box(b);
    }
    for (int i = 0; i < g_nplat; i++) {
        const plat_t *q = &g_plat[i];
        if (P.p[1] < q->y - 0.45f && P.p[1] > q->y - PLAT_THICK - 1.2f)
            push_out(q->x, q->z, q->r * 0.9f, q->y - PLAT_THICK - 1.2f, q->y - 0.45f);
    }
    /* up and down */
    P.p[1] += P.v[1] * dt;
    int plat;
    float sup = support_h(P.p[0], P.p[2], P.p[1], &plat);
    bool was_ground = P.ground;
    if (P.p[1] <= sup) {
        if (!P.ground && P.v[1] < -9.f) {
            snd_land();
            P.squash = 0.25f;
        }
        P.p[1] = sup;
        if (P.v[1] < 0)
            P.v[1] = 0;
        P.ground = true;
        if (P.mode != M_WALK) {
            P.mode = M_WALK;
            if (in->charge)
                P.charge_spd = WALK_SPD;
        }
    } else if (P.ground && P.v[1] <= 0 && P.p[1] - sup < 0.5f) {
        P.p[1] = sup;
        P.v[1] = 0;
    } else {
        P.ground = false;
        if (P.mode == M_WALK) {
            P.mode = M_AIR;
            P.air_t = 0.1f;
            P.coyote = 0.12f;
        }
    }
    if (was_ground && !P.ground && P.coyote <= 0 && P.mode == M_AIR && P.v[1] <= 0)
        P.coyote = 0.12f;
    if (P.coyote > 0)
        P.coyote -= dt;
    P.plat = P.ground ? plat : -1;
    /* steep slopes: slide down */
    if (P.ground && plat < 0) {
        float n[3];
        ground_n(P.p[0], P.p[2], n);
        if (n[1] < 0.66f) {
            P.v[0] += n[0] * 18.f * dt;
            P.v[2] += n[2] * 18.f * dt;
            float up = P.v[0] * n[0] + P.v[2] * n[2];
            if (up < 0) {
                P.v[0] -= n[0] * up;
                P.v[2] -= n[2] * up;
            }
            P.ground_t = 0;
        }
        /* a safe place to come back to */
        P.ground_t += dt;
        if (n[1] > 0.85f && P.ground_t > 0.4f && P.p[1] > SEA + 0.3f) {
            bool ok = true;
            for (int k = 0; k < 6 && ok; k++) {
                float a = (float)k * 1.047f;
                ok = ground_h(P.p[0] + cosf(a) * 2.f, P.p[2] + sinf(a) * 2.f) > SEA + 0.2f;
            }
            if (ok)
                memcpy(P.safe, P.p, sizeof(P.safe));
        }
    } else
        P.ground_t = 0;
    /* the sea */
    if (P.p[1] < SEA - 0.6f && ground_h(P.p[0], P.p[2]) < SEA - 0.3f)
        fall_in_sea();
    /* animation */
    float hs = sqrtf(P.v[0] * P.v[0] + P.v[2] * P.v[2]);
    P.walk_ph += hs * dt * (P.charging ? 1.6f : 1.9f);
    float wing_to = P.mode == M_GLIDE ? 1.f : P.mode == M_AIR ? 0.45f : 0.f;
    P.wing += (wing_to - P.wing) * fminf(1.f, 8.f * dt);
    float head_to = P.charging ? -0.45f : P.flame_t > 0 ? 0.08f : 0.f;
    P.head += (head_to - P.head) * fminf(1.f, 10.f * dt);
    if (P.squash > 0)
        P.squash -= dt;
}

/* gems, berries, the portal */
static void pickups_step(float dt)
{
    float c[3] = { P.p[0], P.p[1] + 0.6f, P.p[2] };
    for (int i = 0; i < MAX_GEMS; i++) {
        gem_t *g = &g_gem[i];
        if (!g->state)
            continue;
        g->t += dt;
        if (g->state == 2) {
            g->v[1] -= 20.f * dt;
            for (int q = 0; q < 3; q++)
                g->p[q] += g->v[q] * dt;
            float s = support_h(g->p[0], g->p[2], g->p[1], NULL) + 0.6f;
            if (g->p[1] < s) {
                g->p[1] = s;
                g->v[1] = -g->v[1] * 0.45f;
                g->v[0] *= 0.6f;
                g->v[2] *= 0.6f;
                if (fabsf(g->v[1]) < 1.f) {
                    g->state = 1;
                    g->p[1] = s + 0.25f;
                }
            }
            if (g->p[1] < SEA - 0.5f) {
                /* fell in the sea: back up onto the land where the dragon is */
                v3_set(g->p, P.p[0] + frand2(), P.p[1] + 1.f, P.p[2] + frand2());
                v3_set(g->v, 0, 4.f, 0);
            }
            if (g->t < 0.4f)
                continue;
        }
        float dx = c[0] - g->p[0], dy = c[1] - g->p[1], dz = c[2] - g->p[2], d2 = dx * dx + dy * dy + dz * dz;
        if (g->state != 3 && d2 < 3.2f * 3.2f && P.mode != M_DEAD && fabsf(dy) < 2.2f) {
            g->state = 3;
            g->t = 0;
        }
        if (g->state == 3) {
            float k = fminf(1.f, (6.f + g->t * 30.f) * dt);
            for (int q = 0; q < 3; q++)
                g->p[q] += (c[q] - g->p[q]) * k;
            if (d2 < 0.5f || g->t > 0.5f) {
                g->state = 0;
                g_gem_got += k_gem_value[g->type];
                snd_gem(k_gem_value[g->type]);
                float v[3] = { 0, 1.5f, 0 };
                spawn_part(g->p, v, 0.35f, 0.6f, k_gem_rgb[g->type][0], PK_RING, 0);
            }
        }
    }
    for (int i = 0; i < g_nberry; i++) {
        berry_t *b = &g_berry[i];
        if (!b->alive || P.hp >= HP_MAX)
            continue;
        float dx = c[0] - b->p[0], dy = P.p[1] - b->p[1], dz = c[2] - b->p[2];
        if (dx * dx + dz * dz < 1.4f * 1.4f && fabsf(dy) < 1.6f) {
            b->alive = false;
            P.hp++;
            snd_heart();
            burst(b->p, 8, 0xFF4060, 3.f, 0.2f);
        }
    }
    if (g_lit == 4)
        g_portal_t += dt;
}

static void parts_step(float dt)
{
    for (int i = 0; i < MAX_PART; i++) {
        part_t *q = &g_part[i];
        if (q->life <= 0)
            continue;
        q->life -= dt;
        q->v[1] -= q->grav * dt;
        for (int k = 0; k < 3; k++)
            q->p[k] += q->v[k] * dt;
        if (q->kind == PK_FIRE || q->kind == PK_DUST) {
            q->v[0] *= 1.f - 2.f * dt;
            q->v[2] *= 1.f - 2.f * dt;
        }
    }
}

static void plats_step(void)
{
    for (int i = 0; i < g_nplat; i++) {
        plat_t *q = &g_plat[i];
        float y = q->y0 + q->bob * sinf(g_clock * 6.2832f / q->period + q->ph);
        q->dy = y - q->y;
        q->y = y;
        /* carry the dragon */
        if (P.plat == i && P.ground)
            P.p[1] += q->dy;
    }
}

/* ── The camera ── */
static void update_camera(const input_t *in, float dt)
{
    float tgt[3] = { P.p[0], P.p[1] + 1.0f, P.p[2] };
    if (g_state == ST_TITLE) {
        g_cam_yaw += dt * 0.25f;
        tgt[1] += 1.0f;
    } else {
        if (fabsf(in->cam) > 0.05f) {
            g_cam_yaw = wrap_pi(g_cam_yaw + in->cam * 2.4f * dt);
            g_cam_manual_t = 1.0f;
        } else if (g_cam_manual_t > 0)
            g_cam_manual_t -= dt;
        if (in->center)
            g_cam_manual_t = -0.6f;   /* swing behind quickly */
        float hs = sqrtf(P.v[0] * P.v[0] + P.v[2] * P.v[2]);
        float diff = wrap_pi(P.yaw - g_cam_yaw);
        if (g_cam_manual_t < 0)
            g_cam_yaw = turn_to(g_cam_yaw, P.yaw, 5.f * dt);
        else if (g_cam_manual_t <= 0 && hs > 1.5f && fabsf(diff) < 2.3f) {
            float rate = (P.mode == M_GLIDE ? 1.6f : 1.1f) * fminf(1.f, hs / 8.f) * (0.4f + fabsf(diff));
            g_cam_yaw = turn_to(g_cam_yaw, P.yaw, rate * dt);
        }
    }
    float pitch_to = P.mode == M_GLIDE ? 0.36f : g_state == ST_TITLE ? 0.16f : 0.26f;
    g_cam_pitch_a += (pitch_to - g_cam_pitch_a) * fminf(1.f, 2.f * dt);
    float dist = P.charging ? 6.4f : g_state == ST_TITLE ? 7.5f : 5.6f;
    float cp = cosf(g_cam_pitch_a), sp = sinf(g_cam_pitch_a);
    float want[3] = { tgt[0] - cosf(g_cam_yaw) * cp * dist, tgt[1] + sp * dist, tgt[2] - sinf(g_cam_yaw) * cp * dist };
    /* keep the land out of the way: pull in where it rises between the dragon and the camera */
    for (int k = 1; k <= 8; k++) {
        float t = (float)k / 8.f;
        float q[3] = { lerpf(tgt[0], want[0], t), lerpf(tgt[1], want[1], t), lerpf(tgt[2], want[2], t) };
        float g = ground_h(q[0], q[2]) + 0.7f;
        if (g > q[1]) {
            if (t > 0.45f) {
                for (int i = 0; i < 3; i++)
                    want[i] = lerpf(tgt[i], want[i], t - 0.12f);
                want[1] = fmaxf(want[1], g);
            } else
                want[1] += g - q[1];
            break;
        }
    }
    want[1] = fmaxf(want[1], ground_h(want[0], want[2]) + 0.8f);
    want[1] = fmaxf(want[1], SEA + 0.6f);
    float k = fminf(1.f, (g_state == ST_TITLE ? 3.f : 7.f) * dt);
    if (P.mode == M_DEAD)
        k *= 0.2f;
    for (int i = 0; i < 3; i++)
        g_cam_p[i] += (want[i] - g_cam_p[i]) * k;
    float dx = tgt[0] - g_cam_p[0], dy = tgt[1] - g_cam_p[1], dz = tgt[2] - g_cam_p[2];
    g_cam_h = atan2f(dz, dx) * DEG;
    g_cam_pt = atan2f(dy, sqrtf(dx * dx + dz * dz)) * DEG;
    if (g_shake > 0) {
        g_shake -= dt;
        g_cam_h += frand2() * g_shake * 3.f;
        g_cam_pt += frand2() * g_shake * 3.f;
    }
}

/* the computer plays: wanders to the nearest gem, beacon or creature (for testing and the demo) */
static float g_ai_t, g_ai_best, g_ai_stuck;
static int g_ai_gem = -1;
static input_t ai_input(float dt)
{
    input_t in = { 0 };
    g_ai_t += dt;
    float best = 1e9f, tx = 0, tz = 0;
    int kind = 0, gem = -1;
    for (int i = 0; i < 4; i++)
        if (!g_beacon[i].lit) {
            float dx = g_beacon[i].p[0] - P.p[0], dz = g_beacon[i].p[2] - P.p[2], d = dx * dx + dz * dz;
            if (fabsf(g_beacon[i].p[1] - P.p[1]) < 2.f && d < best) { best = d; tx = g_beacon[i].p[0]; tz = g_beacon[i].p[2]; kind = 1; }
        }
    for (int i = 0; i < MAX_GEMS; i++)
        if (g_gem[i].state == 1 && !g_ai_skip[i]) {
            float dx = g_gem[i].p[0] - P.p[0], dz = g_gem[i].p[2] - P.p[2], d = dx * dx + dz * dz;
            if (fabsf(g_gem[i].p[1] - P.p[1]) < 1.8f && d < best) { best = d; tx = g_gem[i].p[0]; tz = g_gem[i].p[2]; kind = 0; gem = i; }
        }
    for (int i = 0; i < g_nen; i++)
        if (g_en[i].alive && g_en[i].type == T_GRUB) {
            float dx = g_en[i].p[0] - P.p[0], dz = g_en[i].p[2] - P.p[2], d = dx * dx + dz * dz;
            if (d < best && d < 100.f) { best = d; tx = g_en[i].p[0]; tz = g_en[i].p[2]; kind = 2; gem = -1; }
        }
    /* give up on a gem it cannot get to */
    if (gem != g_ai_gem) {
        g_ai_gem = gem;
        g_ai_best = best;
        g_ai_stuck = 0;
    } else if (gem >= 0) {
        if (best < g_ai_best - 1.f) {
            g_ai_best = best;
            g_ai_stuck = 0;
        } else if ((g_ai_stuck += dt) > 3.f)
            g_ai_skip[gem] = 1;
    }
    if (best < 1e8f) {
        float a = atan2f(tz - P.p[2], tx - P.p[0]) - g_cam_yaw;
        in.mx = sinf(a);
        in.my = cosf(a);
        if ((kind >= 1 && best < 16.f) || (kind == 2 && best < 25.f))
            in.fire = fmodf(g_ai_t, 0.7f) < dt;
    } else {
        in.my = 1.f;
        in.mx = sinf(g_ai_t * 0.3f);
    }
    /* jump at walls, keep out of the sea */
    float ax = P.p[0] + cosf(P.yaw) * 1.5f, az = P.p[2] + sinf(P.yaw) * 1.5f;
    if (P.ground && support_h(ax, az, P.p[1], NULL) > P.p[1] + 0.4f)
        in.jump = true;
    if (support_h(ax, az, P.p[1], NULL) < SEA + 0.2f && P.ground) {
        in.mx = -in.mx;
        in.my = -in.my;
        if (gem >= 0)
            g_ai_skip[gem] = 1;
    }
    return in;
}

static void update(const input_t *in, float dt)
{
    g_state_t += dt;
    g_clock += dt;
    if (g_msg_t > 0)
        g_msg_t -= dt;
    plats_step();
    input_t none = { 0 };
    if (g_state == ST_PLAY) {
        g_time += dt;
        player_step(in, dt);
        for (int i = 0; i < g_nen; i++)
            enemy_step(&g_en[i], dt);
        pickups_step(dt);
        /* into the open portal */
        if (g_lit == 4 && P.mode != M_DEAD) {
            float dx = P.p[0] - g_portal_p[0], dz = P.p[2] - g_portal_p[2];
            if (dx * dx + dz * dz < 2.2f * 2.2f && fabsf(P.p[1] - g_portal_p[1]) < 2.5f) {
                g_state = ST_DONE;
                g_state_t = 0;
                snd_portal();
                record_t *r = &g_rec[g_level];
                if (!g_auto) {
                    if (g_gem_got > r->best_gems)
                        r->best_gems = g_gem_got;
                    if (!r->done || g_time < r->best_time)
                        r->best_time = g_time;
                    r->done = true;
                    if (g_unlocked < g_level + 2 && g_level + 1 < NLEVELS)
                        g_unlocked = g_level + 2;
                }
            }
        }
    } else if (g_state == ST_TITLE) {
        player_step(&none, dt);
        for (int i = 0; i < g_nen; i++)
            enemy_step(&g_en[i], dt);
    }
    parts_step(dt);
    for (int i = 0; i < 4; i++)
        g_beacon[i].t += dt;
    for (int i = 0; i < g_nbox; i++)
        if (g_box[i].hit_t > 0)
            g_box[i].hit_t -= dt;
    update_camera(in, dt);
    /* the sounds that follow the dragon */
    s_wind_vol = g_state == ST_PLAY && P.mode == M_GLIDE ? 0.35f : 0.f;
    s_fire_vol = P.flame_t > 0 ? 0.55f : 0.f;
    s_charge_vol = P.charging ? 0.25f : 0.f;
}

/* ── The background: sky, sun or moon, stars, clouds, far mountains and the sea ──
 * Rows above the horizon look up the sky's colour and, per column, the mountains,
 * clouds and the sun; rows below it are the sea, projected to the plane y = SEA
 * (16.16 fixed point across the row), with waves, lava crust or ice cracks. */
#define SKYN 2048
static uint8_t  s_m1[SKYN], s_m2[SKYN];          /* mountain heights (rows at 240) */
static uint8_t  s_cl_lo[SKYN], s_cl_hi[SKYN];    /* cloud band per column (rows at 240) */
static uint8_t  s_sun_lo[SKYN], s_sun_hi[SKYN];
static uint8_t  s_star[SKYN];
/* the same in pixel rows for this frame size */
static uint16_t p_m1[SKYN], p_m2[SKYN], p_cl_lo[SKYN], p_cl_hi[SKYN], p_cl_mid[SKYN], p_sun_lo[SKYN], p_sun_hi[SKYN], p_star[SKYN];
static int      p_top;                           /* above this many rows up, only the sky */
static uint16_t s_sky_tab[HMAX * 2];
static uint16_t s_c_m1, s_c_m2, s_c_cl, s_c_cl2, s_c_sun, s_c_star;
static int      s_bg_h;                          /* the frame height the tables are for */

typedef struct { int32_t x, z, dx, dz; uint16_t c0, c1, c2; uint8_t pat, pad; } srow_t;
static srow_t   s_sr[HMAX];
static uint16_t s_col[WMAX];
static int      s_hz, s_cloud_shift;

static void make_background(void)
{
    const look_t *lk = g_look;
    float k = (float)H / 240.f;
    for (int up = 0; up < HMAX * 2; up++) {
        float u = (float)up / k;
        uint32_t c;
        if (u < 18)
            c = mix_rgb(lk->fog, lk->sky_hz, u / 18.f);
        else if (u < 100)
            c = mix_rgb(lk->sky_hz, lk->sky_mid, (u - 18) / 82.f);
        else
            c = mix_rgb(lk->sky_mid, lk->sky_top, (u - 100) / 160.f);
        s_sky_tab[up] = rgb565(c);
    }
    s_c_m1 = rgb565(lk->mount);
    s_c_m2 = rgb565(mix_rgb(lk->mount2, lk->fog, 0.35f));
    s_c_cl = rgb565(lk->cloud);
    s_c_cl2 = rgb565(lk->cloud2);
    s_c_sun = rgb565(lk->sun);
    s_c_star = rgb565(0xFFFFFF);
    uint32_t r = 0xC0FFEEu + (uint32_t)g_level * 977u;
#define CR() ((r = r * 1664525u + 1013904223u), (float)(r >> 8) * (1.f / 16777216.f))
    /* mountains: two ridges of value noise */
    for (int i = 0; i < SKYN; i++) {
        float a = (float)i / (float)SKYN * 64.f;
        float n1 = vnoise(a * 0.35f, 3.1f) * 0.6f + vnoise(a * 1.1f, 7.7f) * 0.3f + vnoise(a * 3.3f, 1.3f) * 0.1f;
        float n2 = vnoise(a * 0.22f + 40.f, 5.5f) * 0.7f + vnoise(a * 0.9f, 2.2f) * 0.3f;
        float base1 = lk->sea_kind == SEA_ICE ? 26.f : 14.f, base2 = lk->sea_kind == SEA_ICE ? 44.f : 26.f;
        s_m1[i] = (uint8_t)clampf(base1 + n1 * 18.f, 2, 120);
        s_m2[i] = (uint8_t)clampf(base2 + n2 * 22.f, 2, 140);
    }
    /* clouds: puffy runs */
    memset(s_cl_lo, 0, sizeof(s_cl_lo));
    memset(s_cl_hi, 0, sizeof(s_cl_hi));
    for (int i = 0; i < SKYN;) {
        int w = 30 + (int)(CR() * 90.f), gap = 20 + (int)(CR() * 140.f);
        float y0 = 50.f + CR() * 70.f, th = 6.f + CR() * 10.f;
        for (int k2 = 0; k2 < w && i < SKYN; k2++, i++) {
            float t = (float)k2 / (float)w;
            float bump = sinf(t * (float)M_PI) * th + sinf(t * 23.f + y0) * 2.f + sinf(t * 9.f) * 3.f;
            if (bump < 1.f)
                continue;
            s_cl_lo[i] = (uint8_t)(y0 - bump * 0.35f);
            s_cl_hi[i] = (uint8_t)(y0 + bump);
        }
        i += gap;
    }
    /* stars (at night) */
    memset(s_star, 0, sizeof(s_star));
    if (lk->night)
        for (int i = 0; i < SKYN; i++)
            if (CR() < 0.22f)
                s_star[i] = (uint8_t)(40.f + CR() * 200.f);
    /* the sun or the moon */
    memset(s_sun_lo, 255, sizeof(s_sun_lo));
    memset(s_sun_hi, 0, sizeof(s_sun_hi));
    int sc = SKYN * lk->sun_az / 360, srad = lk->sun_r;
    int srad_c = srad * SKYN / 1200;    /* columns: about 1/1200 of the circle per row at the default lens */
    for (int d = -srad_c; d <= srad_c; d++) {
        int i = (sc + d + SKYN) % SKYN;
        float t = (float)d / (float)srad_c;
        float hh = sqrtf(fmaxf(0, 1.f - t * t)) * (float)srad;
        s_sun_lo[i] = (uint8_t)clampf((float)lk->sun_up - hh, 0, 255);
        s_sun_hi[i] = (uint8_t)clampf((float)lk->sun_up + hh, 0, 255);
    }
#undef CR
    /* to pixel rows */
    p_top = 0;
    for (int i = 0; i < SKYN; i++) {
        p_m1[i] = (uint16_t)((float)s_m1[i] * k);
        p_m2[i] = (uint16_t)((float)s_m2[i] * k);
        p_cl_lo[i] = (uint16_t)((float)s_cl_lo[i] * k);
        p_cl_hi[i] = (uint16_t)((float)s_cl_hi[i] * k);
        p_cl_mid[i] = (uint16_t)(((float)s_cl_lo[i] * 0.6f + (float)s_cl_hi[i] * 0.4f) * k);
        p_sun_lo[i] = s_sun_hi[i] ? (uint16_t)((float)s_sun_lo[i] * k) : 0xFFFF;
        p_sun_hi[i] = (uint16_t)((float)s_sun_hi[i] * k);
        p_star[i] = (uint16_t)((float)s_star[i] * k);
        int top = p_m2[i] > p_cl_hi[i] ? p_m2[i] : p_cl_hi[i];
        if (p_sun_hi[i] > top) top = p_sun_hi[i];
        if (p_star[i] > top) top = p_star[i];
        if (top > p_top) p_top = top;
    }
    s_bg_h = H;
}

/* per frame: where each screen column looks, the horizon, and each sea row in world space */
static void background_frame(const e3d_camera *cam, float focal)
{
    const look_t *lk = g_look;
    if (s_bg_h != H)
        make_background();
    float p = cam->pitch * RAD, hd = cam->heading * RAD;
    float cp = cosf(p), sp = sinf(p), ch = cosf(hd), sh = sinf(hd);
    s_hz = (int)floorf((float)H * 0.5f + focal * tanf(p) + 0.5f);
    s_cloud_shift = (int)(g_clock * 3.f);
    for (int x = 0; x < W; x++) {
        float a = cam->heading + atanf(((float)x + 0.5f - W * 0.5f) / focal) * DEG;
        int i = (int)(a * (SKYN / 360.f)) % SKYN;
        if (i < 0) i += SKYN;
        s_col[x] = (uint16_t)i;
    }
    float camh = cam->pos[1] - SEA;
    if (camh < 0.3f)
        camh = 0.3f;
    float u0 = (0.5f - W * 0.5f) / focal;
    uint32_t sea = lk->sea, hi = lk->sea_hi;
    for (int y = 0; y < H; y++) {
        srow_t *g = &s_sr[y];
        if (s_hz - y > 0)
            continue;
        float v = ((float)y + 0.5f - H * 0.5f) / focal;
        float den = v * cp - sp;
        float t = den > 1e-4f ? camh / den : 1e6f;
        float fog = expf(-t * lk->fog_density * 0.9f);
        float near = clampf(1.f - t / 300.f, 0, 1);
        uint32_t base = mix_rgb(sea, shade_rgb(sea, 1.15f), near);
        g->c0 = rgb565(mix_rgb(lk->fog, base, fog));
        g->c1 = rgb565(mix_rgb(lk->fog, mix_rgb(base, hi, 0.35f + 0.65f * near), fog));
        g->c2 = rgb565(mix_rgb(lk->fog, lk->sea_kind == SEA_LAVA ? 0x5A1A0C : mix_rgb(base, hi, 0.18f), fog));
        if (t > 700.f) {
            g->pat = 0;
            continue;
        }
        float fd = t * (cp + v * sp);
        float lat = t / focal;
        float X = cam->pos[0] + ch * fd - sh * t * u0, Z = cam->pos[2] + sh * fd + ch * t * u0;
        /* the waves drift */
        X += g_clock * 0.6f;
        g->x = (int32_t)(X * 65536.f);
        g->z = (int32_t)(Z * 65536.f);
        g->dx = (int32_t)(-sh * lat * 65536.f);
        g->dz = (int32_t)(ch * lat * 65536.f);
        g->pat = (uint8_t)(lk->sea_kind + 1);
    }
}

static void bg_row(uint16_t *row, int y, int w, void *ctx)
{
    int up = s_hz - y;
    if (up <= 0) {
        const srow_t *g = &s_sr[y];
        uint16_t c0 = g->c0, c1 = g->c1, c2 = g->c2;
        int32_t X = g->x, Z = g->z;
        const int32_t dX = g->dx, dZ = g->dz;
        if (g->pat == 1) {
            /* water: long wave crests and a few glints */
            for (int x = 0; x < w; x++) {
                uint32_t a = (uint32_t)((X >> 14) + (Z >> 17) + ((Z >> 14) & 7));
                row[x] = (a & 31) == 0 ? c1 : (a & 31) < 3 ? c2 : c0;
                X += dX;
                Z += dZ;
            }
        } else if (g->pat == 2) {
            /* lava: plates of dark crust of different sizes, molten gaps between them */
            for (int x = 0; x < w; x++) {
                int32_t wx = X + (Z >> 1), wz = Z - (X >> 2);     /* sheared: the plates do not line up with the view */
                uint32_t hx = (uint32_t)(wx >> 18) * 0x9E3779B1u ^ (uint32_t)(wz >> 18) * 0x85EBCA77u;
                uint32_t ex = (uint32_t)(wx >> 13) & 31, ez = (uint32_t)(wz >> 13) & 31, in = 1 + ((hx >> 24) & 7);
                bool plate = (hx >> 30) != 0 && ex >= in && ez >= in && ex < 32 - in / 2 && ez < 32 - in / 2;
                row[x] = plate ? c2 : (ex + ez + in) & 8 ? c0 : c1;
                X += dX;
                Z += dZ;
            }
        } else if (g->pat == 3) {
            /* ice: plates with pale cracks */
            for (int x = 0; x < w; x++) {
                int32_t cx = (X >> 16) + ((Z >> 18) & 3), cz = Z >> 16;
                row[x] = ((cx & 15) == 0 || (cz & 15) == 0) ? c1 : (((cx >> 4) ^ (cz >> 4)) & 1) ? c2 : c0;
                X += dX;
                Z += dZ;
            }
        } else
            for (int x = 0; x < w; x++)
                row[x] = c0;
        return;
    }
    uint16_t sky = s_sky_tab[up < HMAX * 2 ? up : HMAX * 2 - 1];
    if (up > p_top) {
        for (int x = 0; x < w; x++)
            row[x] = sky;
        return;
    }
    uint16_t m1 = s_c_m1, m2 = s_c_m2, cl = s_c_cl, cl2 = s_c_cl2, sun = s_c_sun, star = s_c_star;
    int sh = s_cloud_shift;
    for (int x = 0; x < w; x++) {
        int i = s_col[x];
        uint16_t c = sky;
        int ci = (i + sh) & (SKYN - 1);
        if (up <= p_m1[i])
            c = m1;
        else if (up <= p_m2[i])
            c = m2;
        else if (up >= p_cl_lo[ci] && up <= p_cl_hi[ci] && p_cl_hi[ci])
            c = up < p_cl_mid[ci] ? cl2 : cl;
        else if (up >= p_sun_lo[i] && up <= p_sun_hi[i])
            c = sun;
        else if (up == p_star[i])
            c = star;
        row[x] = c;
    }
}

/* ── Drawing the world ─────────────────────────────────────────────────── */
/* how far the land is drawn: eased down when the frame rate drops, back up when it recovers */
static float g_view = 210.f;
#define VIEW_MAX 210.f
#define VIEW_MIN 120.f
static void view_adapt(float fps)
{
    if (fps <= 0)
        return;
    if (fps < 24.f)
        g_view = fmaxf(VIEW_MIN, g_view - 15.f);
    else if (fps > 34.f)
        g_view = fminf(VIEW_MAX, g_view + 8.f);
}

static float s_fwd[3], s_right[3], s_tanx, s_tany;
static bool in_view(const float *c, float r, float maxd)
{
    const float *cp = g_cam_p;
    float d[3] = { c[0] - cp[0], c[1] - cp[1], c[2] - cp[2] };
    float x = v3_dot(d, s_fwd);
    if (x < -r)
        return false;
    float dist2 = v3_dot(d, d);
    if (dist2 > (maxd + r) * (maxd + r))
        return false;
    float z = fabsf(v3_dot(d, s_right));
    return z - r * 1.5f <= x * s_tanx;
}
static float cam_dist(const float *c)
{
    float d[3] = { c[0] - g_cam_p[0], c[1] - g_cam_p[1], c[2] - g_cam_p[2] };
    return sqrtf(v3_dot(d, d));
}

static e3d_mat mat_tr(float x, float y, float z, float yaw, float s)
{
    float p[3] = { x, y, z };
    return mat_pose(p, yaw, 0, 0, s);
}

static void draw_dragon(e3d_t *e)
{
    if (P.mode == M_DEAD && P.dead_t < (P.hp <= 0 ? 1.3f : 1.0f))
        return;
    if (P.inv_t > 0 && g_state == ST_PLAY && fmodf(P.inv_t, 0.16f) < 0.07f)
        return;
    float hs = sqrtf(P.v[0] * P.v[0] + P.v[2] * P.v[2]);
    float run = fminf(1.f, hs / 6.f);
    float ph = P.walk_ph * 1.1f;
    float bob = P.ground ? fabsf(sinf(ph * (float)M_PI)) * 0.06f * run : 0;
    float pitch = P.mode == M_GLIDE ? -0.08f : P.charging ? -0.1f : P.mode == M_AIR ? clampf(P.v[1] * 0.02f, -0.25f, 0.2f) : 0;
    float roll = 0;
    if (P.mode == M_GLIDE)
        roll = clampf(wrap_pi(P.yaw - g_cam_yaw) * 0.0f, -0.3f, 0.3f);
    float pos[3] = { P.p[0], P.p[1] + bob + (P.squash > 0 ? -P.squash * 0.2f : 0), P.p[2] };
    int32_t tint = -1;
    e3d_mat base = mat_pose(pos, P.yaw, pitch, roll, 1.f);
    draw_obj(e, m_dragon[DM_BODY], &base, tint, E3D_NO_SHADOW);
    /* head */
    float nod = P.ground ? sinf(ph * 2.f * (float)M_PI) * 0.05f * run : 0;
    float idle = sinf(g_clock * 1.7f) * 0.04f;
    e3d_mat hm = mat_child(&base, 0.34f, 0.58f, 0, 0, P.head + nod + idle, 0);
    draw_obj(e, m_dragon[DM_HEAD], &hm, tint, E3D_NO_SHADOW);
    /* wings: folded on the back, half open in a jump, spread in a glide */
    float w = P.wing;
    float flap = P.mode == M_AIR ? sinf(g_clock * 16.f) * 0.5f * (1.f - w * 0.5f) : P.mode == M_GLIDE ? sinf(g_clock * 3.f) * 0.06f : 0;
    float fold_roll = lerpf(-0.25f, 0.05f, w) + flap, fold_yaw = lerpf(1.35f, 0.0f, w), fold_s = lerpf(0.72f, 1.f, w);
    e3d_mat wr = mat_child(&base, 0.08f, 0.70f, 0.13f, fold_yaw, 0, fold_roll);
    e3d_mat wl = mat_child(&base, 0.08f, 0.70f, -0.13f, -fold_yaw, 0, -fold_roll);
    if (fold_s < 0.999f) {
        e3d_mat sc;
        e3d_scale(&sc, fold_s, fold_s, fold_s);
        e3d_mul(&wr, &sc);
        e3d_mul(&wl, &sc);
    }
    draw_obj(e, m_dragon[DM_WING_R], &wr, tint, E3D_NO_SHADOW | E3D_DOUBLE_SIDED);
    draw_obj(e, m_dragon[DM_WING_L], &wl, tint, E3D_NO_SHADOW | E3D_DOUBLE_SIDED);
    /* legs: diagonal pairs swing together; tucked in the air */
    static const float hip[4][3] = { { 0.22f, 0.40f, 0.16f }, { 0.22f, 0.40f, -0.16f }, { -0.28f, 0.40f, 0.17f }, { -0.28f, 0.40f, -0.17f } };
    for (int l = 0; l < 4; l++) {
        float sw;
        if (!P.ground)
            sw = l < 2 ? 0.7f : -0.7f;
        else {
            float phase = (l == 0 || l == 3) ? 0.f : (float)M_PI;
            sw = sinf(ph * 2.f * (float)M_PI + phase) * 0.75f * run;
        }
        e3d_mat lm = mat_child(&base, hip[l][0], hip[l][1], hip[l][2], 0, sw, 0);
        draw_obj(e, m_dragon[DM_LEG], &lm, tint, E3D_NO_SHADOW);
    }
    /* tail: sways */
    float sway = sinf(g_clock * 2.3f) * 0.22f + (P.ground ? sinf(ph * (float)M_PI) * 0.25f * run : 0);
    e3d_mat tm = mat_child(&base, -0.44f, 0.50f, 0, sway, P.mode == M_GLIDE ? 0.15f : -0.05f, 0);
    draw_obj(e, m_dragon[DM_TAIL], &tm, tint, E3D_NO_SHADOW);
}

/* a dark disc on the land under (x, z), as wide as r */
static void draw_shadow(e3d_t *e, float x, float z, float y_above, float r)
{
    float h = support_h(x, z, y_above, NULL);
    if (h < SEA - 0.05f)
        h = SEA;
    float gap = y_above - h;
    if (gap > 25.f)
        return;
    float k = r * clampf(1.f - gap / 30.f, 0.4f, 1.f);
    float n[3];
    ground_n(x, z, n);
    /* a basis on the slope */
    float f[3] = { 1, 0, 0 }, rt[3], fw[3];
    v3_cross(rt, f, n);
    v3_norm(rt);
    v3_cross(fw, n, rt);
    e3d_mat m;
    for (int i = 0; i < 3; i++) {
        m.m[i][0] = fw[i] * k;
        m.m[i][1] = n[i];
        m.m[i][2] = rt[i] * k;
    }
    m.m[0][3] = x;
    m.m[1][3] = h + 0.08f;
    m.m[2][3] = z;
    m.m[3][0] = m.m[3][1] = m.m[3][2] = 0;
    m.m[3][3] = 1;
    draw_obj(e, m_blob, &m, 0x141414, E3D_UNLIT | E3D_NO_SHADOW);
}

static void draw_enemy(e3d_t *e, const enemy_t *en)
{
    float s = 1.f, sy = 1.f;
    int32_t col = en->hit_t > 0 && ((int)(en->hit_t * 20.f) & 1) ? 0xFFFFFF : -1;
    float y = en->p[1];
    if (!en->alive) {
        if (en->dead_t <= 0)
            return;
        float t = en->dead_t / 0.8f;
        if (en->burnt) {
            col = 0x2A2018;
            sy = t;
        } else {
            y += (1.f - t) * 3.f;
            s = t;
        }
    }
    float yaw = en->yaw, pitch = 0, roll = 0;
    if (en->type == T_GRUB) {
        sy *= 1.f + sinf(en->anim * 6.f) * 0.08f;
        roll = sinf(en->anim * 3.f) * 0.08f;
    } else if (en->type == T_BEETLE) {
        if (en->state == 1)
            pitch = -0.15f + sinf(g_clock * 40.f) * 0.05f;
        y += fabsf(sinf(en->anim * 7.f)) * 0.05f;
    } else {
        y += fabsf(sinf(en->anim * 3.f)) * 0.08f;
        if (en->state == 2)
            pitch = 0.12f;
        else if (en->state == 3)
            pitch = -0.15f;
    }
    float p[3] = { en->p[0], y, en->p[2] };
    e3d_mat m = mat_pose(p, yaw, pitch, roll, s);
    if (sy != 1.f)
        for (int i = 0; i < 3; i++)
            m.m[i][1] *= sy;
    static const int mesh[3] = { EM_GRUB, EM_BEETLE, EM_BRUTE };
    draw_obj(e, m_enemy[mesh[en->type]], &m, col, E3D_NO_SHADOW);
    if (en->type == T_BRUTE && (en->alive || en->dead_t > 0)) {
        /* the club in the right hand: raised in the wind-up, down in the swing */
        float a = en->state == 2 ? -1.9f + 0.2f * sinf(g_clock * 30.f) : en->state == 3 ? 0.9f : en->state == 4 ? 0.6f - en->st : -0.4f + sinf(en->anim * 3.f) * 0.2f;
        e3d_mat cm = mat_child(&m, 0.35f, 1.0f, 0.85f, 0, a, 0.3f);
        draw_obj(e, m_enemy[EM_CLUB], &cm, col, E3D_NO_SHADOW);
    }
}


static void draw_scene(e3d_t *e)
{
    const look_t *lk = g_look;
    float hd = g_cam_h * RAD, pt = g_cam_pt * RAD;
    v3_set(s_fwd, cosf(pt) * cosf(hd), sinf(pt), cosf(pt) * sinf(hd));
    v3_set(s_right, -sinf(hd), 0, cosf(hd));
    float focal = g_focal * (float)W / 400.f;
    s_tanx = (W * 0.5f + 8.f) / focal;
    s_tany = (H * 0.5f + 8.f) / focal;
    e3d_mat id;
    e3d_identity(&id);
    float lod_k = g_view / VIEW_MAX;
    /* the land and what stands on it */
    for (int i = 0; i < NCH * NCH; i++) {
        const chunk_t *k = &g_chunk[i];
        if (!in_view(k->c, k->r, g_view))
            continue;
        float d = cam_dist(k->c) - k->r;
        int lod = d < 34.f * lod_k ? 0 : d < 90.f * lod_k ? 1 : 2;
        if (k->land[lod])
            draw_obj(e, k->land[lod], &id, -1, E3D_NO_SHADOW);
        if (d < 150.f * lod_k) {
            const e3d_mesh *s = k->scen[d < 50.f * lod_k ? 0 : 1];
            if (s)
                draw_obj(e, s, &id, -1, E3D_NO_SHADOW);
        }
    }
    /* floating stones */
    for (int i = 0; i < g_nplat; i++) {
        const plat_t *q = &g_plat[i];
        float c[3] = { q->x, q->y - 1.f, q->z };
        if (!in_view(c, q->r + 1.5f, g_view))
            continue;
        e3d_mat m = mat_tr(q->x, q->y, q->z, (float)i, 1.f);
        draw_obj(e, q->mesh, &m, -1, E3D_NO_SHADOW);
    }
    /* beacons */
    for (int i = 0; i < 4; i++) {
        const beacon_t *b = &g_beacon[i];
        float c[3] = { b->p[0], b->p[1] + 1.5f, b->p[2] };
        if (!in_view(c, 3.f, g_view))
            continue;
        e3d_mat m = mat_tr(b->p[0], b->p[1], b->p[2], 0, 1.f);
        draw_obj(e, m_pillar, &m, -1, E3D_NO_SHADOW);
        draw_obj(e, m_bowl, &m, -1, E3D_NO_SHADOW);
        if (b->lit) {
            float grow = fminf(1.f, b->t * 2.f);
            float fl = 0.85f + 0.3f * frand();
            e3d_mat fm = mat_tr(b->p[0], b->p[1] + 2.62f, b->p[2], g_clock * 3.f + (float)i, grow * fl);
            draw_obj(e, m_fire, &fm, -1, E3D_UNLIT | E3D_NO_SHADOW);
            if (frand() < 0.3f) {
                float p[3] = { b->p[0] + frand2() * 0.3f, b->p[1] + 3.2f, b->p[2] + frand2() * 0.3f }, v[3] = { frand2() * 0.4f, 2.5f, frand2() * 0.4f };
                spawn_part(p, v, 0.6f, 0.2f, 0xFFA030, PK_FIRE, -1.f);
            }
        }
    }
    /* the portal */
    {
        float c[3] = { g_portal_p[0], g_portal_p[1] + 2.f, g_portal_p[2] };
        if (in_view(c, 4.f, g_view)) {
            e3d_mat m = mat_tr(g_portal_p[0], g_portal_p[1], g_portal_p[2], g_portal_yaw, 1.f);
            draw_obj(e, m_portal_ring, &m, -1, E3D_NO_SHADOW);
            float spin = g_lit == 4 ? g_clock * 2.5f : g_clock * 0.3f;
            e3d_mat dm = mat_child(&m, 0, 1.75f, 0, 0, 0, spin);
            /* the disc spins round its centre: shift it so its centre (0, 1.75, 0) stays put */
            e3d_mat off;
            e3d_move(&off, 0, -1.75f, 0);
            e3d_mul(&dm, &off);
            draw_obj(e, g_lit == 4 ? m_portal_disc : m_portal_off, &dm, -1, E3D_UNLIT | E3D_NO_SHADOW | E3D_DOUBLE_SIDED);
        }
    }
    /* urns and chests */
    for (int i = 0; i < g_nbox; i++) {
        const box_t *b = &g_box[i];
        if (!b->alive || !in_view(b->p, 1.5f, 120.f))
            continue;
        float wob = b->hit_t > 0 ? sinf(b->hit_t * 60.f) * 0.1f : 0;
        float p[3] = { b->p[0], b->p[1], b->p[2] };
        e3d_mat m = mat_pose(p, (float)i * 0.7f, 0, wob, 1.f);
        draw_obj(e, b->kind == T_URN ? m_urn : m_chest, &m, -1, E3D_NO_SHADOW);
    }
    /* berries */
    for (int i = 0; i < g_nberry; i++) {
        const berry_t *b = &g_berry[i];
        if (!b->alive || !in_view(b->p, 1.f, 90.f))
            continue;
        e3d_mat m = mat_tr(b->p[0], b->p[1] + 0.1f * sinf(g_clock * 3.f + (float)i), b->p[2], g_clock, 1.f);
        draw_obj(e, m_berry, &m, -1, E3D_NO_SHADOW);
    }
    /* gems */
    for (int i = 0; i < MAX_GEMS; i++) {
        const gem_t *g = &g_gem[i];
        if (!g->state || !in_view(g->p, 0.6f, 110.f))
            continue;
        float s = g->type == GEM_STAR ? 1.3f : 1.f;
        if (g->state == 3)
            s *= fmaxf(0.3f, 1.f - g->t * 1.5f);
        float y = g->p[1] + (g->state == 1 ? sinf(g->t * 2.5f) * 0.12f : 0);
        e3d_mat m = mat_tr(g->p[0], y, g->p[2], g->t * 2.4f, s);
        draw_obj(e, m_gem[g->type], &m, -1, E3D_NO_SHADOW | E3D_FLAT);
    }
    /* creatures */
    for (int i = 0; i < g_nen; i++) {
        const enemy_t *en = &g_en[i];
        if ((!en->alive && en->dead_t <= 0) || !in_view(en->p, 2.5f, 130.f))
            continue;
        draw_enemy(e, en);
        if (en->alive && cam_dist(en->p) < 40.f)
            draw_shadow(e, en->p[0], en->p[2], en->p[1] + 0.5f, k_en_r[en->type] * 0.9f);
    }
    /* the dragon and its shadow */
    draw_dragon(e);
    if (P.mode != M_DEAD)
        draw_shadow(e, P.p[0], P.p[2], P.p[1] + 0.3f, 0.42f);
    /* particles */
    for (int i = 0; i < MAX_PART; i++) {
        const part_t *q = &g_part[i];
        if (q->life <= 0 || !in_view(q->p, 1.f, 80.f))
            continue;
        float t = q->life / q->life0;   /* 1 .. 0 */
        if (q->kind == PK_FIRE) {
            uint32_t c = t > 0.7f ? mix_rgb(0xFF8020, 0xFFF0A0, (t - 0.7f) / 0.3f) : t > 0.35f ? mix_rgb(0xD03010, 0xFF8020, (t - 0.35f) / 0.35f)
                                                                                                : mix_rgb(0x302020, 0xD03010, t / 0.35f);
            float s = q->size * (0.5f + 2.2f * (1.f - t));
            e3d_mat m = mat_pose(q->p, t * 9.f + (float)i, t * 5.f, 0, s);
            draw_obj(e, m_puff, &m, (int32_t)c, E3D_UNLIT | E3D_NO_SHADOW | E3D_FLAT);
        } else if (q->kind == PK_RING) {
            float s = q->size * (1.f + (1.f - t) * 1.6f);
            e3d_mat m = mat_tr(q->p[0], q->p[1], q->p[2], 0, s);
            draw_obj(e, m_ring, &m, (int32_t)mix_rgb(0xFFFFFF, q->rgb, 1.f - t), E3D_UNLIT | E3D_NO_SHADOW | E3D_DOUBLE_SIDED);
        } else if (q->kind == PK_DUST) {
            float s = q->size * (0.6f + (1.f - t) * 1.5f);
            e3d_mat m = mat_pose(q->p, (float)i, 0.3f, 0, s);
            draw_obj(e, m_puff, &m, (int32_t)mix_rgb(lk->fog, q->rgb, t), E3D_UNLIT | E3D_NO_SHADOW | E3D_FLAT);
        } else {
            e3d_mat m = mat_pose(q->p, t * 12.f + (float)i, t * 7.f, 0, q->size * (0.4f + 0.6f * t));
            draw_obj(e, m_spark, &m, (int32_t)q->rgb, E3D_UNLIT | E3D_NO_SHADOW | E3D_FLAT);
        }
    }
}

/* the camera and the light for the next frame: set before e3d_begin, which builds the view from them */
static void scene_setup(e3d_t *e)
{
    const look_t *lk = g_look;
    e3d_camera *cam = e3d_cam(e);
    *cam = (e3d_camera){ .pos = { g_cam_p[0], g_cam_p[1], g_cam_p[2] }, .heading = g_cam_h, .pitch = g_cam_pt };
    float focal = g_focal * (float)W / 400.f;
    e3d_set_focal(e, focal);
    *e3d_light_get(e) = (e3d_light){ .pos = { P.p[0] + lk->light_dir[0] * 1000.f, P.p[1] + lk->light_dir[1] * 1000.f, P.p[2] + lk->light_dir[2] * 1000.f },
                                     .r = (float)(lk->light_rgb >> 16), .g = (float)((lk->light_rgb >> 8) & 255),
                                     .b = (float)(lk->light_rgb & 255) };
    background_frame(cam, focal);
}

static void apply_look(e3d_t *e)
{
    e3d_params *par = e3d_params_get(e);
    par->ambient = g_look->ambient;
    par->diffuse = 0.78f;
    par->specular = 0.18f;
    par->shininess = 12.f;
    par->shadows = false;
    par->fog_density = g_look->fog_density;
    par->fog_rgb565 = rgb565(g_look->fog);
    par->fog_keep_bg = true;
    make_background();
}

/* ── HUD ───────────────────────────────────────────────────────────────── */
static int g_ts = 2;     /* text scale for this frame size */
static void text_at(uint16_t *px, int x, int y, const char *s, uint16_t col, int scale)
{
    int sh = scale > 1 ? scale / 2 + 1 : 1;
    e3d_text(px, W, W, H, x + sh, y + sh, s, 0x0000, scale);
    e3d_text(px, W, W, H, x, y, s, col, scale);
}
static int text_w(const char *s, int scale) { return (int)strlen(s) * 6 * scale - scale; }
static void text_c(uint16_t *px, int y, const char *s, uint16_t col, int scale) { text_at(px, (W - text_w(s, scale)) / 2, y, s, col, scale); }
static void shade_box(uint16_t *px, int x, int y, int w, int h)
{
    for (int j = y < 0 ? 0 : y; j < y + h && j < H; j++)
        for (int i = x < 0 ? 0 : x; i < x + w && i < W; i++) {
            uint16_t c = px[j * W + i];
            px[j * W + i] = (uint16_t)((c >> 1) & 0x7BEF) - (uint16_t)((c >> 3) & 0x18E3);
        }
}
static void tri_fill(uint16_t *px, float ax, float ay, float bx, float by, float cx, float cy, uint16_t col)
{
    int y0 = (int)fmaxf(0, floorf(fminf(ay, fminf(by, cy)))), y1 = (int)fminf(H - 1, ceilf(fmaxf(ay, fmaxf(by, cy))));
    const float P2[3][2] = { { ax, ay }, { bx, by }, { cx, cy } };
    for (int y = y0; y <= y1; y++) {
        float fy = (float)y + 0.5f, xl = 1e9f, xr = -1e9f;
        for (int i = 0; i < 3; i++) {
            const float *p = P2[i], *q = P2[(i + 1) % 3];
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
static void disc_fill(uint16_t *px, float cx, float cy, float r, uint16_t col)
{
    for (int y = (int)fmaxf(0, cy - r); y <= (int)fminf(H - 1, cy + r); y++) {
        float dy = (float)y + 0.5f - cy, w = sqrtf(fmaxf(0, r * r - dy * dy));
        for (int x = (int)fmaxf(0, ceilf(cx - w - 0.5f)); x <= (int)fminf(W - 1, floorf(cx + w - 0.5f)); x++)
            px[y * W + x] = col;
    }
}
/* a gem icon, r pixels across */
static void icon_gem(uint16_t *px, float x, float y, float r, uint32_t c0, uint32_t c1)
{
    tri_fill(px, x - r, y, x + r, y, x, y + r * 1.3f, rgb565(c1));
    tri_fill(px, x - r, y, x - r * 0.5f, y - r * 0.6f, x + r * 0.5f, y - r * 0.6f, rgb565(c0));
    tri_fill(px, x - r, y, x + r * 0.5f, y - r * 0.6f, x + r, y, rgb565(c0));
    tri_fill(px, x - r * 0.35f, y, x + r * 0.35f, y, x, y + r * 1.1f, rgb565(shade_rgb(c1, 1.3f)));
}
static void icon_heart(uint16_t *px, float x, float y, float r, bool full)
{
    uint16_t edge = 0x0000, fill = full ? rgb565(0xF02848) : rgb565(0x503038);
    float e = fmaxf(1.f, r * 0.18f);
    disc_fill(px, x - r * 0.5f, y, r * 0.55f + e, edge);
    disc_fill(px, x + r * 0.5f, y, r * 0.55f + e, edge);
    tri_fill(px, x - r * 1.05f - e, y + r * 0.1f, x + r * 1.05f + e, y + r * 0.1f, x, y + r * 1.25f + e * 1.5f, edge);
    disc_fill(px, x - r * 0.5f, y, r * 0.55f, fill);
    disc_fill(px, x + r * 0.5f, y, r * 0.55f, fill);
    tri_fill(px, x - r * 1.05f, y + r * 0.1f, x + r * 1.05f, y + r * 0.1f, x, y + r * 1.25f, fill);
    if (full)
        disc_fill(px, x - r * 0.55f, y - r * 0.15f, r * 0.18f, rgb565(0xFFB0C0));
}
static void icon_flame(uint16_t *px, float x, float y, float r, bool lit)
{
    /* a bowl on a stub, with a flame when lit */
    uint16_t stone = rgb565(lit ? 0xB0A890 : 0x787468), dark = rgb565(0x302820);
    tri_fill(px, x - r * 0.9f, y + r * 0.35f, x + r * 0.9f, y + r * 0.35f, x, y + r * 1.0f, stone);
    tri_fill(px, x - r * 0.25f, y + r * 0.7f, x + r * 0.25f, y + r * 0.7f, x, y + r * 1.5f, stone);
    tri_fill(px, x - r * 0.9f, y + r * 0.35f, x + r * 0.9f, y + r * 0.35f, x, y + r * 0.55f, dark);
    if (lit) {
        disc_fill(px, x, y, r * 0.45f, rgb565(0xFF7018));
        tri_fill(px, x - r * 0.45f, y, x + r * 0.45f, y, x + r * 0.1f, y - r * 1.2f, rgb565(0xFF7018));
        disc_fill(px, x, y + r * 0.1f, r * 0.25f, rgb565(0xFFE060));
    }
}

static void fmt_time(char *out, size_t n, float t)
{
    if (t <= 0) {
        snprintf(out, n, "-:--");
        return;
    }
    int s = (int)(t + 0.5f);
    snprintf(out, n, "%d:%02d", s / 60, s % 60);
}

static void draw_title(uint16_t *px)
{
    int ts = g_ts;
    char buf[64];
    int big = ts * 4;
    text_c(px, H / 7, "CINDERHORN", rgb565(0xFFB040), big);
    text_c(px, H / 7 + big * 9, "A LITTLE DRAGON - THREE WORLDS", rgb565(0xFFF0D0), ts);
    if (((int)(g_state_t * 2.f) & 1) == 0)
        text_c(px, H - H / 5, "PRESS A", rgb565(0xFFFFFF), ts * 2);
    snprintf(buf, sizeof(buf), "WORLD %d: %s", g_level + 1, g_lv->name);
    text_c(px, H - H / 5 + ts * 18, buf, rgb565(0xC0E0FF), ts);
    const record_t *r = &g_rec[g_level];
    if (r->done) {
        char t[16];
        fmt_time(t, sizeof(t), r->best_time);
        snprintf(buf, sizeof(buf), "BEST: %d GEMS   %s", r->best_gems, t);
        text_c(px, H - H / 5 + ts * 28, buf, rgb565(0xFFE080), ts);
    }
}

static void draw_done(uint16_t *px)
{
    int ts = g_ts;
    int bw = W * 3 / 5, bh = ts * 80, bx = (W - bw) / 2, by = H / 2 - bh / 2;
    shade_box(px, bx, by, bw, bh);
    char buf[64], t[16];
    text_c(px, by + ts * 8, "WORLD CLEARED", rgb565(0xFFC040), ts * 2);
    snprintf(buf, sizeof(buf), "GEMS  %d / %d", g_gem_got, g_gem_total);
    text_c(px, by + ts * 30, buf, rgb565(0xFFFFFF), ts);
    fmt_time(t, sizeof(t), g_time);
    snprintf(buf, sizeof(buf), "TIME  %s", t);
    text_c(px, by + ts * 42, buf, rgb565(0xFFFFFF), ts);
    const record_t *r = &g_rec[g_level];
    fmt_time(t, sizeof(t), r->best_time);
    snprintf(buf, sizeof(buf), "BEST  %d GEMS  %s", r->best_gems, t);
    text_c(px, by + ts * 54, buf, rgb565(0xFFE080), ts);
    if (g_state_t > 1.f)
        text_c(px, by + ts * 68, g_level + 1 < NLEVELS ? "A: NEXT WORLD" : "A: BACK TO THE TITLE", rgb565(0xC0FFC0), ts);
}

static void draw_hud(uint16_t *px, float fps)
{
    int ts = g_ts;
    char buf[48];
    if (g_state == ST_TITLE) {
        draw_title(px);
    } else {
        float m = (float)(ts * 6);
        /* gems */
        float r = (float)ts * 5.f;
        icon_gem(px, m + r, m + r * 0.8f, r, 0xFFD040, 0xD08010);
        snprintf(buf, sizeof(buf), "%d / %d", g_gem_got, g_gem_total);
        text_at(px, (int)(m + r * 2.6f), (int)(m + r * 0.8f) - ts * 3, buf, rgb565(0xFFF4C0), ts);
        /* beacons */
        float bx = m + r * 2.6f + (float)text_w(buf, ts) + (float)ts * 14.f;
        for (int i = 0; i < 4; i++)
            icon_flame(px, bx + (float)i * r * 2.4f, m + r * 0.4f, r, g_beacon[i].lit);
        /* hearts */
        for (int i = 0; i < HP_MAX; i++)
            icon_heart(px, (float)W - m - r - (float)(HP_MAX - 1 - i) * r * 2.6f, m + r * 0.6f, r, i < P.hp);
        /* a message */
        if (g_msg_t > 0 && g_state != ST_DONE) {
            int sc = ts * 2;
            if (g_msg_t > 0.4f || ((int)(g_msg_t * 20.f) & 1))
                text_c(px, H / 4, g_msg, rgb565(0xFFF0A0), sc);
        }
        if (g_state == ST_DONE)
            draw_done(px);
        if (g_lit == 4 && g_state == ST_PLAY) {
            /* a pointer to the portal along the bottom */
            float dx = g_portal_p[0] - P.p[0], dz = g_portal_p[2] - P.p[2];
            float a = wrap_pi(atan2f(dz, dx) - g_cam_h * RAD);
            float cx = (float)W * 0.5f + clampf(a / 1.2f, -1.f, 1.f) * (float)W * 0.4f, cy = (float)H - m - r;
            tri_fill(px, cx - r, cy + r * 0.6f, cx + r, cy + r * 0.6f, cx, cy - r * 0.9f, rgb565(0xB070FF));
            text_c(px, (int)(cy - r * 3.f), "PORTAL", rgb565(0xD0B0FF), ts);
        }
    }
    if (g_show_fps) {
        snprintf(buf, sizeof(buf), "%.0f FPS  %dX%d", (double)fps, W, H);
        text_at(px, ts * 4, H - ts * 12, buf, rgb565(0xFFFFFF), ts);
    }
}

/* ── Records and settings ──────────────────────────────────────────────── */
#define DATA_DIR "/sd/data/cinderhorn"
#define REC_PATH DATA_DIR "/records.txt"
#define SET_PATH DATA_DIR "/settings.txt"

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
        fprintf(f, "unlocked %d\n", g_unlocked);
        for (int i = 0; i < NLEVELS; i++)
            fprintf(f, "%d %d %d %.2f\n", i, g_rec[i].done ? 1 : 0, g_rec[i].best_gems, (double)g_rec[i].best_time);
        fclose(f);
    } else {
        FILE *f = fopen(REC_PATH, "r");
        if (!f)
            return;
        int u;
        if (fscanf(f, "unlocked %d ", &u) == 1 && u >= 1 && u <= NLEVELS)
            g_unlocked = u;
        int i, d, g;
        float t;
        while (fscanf(f, "%d %d %d %f", &i, &d, &g, &t) == 4)
            if (i >= 0 && i < NLEVELS) {
                g_rec[i].done = d != 0;
                g_rec[i].best_gems = g;
                g_rec[i].best_time = t;
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
            fprintf(f, "res=%d\ninvert=%d\nmusic=%d\nlevel=%d\n", g_res, g_cam_invert ? 1 : 0, s_music_on ? 1 : 0, g_level);
            fclose(f);
        }
    } else {
        FILE *f = fopen(SET_PATH, "r");
        if (f) {
            int v;
            if (fscanf(f, "res=%d ", &v) == 1 && v >= 0 && v < NRES) g_res = v;
            if (fscanf(f, "invert=%d ", &v) == 1) g_cam_invert = v != 0;
            if (fscanf(f, "music=%d ", &v) == 1) s_music_on = v != 0;
            if (fscanf(f, "level=%d", &v) == 1 && v >= 0 && v < NLEVELS) g_level = v;
            fclose(f);
        }
    }
}

/* ── Input ─────────────────────────────────────────────────────────────── */
static uint32_t s_prev_btn = ~0u;
static int s_touch_n;
static float s_tstick_x, s_tstick_y;      /* where the touch stick started */
static bool s_tstick;

static float stick_axis(int raw)
{
    const float dz = 0.15f;
    float v = clampf((float)raw / 30000.f, -1.f, 1.f), m = fabsf(v);
    if (m <= dz)
        return 0;
    float u = (m - dz) / (1.f - dz);
    return v < 0 ? -u : u;
}

static input_t read_input(papp_surface_t *surf)
{
    input_t a = { 0 };
    papp_gamepad_t pad = { 0 };
    uint32_t held = papp_input_gamepad(0, &pad) ? pad.buttons : 0;
    uint32_t press = held & ~s_prev_btn;
    s_prev_btn = held;
    a.any = press != 0;
    if (press & PAPP_BTN_A) { a.jump = true; a.start = true; }
    if (held & PAPP_BTN_A) a.jump_held = true;
    if (press & PAPP_BTN_B) a.fire = true;
    if (held & (PAPP_BTN_X | PAPP_BTN_R2)) a.charge = true;
    if (pad.rt > 20000) a.charge = true;
    if (press & PAPP_BTN_Y) a.center = true;
    if (press & (PAPP_BTN_START | PAPP_BTN_SELECT)) a.pause = true;
    if (held & PAPP_BTN_L1) a.cam -= 1.f;
    if (held & PAPP_BTN_R1) a.cam += 1.f;
    float rx = stick_axis(pad.rx);
    if (rx != 0) a.cam = rx;
    if (g_cam_invert) a.cam = -a.cam;
    float mx = 0, my = 0;
    if (held & PAPP_BTN_LEFT) mx -= 1.f;
    if (held & PAPP_BTN_RIGHT) mx += 1.f;
    if (held & PAPP_BTN_UP) my += 1.f;
    if (held & PAPP_BTN_DOWN) my -= 1.f;
    if (mx == 0 && my == 0) {
        mx = stick_axis(pad.lx);
        my = -stick_axis(pad.ly);
    }
    /* touch: the left half is a stick round where the finger lands; the right half's bottom row
       is JUMP / FIRE / DASH; the top-right corner pauses; elsewhere on the right: the camera */
    papp_touch_point_t tp[4];
    int n = surf ? papp_disp_touch(surf, tp, 4) : 0;
    bool stick_seen = false;
    for (int i = 0; i < n; i++) {
        float x = tp[i].x, y = tp[i].y;
        bool first = i >= s_touch_n;
        if (x > W - W / 10 && y < H / 8) {
            if (first) a.pause = true;
        } else if (g_state != ST_PLAY) {
            if (first) a.tap = true;
        } else if (x < W / 2) {
            stick_seen = true;
            if (!s_tstick) {
                s_tstick = true;
                s_tstick_x = x;
                s_tstick_y = y;
            }
            float r = (float)H / 6.f;
            mx = clampf((x - s_tstick_x) / r, -1.f, 1.f);
            my = clampf(-(y - s_tstick_y) / r, -1.f, 1.f);
        } else if (y > H * 2 / 3) {
            int b = (int)((x - W / 2) * 3 / (W / 2));
            if (b == 0 && first) a.jump = true;
            if (b == 0) a.jump_held = true;
            if (b == 1 && first) a.fire = true;
            if (b == 2) a.charge = true;
        } else {
            a.cam = x < W * 3 / 4 ? -1.f : 1.f;
        }
    }
    if (!stick_seen)
        s_tstick = false;
    s_touch_n = n > 0 ? n : 0;
    a.mx = mx;
    a.my = my;
    return a;
}

/* ── Building a world for the renderer ─────────────────────────────────── */
static e3d_t *g_e;

static bool world_switch(int li)
{
    bool ok = level_build(li);
    if (g_e)
        apply_look(g_e);
    return ok;
}

#ifndef CH_HOST
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
    g_ts = H >= 560 ? 3 : H >= 400 ? 2 : 1;
    papp_surface_cfg_t cfg = { .width = (uint16_t)W, .height = (uint16_t)H, .format = PAPP_PIXFMT_RGB565,
                               .scale = PAPP_SCALE_FIT_ASPECT, .buffers = 1 };
    g_surf = papp_disp_surface_create(&cfg);
    for (int i = 0; i < 2; i++)
        g_fb[i] = papp_alloc(sizeof(uint16_t) * (size_t)(W * H), PAPP_MEM_LARGE | PAPP_MEM_DMA, 64);
    g_e = g_surf && g_fb[0] && g_fb[1] ? e3d_create(W, H, 2) : NULL;
    if (!g_e)
        return false;
    e3d_set_background_rows(g_e, bg_row, NULL);
    if (g_look)
        apply_look(g_e);
    papp_log("cinderhorn: drawing at %d x %d\n", W, H);
    return true;
}

/* open at g_res, stepping down to smaller sizes when there is not enough memory */
static bool video_open_any(void)
{
    for (;;) {
        if (video_open())
            return true;
        video_close();
        if (g_res >= NRES - 1)
            return false;
        papp_log("cinderhorn: no memory for %d x %d\n", W, H);
        g_res++;
    }
}

static int menu(const char *title, const char *const *items, int n, int initial)
{
    papp_sem_take(g_pres_done, PAPP_WAIT_FOREVER);   /* nothing is being presented now */
    float wv = s_wind_vol, fv = s_fire_vol, cv = s_charge_vol;
    s_wind_vol = s_fire_vol = s_charge_vol = 0;
    s_music_vol = 0.4f;
    int r = papp_ui_menu(title, items, n, initial);
    papp_sem_give(g_pres_done);
    papp_input_flush();
    s_prev_btn = ~0u;          /* ignore the buttons still held */
    s_wind_vol = wv;
    s_fire_vol = fv;
    s_charge_vol = cv;
    s_music_vol = 1.f;
    return r;
}

static bool change_world(int li)
{
    if (li == g_level)
        return true;
    if (!world_switch(li)) {
        papp_log("cinderhorn: out of memory building %s\n", k_levels[li]->name);
        return world_switch(0);
    }
    return true;
}

/* returns false to exit */
static bool pause_menu(void)
{
    const char *const items[] = { "Resume", "Restart world", "Quit to title", "Exit game" };
    int r = menu("Paused", items, 4, 0);
    if (r == 1)
        start_play();
    else if (r == 2)
        start_attract();
    return r != 3 && !papp_exit_requested();
}

static void settings_menu(void)
{
    for (;;) {
        char res[48], cam[32], mus[24];
        snprintf(res, sizeof(res), "Resolution: %d x %d", k_res[g_res][0], k_res[g_res][1]);
        snprintf(cam, sizeof(cam), "Camera turn: %s", g_cam_invert ? "inverted" : "normal");
        snprintf(mus, sizeof(mus), "Music: %s", s_music_on ? "on" : "off");
        const char *ci[4] = { res, cam, mus, "Back" };
        int c = menu("Settings", ci, 4, 0);
        if (c == 0) {
            int q = menu("Resolution", k_res_name, NRES, g_res);
            if (q >= 0 && q != g_res) {
                g_res = q;
                g_res_dirty = true;     /* applied when the menu closes */
            }
        } else if (c == 1)
            g_cam_invert = !g_cam_invert;
        else if (c == 2)
            s_music_on = !s_music_on;
        else
            break;
    }
}

/* the title menu; returns false to exit */
static bool title_menu(void)
{
    for (;;) {
        char wld[48];
        snprintf(wld, sizeof(wld), "World: %s", k_levels[g_level]->name);
        const char *items[5] = { "Play", wld, "Settings", "How to play", "Exit game" };
        int r = menu("Cinderhorn", items, 5, 0);
        if (r < 0)
            return !papp_exit_requested();
        if (r == 0) {
            start_play();
            return true;
        }
        if (r == 4)
            return false;
        if (r == 1) {
            const char *names[NLEVELS];
            char nb[NLEVELS][48];
            for (int i = 0; i < NLEVELS; i++) {
                snprintf(nb[i], sizeof(nb[i]), "%d. %s%s", i + 1, k_levels[i]->name, i < g_unlocked ? (g_rec[i].done ? "  (cleared)" : "") : "  (locked)");
                names[i] = nb[i];
            }
            int c = menu("World", names, NLEVELS, g_level);
            if (c >= 0 && c < g_unlocked) {
                change_world(c);
                start_attract();
            }
        } else if (r == 2)
            settings_menu();
        else if (r == 3)
            papp_ui_message("How to play",
                            "Light the four beacons with your fire, then fly through the portal.\n\n"
                            "Stick / D-pad: move.  A: jump, A again in the air: glide.\n"
                            "B: breathe fire.  X or R2 (hold): charge.\n"
                            "L1 / R1 or the right stick: turn the camera.  Y: camera behind.\n\n"
                            "Grubs: fire or charge.  Beetles: charge (fire bounces off).\n"
                            "Brutes: too big to charge, two blasts of fire.\n"
                            "Urns: fire or charge.  Iron chests: charge.  Berries heal.");
        settings_io(true);
    }
}

int main(int argc, char **argv)
{
    papp_disp_info_t di = { 0 };
    if (papp_disp_info(&di))
        res_init(di.width, di.height);
    else
        res_init(1024, 600);
    settings_io(false);
    records_io(false);
    int start_level = g_level;
    for (int i = 1; i < argc; i++) {
        if (!strcmp(argv[i], "auto"))
            g_auto = true;
        else if (!strcmp(argv[i], "fps"))
            g_show_fps = true;
        else if (!strcmp(argv[i], "level") && i + 1 < argc)
            start_level = (int)clampf((float)atoi(argv[++i]), 0, NLEVELS - 1);
        else if (!strcmp(argv[i], "res") && i + 1 < argc)
            g_res = (int)clampf((float)atoi(argv[++i]), 0, NRES - 1);
    }
    if (start_level >= g_unlocked && !g_auto)
        start_level = 0;
    srand((unsigned)papp_time_us());

    g_pres_go = papp_sem_create(0, 1);
    g_pres_done = papp_sem_create(1, 1);
    bool vid = g_pres_go && g_pres_done && video_open_any();
    papp_thread_cfg_t pcfg = { .name = "ch_present", .stack_size = 4096, .prio = PAPP_PRIO_HIGH, .core = 0 };
    papp_thread_t *pres = vid ? papp_thread_create(presenter_main, NULL, &pcfg) : NULL;
    bool ok = pres && g_e && b_init() && make_models() && world_switch(start_level);
    b_free();
    if (!ok) {
        papp_log("cinderhorn: out of memory\n");
        return 1;
    }

    g_snd_lock = papp_mutex_create(false);
    papp_audio_cfg_t acfg = { .rate = SR, .channels = 1, .format = PAPP_AUDIO_S16, .latency_ms = 60 };
    g_snd = g_snd_lock ? papp_audio_open(&acfg) : NULL;
    papp_thread_cfg_t acfg_t = { .name = "ch_audio", .stack_size = 4096, .prio = PAPP_PRIO_HIGH, .core = 1 };
    papp_thread_t *aud = g_snd ? papp_thread_create(audio_main, NULL, &acfg_t) : NULL;
    if (!aud && g_snd) {
        papp_audio_close(g_snd);
        g_snd = NULL;
    }

    if (g_auto)
        start_play();
    else
        start_attract();
    papp_log("cinderhorn: running%s, %s, %d x %d\n", g_auto ? " (auto)" : "", g_lv->name, W, H);

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
        input_t in = read_input(g_surf);
        if (g_auto && g_state == ST_PLAY)
            in = ai_input(dt);
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
                    cur = 0;
                }
                last = papp_time_us();
                continue;
            }
        } else if (in.pause && g_state == ST_PLAY) {
            run = pause_menu();
            last = papp_time_us();
            continue;
        } else if (g_state == ST_DONE && g_state_t > 1.f && (go || g_auto)) {
            records_io(true);
            if (g_level + 1 < NLEVELS) {
                change_world(g_level + 1);
                start_play();
            } else
                start_attract();
            last = papp_time_us();
            continue;
        }
        /* ride over a slow frame in two steps */
        if (dt > 0.034f) {
            update(&in, dt * 0.5f);
            input_t none = in;
            none.jump = none.fire = none.center = false;
            update(&none, dt * 0.5f);
        } else
            update(&in, dt);

        uint16_t *px = g_fb[cur];
        e3d_t *e = g_e;
        scene_setup(e);
        e3d_begin(e, px, W);
        draw_scene(e);
        e3d_end(e);
        draw_hud(px, fps);

        frames++;
        if (now - stat_t >= 1000000) {
            fps = (float)frames * 1e6f / (float)(now - stat_t);
            frames = 0;
            stat_t = now;
            view_adapt(fps);
        }
        if (now - log_t >= 5000000) {
            const e3d_stats *st = e3d_get_stats(e);
            papp_log("cinderhorn: %.1f fps at %dx%d, state %d, gems %d/%d, beacons %d, objs %d, tris %d/%d, "
                     "geom %d us, raster %d us, view %.0f m\n",
                     (double)fps, W, H, g_state, g_gem_got, g_gem_total, g_lit, st->objects, st->tris_drawn, st->tris_in,
                     st->t_geom_us, st->t_raster_us, (double)g_view);
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
    records_io(true);
    video_close();
    free_world();
    free_models();
    papp_free(g_hf);
    papp_free(g_hm);
    papp_sem_destroy(g_pres_go);
    papp_sem_destroy(g_pres_done);
    return 0;
}
#endif /* !CH_HOST */
