/*
 * e3d private: the engine's SIMD code for chip revisions v3.x (PIE re-encoded as xespv).
 * Compile this file with -march=rv32imafc_zicsr_zifencei_xespv
 * (examples/papp/papp_common.py, e3d/CMakeLists.txt); without it the set is
 * NULL and the engine runs in plain C there. See e3d_simd.h.
 */
#include "e3d_simd.h"

#if defined(__riscv_xespv) && !defined(E3D_NO_SIMD)
#define E3D_SIMD_NAME "xespv, chip v3.x"
#include "e3d_simd_kernels.inc"
const e3d_simd_t *const e3d_simd_espv = &simd_table;
#else
const e3d_simd_t *const e3d_simd_espv = NULL;
#endif
