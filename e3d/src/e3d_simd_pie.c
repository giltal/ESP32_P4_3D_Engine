/*
 * e3d private: the engine's SIMD code for chip revisions v1.x (PIE).
 * Compile this file with -march=rv32imafc_zicsr_zifencei_xesppie
 * (examples/papp/papp_common.py, e3d/CMakeLists.txt); without it the set is
 * NULL and the engine runs in plain C there. See e3d_simd.h.
 */
#include "e3d_simd.h"

#if defined(__riscv_xesppie) && !defined(E3D_NO_SIMD)
#define E3D_SIMD_NAME "PIE, chip v1.x"
#include "e3d_simd_kernels.inc"
const e3d_simd_t *const e3d_simd_pie = &simd_table;
#else
const e3d_simd_t *const e3d_simd_pie = NULL;
#endif
