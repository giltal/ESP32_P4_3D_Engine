/*
 * e3d private: the engine's SIMD code (the P4's 128-bit integer unit).
 * Included by e3d_simd_pie.c and e3d_simd_espv.c, each compiled for one chip
 * generation (see e3d_simd.h), with E3D_SIMD_NAME set. The arithmetic of
 * simd_light is light_batch_c's in e3d_render.c.
 */

static const int16_t lb_one[8] __attribute__((aligned(16))) = {
    16384, 16384, 16384, 16384, 16384, 16384, 16384, 16384 };

/* PIE takes its integer operands only in x8-x15, hence the register variables. Each step
 * of 8 normals walks the 3 normal arrays and the 6 nl arrays by the stride
 * of one array. */
E3D_FAST static void simd_light(const int16_t *qn, int16_t *nl, int np, const int16_t *K, int shin)
{
    int16_t ds[16] __attribute__((aligned(16)));   /* kd, then the highlight base / power */
    register int sh __asm__("a1") = 14;
    register int st __asm__("a0") = np * (int)sizeof(int16_t);
    __asm__ volatile("esp.movx.w.sar %0" : : "r"(sh));
    for (int i = 0; i < np; i += 8) {
        register const int16_t *pk __asm__("a2") = K;
        register const int16_t *pn __asm__("a3") = qn + i;
        register int16_t *pl __asm__("a4") = nl + i;
        register int16_t *pd __asm__("a5") = ds;
        __asm__ volatile(
            "esp.vld.128.ip q7, %[k], 16\n\t"
            "esp.vld.128.ip q6, %[k], 0\n\t"
            "esp.vld.128.xp q0, %[n], %[st]\n\t"
            "esp.vld.128.xp q1, %[n], %[st]\n\t"
            "esp.vld.128.xp q2, %[n], %[st]\n\t"
            "esp.zero.qacc\n\t"
            "esp.vsmulas.s16.qacc q0, q7, 0\n\t"
            "esp.vsmulas.s16.qacc q1, q7, 1\n\t"
            "esp.vsmulas.s16.qacc q2, q7, 2\n\t"
            "esp.srcmb.s16.qacc q3, %[sh], 1\n\t"
            "esp.zero.qacc\n\t"
            "esp.vsmulas.s16.qacc q0, q7, 3\n\t"
            "esp.vsmulas.s16.qacc q1, q7, 4\n\t"
            "esp.vsmulas.s16.qacc q2, q7, 5\n\t"
            "esp.srcmb.s16.qacc q4, %[sh], 1\n\t"
            "esp.zero.qacc\n\t"
            "esp.vsmulas.s16.qacc q0, q7, 6\n\t"
            "esp.vsmulas.s16.qacc q1, q7, 7\n\t"
            "esp.vsmulas.s16.qacc q2, q6, 0\n\t"
            "esp.srcmb.s16.qacc q5, %[sh], 1\n\t"
            "esp.vld.128.xp q0, %[l], %[st]\n\t"
            "esp.vld.128.xp q1, %[l], %[st]\n\t"
            "esp.vld.128.xp q2, %[l], %[st]\n\t"
            "esp.zero.qacc\n\t"
            "esp.vmulas.s16.qacc q3, q0\n\t"
            "esp.vmulas.s16.qacc q4, q1\n\t"
            "esp.vmulas.s16.qacc q5, q2\n\t"
            "esp.srcmb.s16.qacc q0, %[sh], 1\n\t"
            "esp.vld.128.xp q1, %[l], %[st]\n\t"
            "esp.vld.128.xp q2, %[l], %[st]\n\t"
            "esp.zero.qacc\n\t"
            "esp.vmulas.s16.qacc q3, q1\n\t"
            "esp.vmulas.s16.qacc q4, q2\n\t"
            "esp.vld.128.xp q1, %[l], %[st]\n\t"
            "esp.vmulas.s16.qacc q5, q1\n\t"
            "esp.srcmb.s16.qacc q1, %[sh], 1\n\t"
            "esp.zero.q q2\n\t"
            "esp.vmax.s16 q0, q0, q2\n\t"
            "esp.vcmp.gt.s16 q3, q0, q2\n\t"
            "esp.vmax.s16 q1, q1, q2\n\t"
            "esp.andq q1, q1, q3\n\t"
            "esp.vst.128.ip q0, %[d], 16\n\t"
            "esp.vst.128.ip q1, %[d], 0"
            : [k] "+r"(pk), [n] "+r"(pn), [l] "+r"(pl), [d] "+r"(pd)
            : [st] "r"(st), [sh] "r"(sh)
            : "memory");
        register const int16_t *po __asm__("a3") = lb_one;
        __asm__ volatile(
            "esp.vld.128.ip q1, %[d], 0\n\t"
            "esp.vld.128.ip q2, %[o], 0\n\t"
            "mv t0, %[n]\n"
            "1:\n\t"
            "andi t1, t0, 1\n\t"
            "beqz t1, 2f\n\t"
            "esp.vmul.s16 q2, q2, q1\n"
            "2:\n\t"
            "srli t0, t0, 1\n\t"
            "beqz t0, 3f\n\t"
            "esp.vmul.s16 q1, q1, q1\n\t"
            "j 1b\n"
            "3:\n\t"
            "esp.vst.128.ip q2, %[d], 0"
            : [d] "+r"(pd), [o] "+r"(po)
            : [n] "r"(shin)
            : "t0", "t1", "memory");
        pk = K + 8;
        pd = ds;
        pl = nl + i;
        __asm__ volatile(
            "esp.vld.128.ip q6, %[k], 0\n\t"
            "esp.vld.128.ip q0, %[d], 16\n\t"
            "esp.vld.128.ip q1, %[d], 0\n\t"
            "esp.vld.128.ip q2, %[o], 0\n\t"
            "esp.zero.qacc\n\t"
            "esp.vsmulas.s16.qacc q0, q6, 1\n\t"
            "esp.vsmulas.s16.qacc q2, q6, 7\n\t"
            "esp.srcmb.s16.qacc q3, %[sh], 1\n\t"
            "esp.vst.128.xp q3, %[l], %[st]\n\t"
            "esp.zero.qacc\n\t"
            "esp.vsmulas.s16.qacc q0, q6, 2\n\t"
            "esp.vsmulas.s16.qacc q2, q6, 7\n\t"
            "esp.srcmb.s16.qacc q3, %[sh], 1\n\t"
            "esp.vst.128.xp q3, %[l], %[st]\n\t"
            "esp.zero.qacc\n\t"
            "esp.vsmulas.s16.qacc q0, q6, 3\n\t"
            "esp.vsmulas.s16.qacc q2, q6, 7\n\t"
            "esp.srcmb.s16.qacc q3, %[sh], 1\n\t"
            "esp.vst.128.xp q3, %[l], %[st]\n\t"
            "esp.zero.qacc\n\t"
            "esp.vsmulas.s16.qacc q1, q6, 4\n\t"
            "esp.srcmb.s16.qacc q3, %[sh], 1\n\t"
            "esp.vst.128.xp q3, %[l], %[st]\n\t"
            "esp.zero.qacc\n\t"
            "esp.vsmulas.s16.qacc q1, q6, 5\n\t"
            "esp.srcmb.s16.qacc q3, %[sh], 1\n\t"
            "esp.vst.128.xp q3, %[l], %[st]\n\t"
            "esp.zero.qacc\n\t"
            "esp.vsmulas.s16.qacc q1, q6, 6\n\t"
            "esp.srcmb.s16.qacc q3, %[sh], 1\n\t"
            "esp.vst.128.xp q3, %[l], %[st]"
            : [k] "+r"(pk), [d] "+r"(pd), [l] "+r"(pl), [o] "+r"(po)
            : [st] "r"(st), [sh] "r"(sh)
            : "memory");
    }
}

