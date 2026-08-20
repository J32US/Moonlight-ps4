/*
 * hdr10_convert.c — NV12 (8-bit) / P010 (10-bit) YCbCr → A2R10G10B10 BT.2020
 * (HDR10 present, 0x88740000).
 *
 * The host (Sunshine) encodes Main10 with HDR10 SEI (PQ transfer). The decoded
 * YCbCr is therefore ALREADY a PQ-encoded signal: passthrough mode just does
 * the BT.2020 limited-range YCbCr→RGB matrix and writes PQ-encoded RGB to the
 * A2R10G10B10_BT2020_PQ buffer. The TV applies the PQ EOTF.
 *
 * BT.2020 non-constant luminance, limited range (10-bit):
 *   Y'  = (Y - 64) / 876
 *   Cb' = (Cb - 512) / 896
 *   Cr' = (Cr - 512) / 896
 *   R = Y' + 1.4746*Cr'    G = Y' - 0.16455*Cb' - 0.57135*Cr'
 *   B = Y' + 1.88140*Cb'
 *
 * AVX1 (Jaguar): 8×32-bit float lanes. NO AVX2 → byte/word→float widening is
 * SSE4.1 (cvtepu8_epi32 / cvtepi32_ps = 4 lanes), then _mm256_set_m128
 * merges two 4-lane results into one 8-lane float vector for the matrix.
 * Matrix in AVX float = 8 px per iteration (vs 4 for SSE).
 */

#include "hdr10_convert.h"

#include <string.h>
#include <stdint.h>
#include <math.h>

#if defined(__x86_64__)
#include <immintrin.h>
#define HAVE_SIMD 1
#else
#define HAVE_SIMD 0
#endif

/* Optional SDR→HDR PQ upconversion LUT (peak_nits>0). 0 = passthrough. */
static uint16_t s_pq_lut[1024];
static int s_apply_pq;

/* ST 2084 */
#define PQ_M1 0.1593017578125
#define PQ_M2 78.84375
#define PQ_C1 0.8359375
#define PQ_C2 18.8515625
#define PQ_C3 18.6875

static double pq_signal(double lin01) {
    /* inverse EOTF: linear [0,1] → PQ signal [0,1] */
    double y = lin01 * 10000.0;
    double yn = y / 10000.0;
    double p = pow(yn, 1.0 / PQ_M2);
    double num = p - PQ_C1;
    if (num < 0.0) num = 0.0;
    double den = PQ_C2 - PQ_C3 * p;
    double sig = pow(num / den, 1.0 / PQ_M1);
    if (sig < 0.0) sig = 0.0;
    if (sig > 1.0) sig = 1.0;
    return sig;
}

int hdr10_convert_init(int peak_nits) {
    if (peak_nits > 0) {
        s_apply_pq = 1;
        for (int i = 0; i < 1024; i++) {
            double lin = (double)i / 1023.0;
            double sig = pq_signal(lin);
            uint32_t code = (uint32_t)(sig * 1023.0 + 0.5);
            if (code > 1023) code = 1023;
            s_pq_lut[i] = (uint16_t)code;
        }
    } else {
        s_apply_pq = 0;
    }
    return 0;
}

/* 3×10-bit → packed A2R10G10B10 (A=11). */
static inline uint32_t pack_rgb10(uint32_t r, uint32_t g, uint32_t b) {
    if (r > 1023) r = 1023;
    if (g > 1023) g = 1023;
    if (b > 1023) b = 1023;
    return 0xC0000000u | (r << 20) | (g << 10) | b;
}

static inline uint32_t yuv10_to_rgb10_pq(int y10, int cb10, int cr10) {
    double yf = ((double)y10 - 64.0) / 876.0;
    double uf = ((double)cb10 - 512.0) / 896.0;
    double vf = ((double)cr10 - 512.0) / 896.0;
    double r = yf + 1.4746 * vf;
    double g = yf - 0.16455 * uf - 0.57135 * vf;
    double b = yf + 1.88140 * uf;
    if (r < 0) r = 0; if (r > 1) r = 1;
    if (g < 0) g = 0; if (g > 1) g = 1;
    if (b < 0) b = 0; if (b > 1) b = 1;
    uint32_t ri = (uint32_t)(r * 1023.0 + 0.5);
    uint32_t gi = (uint32_t)(g * 1023.0 + 0.5);
    uint32_t bi = (uint32_t)(b * 1023.0 + 0.5);
    if (s_apply_pq) {
        ri = s_pq_lut[ri];
        gi = s_pq_lut[gi];
        bi = s_pq_lut[bi];
    }
    return pack_rgb10(ri, gi, bi);
}

