/*
 * e3d private: the engine's SIMD code and the choice of it.
 *
 * The P4's 128-bit integer SIMD comes in two encodings: PIE on chip revisions
 * v1.x (-march ..._xesppie) and _xespv on v3.x. The instructions are the
 * same, the encodings are not, so code built for one generation faults on
 * the other. The SIMD functions (e3d_simd_kernels.inc) are therefore built
 * twice, once per generation (e3d_simd_pie.c, e3d_simd_espv.c, each with its
 * -march), and the first e3d_create picks the set for the chip it runs on.
 * The rest of the engine is plain RV32, so one binary runs on both.
 *
 * Plain C instead: a file built without its -march (its set is NULL), a chip
 * revision the port can't tell (a PAPP on a launcher older than 0.33), a
 * kernel that fails its check against the C lighting, or -DE3D_NO_SIMD.
 */
#pragma once

#include <stddef.h>
#include <stdint.h>

#include "e3d_port.h"

typedef struct {
    const char *name;
    /* lighting of np normals, 8 at a time (np a multiple of 8): see light_batch_c */
    void (*light)(const int16_t *qn, int16_t *nl, int np, const int16_t *K, int shin);
    /* dst (and src) 16-byte aligned, n a multiple of 64 */
    void (*fill64)(void *dst, uint32_t v, size_t n);
    void (*copy64)(void *dst, const void *src, size_t n);
} e3d_simd_t;

extern const e3d_simd_t *const e3d_simd_pie;    /* chip v1.x; NULL when not built for it */
extern const e3d_simd_t *const e3d_simd_espv;   /* chip v3.x; NULL when not built for it */

static inline const e3d_simd_t *e3d_simd_pick(void)
{
#if defined(E3D_NO_SIMD)
    return NULL;
#else
    int rev = e3d_port_chip_rev();          /* major * 100 + minor, -1 = unknown */
    if (rev < 0)
        return NULL;
    return rev >= 300 ? e3d_simd_espv : e3d_simd_pie;
#endif
}