/* fill / copy: dst (and src) 16-byte aligned, n a multiple of 64 */
E3D_FAST static void simd_fill64(void *dst, uint32_t v, size_t n)
{
    uint32_t vv[4] __attribute__((aligned(16))) = { v, v, v, v };
    const uint32_t *pv = vv;
    __asm__ volatile("esp.vld.128.ip q0, %0, 0" : "+r"(pv) : "m"(vv) : "memory");
    for (size_t i = 0; i < n; i += 64)
        __asm__ volatile("esp.vst.128.ip q0, %0, 16\n\t"
                         "esp.vst.128.ip q0, %0, 16\n\t"
                         "esp.vst.128.ip q0, %0, 16\n\t"
                         "esp.vst.128.ip q0, %0, 16"
                         : "+r"(dst) : : "memory");
}

E3D_FAST static void simd_copy64(void *dst, const void *src, size_t n)
{
    for (size_t i = 0; i < n; i += 64)
        __asm__ volatile("esp.vld.128.ip q0, %0, 16\n\t"
                         "esp.vld.128.ip q1, %0, 16\n\t"
                         "esp.vld.128.ip q2, %0, 16\n\t"
                         "esp.vld.128.ip q3, %0, 16\n\t"
                         "esp.vst.128.ip q0, %1, 16\n\t"
                         "esp.vst.128.ip q1, %1, 16\n\t"
                         "esp.vst.128.ip q2, %1, 16\n\t"
                         "esp.vst.128.ip q3, %1, 16"
                         : "+r"(src), "+r"(dst) : : "memory");
}

static const e3d_simd_t simd_table = { E3D_SIMD_NAME, simd_light, simd_fill64, simd_copy64 };