/* Scalar reference (correctness anchor + tails). */
static void hdr10_row_scalar(uint32_t *drow, const uint8_t *yrow,
                             const uint8_t *uvrow, int x0, int x1,
                             int is_10bit) {
    for (int x = x0; x < x1; x++) {
        int yv, uu, vv;
        if (is_10bit) {
            yv = ((const uint16_t *)yrow)[x];
            uu = ((const uint16_t *)uvrow)[x & ~1];
            vv = ((const uint16_t *)uvrow)[(x & ~1) + 1];
        } else {
            yv = yrow[x] << 2;
            uu = uvrow[(x / 2) * 2] << 2;
            vv = uvrow[(x / 2) * 2 + 1] << 2;
        }
        drow[x] = yuv10_to_rgb10_pq(yv, uu, vv);
    }
}

/* Reference entry (used by the host-side AVX-vs-scalar validation). */
int hdr10_convert_frame_ref(uint8_t *dst, int dst_pitch_bytes,
                            const uint8_t *y, const uint8_t *uv,
                            int pitch_y, int pitch_uv,
                            int w, int h, int is_10bit) {
    return hdr10_convert_frame(dst, dst_pitch_bytes, y, uv, pitch_y, pitch_uv, w, h, is_10bit);
}

int hdr10_convert_frame(uint8_t *dst, int dst_pitch_bytes,
                        const uint8_t *y, const uint8_t *uv,
                        int pitch_y, int pitch_uv,
                        int w, int h, int is_10bit) {
    for (int row = 0; row < h; row++) {
        uint32_t *drow = (uint32_t *)(void *)(dst + (size_t)row * (size_t)dst_pitch_bytes);
        const uint8_t *yrow = y + (size_t)row * (size_t)pitch_y;
        const uint8_t *uvrow = uv + (size_t)(row / 2) * (size_t)pitch_uv;
        hdr10_row_scalar(drow, yrow, uvrow, 0, w, is_10bit);
    }
    return 0;
}

#if HAVE_SIMD
/* 8 px → packed A2R10G10B10 (+ optional PQ LUT). */
static inline void hdr10_store8(uint32_t *dp, const float rf[8], const float gf[8],
                                const float bf[8]) {
    for (int k = 0; k < 8; k++) {
        uint32_t ri = (uint32_t)(rf[k] + 0.5f);
        uint32_t gi = (uint32_t)(gf[k] + 0.5f);
        uint32_t bi = (uint32_t)(bf[k] + 0.5f);
        if (ri > 1023) ri = 1023;
        if (gi > 1023) gi = 1023;
        if (bi > 1023) bi = 1023;
        if (s_apply_pq) {
            ri = s_pq_lut[ri];
            gi = s_pq_lut[gi];
            bi = s_pq_lut[bi];
        }
        dp[k] = 0xC0000000u | (ri << 20) | (gi << 10) | bi;
    }
}

/* Convert 8 uint16 Y + 8 U + 8 V (already gathered) → floats via SSE widen. */
static inline void load8_u16_ps(const uint16_t *p, float out[8]) {
    __m128i w0 = _mm_loadu_si128((const __m128i *)(const void *)p);
    __m128i w1 = _mm_loadu_si128((const __m128i *)(const void *)(p + 8));
    __m128 f0 = _mm_cvtepi32_ps(_mm_unpacklo_epi16(w0, _mm_setzero_si128()));
    __m128 f1 = _mm_cvtepi32_ps(_mm_unpackhi_epi16(w0, _mm_setzero_si128()));
    __m128 f2 = _mm_cvtepi32_ps(_mm_unpacklo_epi16(w1, _mm_setzero_si128()));
    __m128 f3 = _mm_cvtepi32_ps(_mm_unpackhi_epi16(w1, _mm_setzero_si128()));
    _mm_storeu_ps(out, f0);
    _mm_storeu_ps(out + 4, f1);
    /* 8 values only: f2/f3 unused */
    (void)f2; (void)f3;
}

