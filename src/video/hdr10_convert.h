#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Init the converter. peak_nits>0 enables SDR→HDR PQ upconversion LUT
 * (0 = passthrough: input YCbCr is already PQ-encoded from the host). */
int hdr10_convert_init(int peak_nits);

/* NV12/P010 YCbCr → A2R10G10B10_BT2020_PQ (0x88740000 layout: A2R10G10B10,
 * A in bits 31-30). is_10bit: input is 16-bit-per-sample P010. */
int hdr10_convert_frame(uint8_t *dst, int dst_pitch_bytes,
                        const uint8_t *y, const uint8_t *uv,
                        int pitch_y, int pitch_uv,
                        int w, int h, int is_10bit);

/* Scalar reference (validation / no-AVX fallback). */
int hdr10_convert_frame_ref(uint8_t *dst, int dst_pitch_bytes,
                            const uint8_t *y, const uint8_t *uv,
                            int pitch_y, int pitch_uv,
                            int w, int h, int is_10bit);

/* AVX1-accelerated variant (falls back to scalar when no AVX). */
int hdr10_convert_frame_avx(uint8_t *dst, int dst_pitch_bytes,
                            const uint8_t *y, const uint8_t *uv,
                            int pitch_y, int pitch_uv,
                            int w, int h, int is_10bit);

/* SDR NV12 -> BGRA (A8R8G8B8) AVX1 kernel. dst_pitch in BYTES.
 * Returns 1 if AVX ran, 0 if unavailable (caller falls back to SSE2). */
int nv12_to_bgra_avx(uint8_t *dst, int dst_pitch,
                     const uint8_t *y, const uint8_t *uv,
                     int pitch_y, int pitch_uv,
                     int w, int h);

#ifdef __cplusplus
}
#endif
