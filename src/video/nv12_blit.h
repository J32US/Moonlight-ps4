// Copy NV12 1088→1080 (Y + UV with correct offsets).
// Fast path: if src is WB_GARLIC / Onion-alias, bulk memcpy <5 ms.
// Fallback: CPU memcpy from WC_Garlic (may be ~40 ms).
#pragma once

#include <stddef.h>
#include <stdint.h>
#include <sys/types.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Initialize blit helpers (idempotent). compute_queue typed void* optional. */
int nv12_blit_init(void);
void nv12_blit_shutdown(void);

/* Register decoder FB (phys+virt) to try Onion/WB alias. */
int nv12_blit_bind_decoder_fb(void *garlic_va, off_t phys, size_t size, int mem_type);

/*
 * Copy decoder layout → present.
 * expand_x4=1: each Y/UV sample → ×4 (compensates DCE stride bpp×4).
 * expand_x4=0: packed NV12; dst_pitch may be > width (per-row padding).
 * UV @ dst_pitch*buf_h from src + src_pitch*src_h.
 */
int nv12_blit_copy(uint8_t *dst, int dst_pitch, int buf_h,
                   const uint8_t *src_y, int src_pitch, int src_h, int width,
                   int expand_x4);

const char *nv12_blit_mode_name(void);

#ifdef __cplusplus
}
#endif