__attribute__((target("avx")))
static void hdr10_row_avx(uint32_t *drow, const uint8_t *yrow,
                          const uint8_t *uvrow, int w, int is_10bit) {
    const __m256 m_sy = _mm256_set1_ps(1.0f / 876.0f);
    const __m256 m_sc = _mm256_set1_ps(1.0f / 896.0f);
    const __m256 m_64 = _mm256_set1_ps(64.0f);
    const __m256 m_512 = _mm256_set1_ps(512.0f);
    const __m256 m_rv = _mm256_set1_ps(1.4746f);
    const __m256 m_gu = _mm256_set1_ps(0.16455f);
    const __m256 m_gv = _mm256_set1_ps(0.57135f);
    const __m256 m_bu = _mm256_set1_ps(1.88140f);
    const __m256 m_1023 = _mm256_set1_ps(1023.0f);
    const __m256 m_one = _mm256_set1_ps(1.0f);
    const __m256 m_zero = _mm256_setzero_ps();

    int x = 0;
    for (; x + 7 < w; x += 8) {
        float yf[8], uf[8], vf[8];
        if (is_10bit) {
            load8_u16_ps((const uint16_t *)(const void *)(yrow + x * 2), yf);
            for (int k = 0; k < 8; k++) {
                uf[k] = (float)((const uint16_t *)uvrow)[(x + k) & ~1];
                vf[k] = (float)((const uint16_t *)uvrow)[((x + k) & ~1) + 1];
            }
        } else {
            for (int k = 0; k < 8; k++) {
                yf[k] = (float)(yrow[x + k] << 2);
                int ux = ((x + k) / 2) * 2;
                uf[k] = (float)(uvrow[ux] << 2);
                vf[k] = (float)(uvrow[ux + 1] << 2);
            }
        }
        __m256 yv = _mm256_loadu_ps(yf);
        __m256 uvv = _mm256_loadu_ps(uf);
        __m256 vvv = _mm256_loadu_ps(vf);

        yv = _mm256_mul_ps(_mm256_sub_ps(yv, m_64), m_sy);
        uvv = _mm256_mul_ps(_mm256_sub_ps(uvv, m_512), m_sc);
        vvv = _mm256_mul_ps(_mm256_sub_ps(vvv, m_512), m_sc);

        __m256 r = _mm256_add_ps(yv, _mm256_mul_ps(vvv, m_rv));
        __m256 g = _mm256_sub_ps(_mm256_sub_ps(yv, _mm256_mul_ps(uvv, m_gu)),
                                 _mm256_mul_ps(vvv, m_gv));
        __m256 b = _mm256_add_ps(yv, _mm256_mul_ps(uvv, m_bu));

        r = _mm256_mul_ps(_mm256_min_ps(_mm256_max_ps(r, m_zero), m_one), m_1023);
        g = _mm256_mul_ps(_mm256_min_ps(_mm256_max_ps(g, m_zero), m_one), m_1023);
        b = _mm256_mul_ps(_mm256_min_ps(_mm256_max_ps(b, m_zero), m_one), m_1023);

        float rf[8], gf[8], bf[8];
        _mm256_storeu_ps(rf, r);
        _mm256_storeu_ps(gf, g);
        _mm256_storeu_ps(bf, b);
        hdr10_store8(drow + x, rf, gf, bf);
    }
    if (x < w)
        hdr10_row_scalar(drow, yrow, uvrow, x, w, is_10bit);
}
#endif

int hdr10_convert_frame_avx(uint8_t *dst, int dst_pitch_bytes,
                            const uint8_t *y, const uint8_t *uv,
                            int pitch_y, int pitch_uv,
                            int w, int h, int is_10bit) {
#if HAVE_SIMD
    for (int row = 0; row < h; row++) {
        uint32_t *drow = (uint32_t *)(void *)(dst + (size_t)row * (size_t)dst_pitch_bytes);
        const uint8_t *yrow = y + (size_t)row * (size_t)pitch_y;
        const uint8_t *uvrow = uv + (size_t)(row / 2) * (size_t)pitch_uv;
        hdr10_row_avx(drow, yrow, uvrow, w, is_10bit);
    }
    return 0;
#else
    return hdr10_convert_frame(dst, dst_pitch_bytes, y, uv, pitch_y, pitch_uv, w, h, is_10bit);
#endif
}

/* ---------------------------------------------------------------------------
 * SDR NV12 -> BGRA (A8R8G8B8, 0x80000000) AVX1 kernel.
 * FULL-RANGE BT.601 JPEG — matches the fork's SSE2 path EXACTLY
 * (coefs 179/44/92/227 >> 7 = 1.3984/0.34375/0.71875/1.7734, Y unscaled):
 *   R = Y + 1.3984*(V-128)   G = Y - 0.34375*(U-128) - 0.71875*(V-128)
 *   B = Y + 1.7734*(U-128)
 * Float math, 8 px/iter (AVX1 256-bit). Output packed BGRA (B,G,R,A).
 * ------------------------------------------------------------------------ */
#if HAVE_SIMD
__attribute__((target("avx")))
static void nv12_to_bgra_row_avx(uint8_t *drow, const uint8_t *yrow,
                                 const uint8_t *uvrow, int w) {
    const __m256 m_128 = _mm256_set1_ps(128.0f);
    const __m256 m_rv = _mm256_set1_ps(179.0f / 128.0f);   /* 1.3984375 */
    const __m256 m_gu = _mm256_set1_ps(44.0f / 128.0f);    /* 0.34375   */
    const __m256 m_gv = _mm256_set1_ps(92.0f / 128.0f);    /* 0.71875   */
    const __m256 m_bu = _mm256_set1_ps(227.0f / 128.0f);   /* 1.7734375 */
    const __m256 m_255 = _mm256_set1_ps(255.0f);
    const __m256 m_zero = _mm256_setzero_ps();

    int x = 0;
    for (; x + 7 < w; x += 8) {
        float yf[8], uf[8], vf[8];
        for (int k = 0; k < 8; k++) {
            yf[k] = (float)yrow[x + k];
            int ux = ((x + k) / 2) * 2;
            uf[k] = (float)uvrow[ux] - 128.0f;
            vf[k] = (float)uvrow[ux + 1] - 128.0f;
        }
        __m256 yv = _mm256_loadu_ps(yf);
        __m256 uvv = _mm256_loadu_ps(uf);
        __m256 vvv = _mm256_loadu_ps(vf);

        __m256 r = _mm256_add_ps(yv, _mm256_mul_ps(vvv, m_rv));
        __m256 g = _mm256_sub_ps(yv, _mm256_add_ps(_mm256_mul_ps(uvv, m_gu),
                                                   _mm256_mul_ps(vvv, m_gv)));
        __m256 b = _mm256_add_ps(yv, _mm256_mul_ps(uvv, m_bu));

        r = _mm256_min_ps(_mm256_max_ps(r, m_zero), m_255);
        g = _mm256_min_ps(_mm256_max_ps(g, m_zero), m_255);
        b = _mm256_min_ps(_mm256_max_ps(b, m_zero), m_255);

        float rf[8], gf[8], bf[8];
        _mm256_storeu_ps(rf, r);
        _mm256_storeu_ps(gf, g);
        _mm256_storeu_ps(bf, b);
        for (int k = 0; k < 8; k++) {
            uint32_t ri = (uint32_t)(rf[k] + 0.5f);
            uint32_t gi = (uint32_t)(gf[k] + 0.5f);
            uint32_t bi = (uint32_t)(bf[k] + 0.5f);
            if (ri > 255) ri = 255;
            if (gi > 255) gi = 255;
            if (bi > 255) bi = 255;
            /* BGRA: B,G,R,A */
            drow[(x + k) * 4 + 0] = (uint8_t)bi;
            drow[(x + k) * 4 + 1] = (uint8_t)gi;
            drow[(x + k) * 4 + 2] = (uint8_t)ri;
            drow[(x + k) * 4 + 3] = 0xFF;
        }
    }
    for (; x < w; x++) {
        int yy = yrow[x];
        int uu = (int)uvrow[(x / 2) * 2] - 128;
        int vv = (int)uvrow[(x / 2) * 2 + 1] - 128;
        int r = yy + ((179 * vv) >> 7);
        int g = yy - ((44 * uu + 92 * vv) >> 7);
        int b = yy + ((227 * uu) >> 7);
        if (r < 0) r = 0; if (r > 255) r = 255;
        if (g < 0) g = 0; if (g > 255) g = 255;
        if (b < 0) b = 0; if (b > 255) b = 255;
        drow[x * 4 + 0] = (uint8_t)b;
        drow[x * 4 + 1] = (uint8_t)g;
        drow[x * 4 + 2] = (uint8_t)r;
        drow[x * 4 + 3] = 0xFF;
    }
}

__attribute__((target("avx")))
int nv12_to_bgra_avx(uint8_t *dst, int dst_pitch,
                     const uint8_t *y, const uint8_t *uv,
                     int pitch_y, int pitch_uv,
                     int w, int h) {
    for (int row = 0; row < h; row++) {
        uint8_t *drow = dst + (size_t)row * (size_t)dst_pitch;
        const uint8_t *yrow = y + (size_t)row * (size_t)pitch_y;
        const uint8_t *uvrow = uv + (size_t)(row / 2) * (size_t)pitch_uv;
        nv12_to_bgra_row_avx(drow, yrow, uvrow, w);
    }
    return 1;
}
#endif

