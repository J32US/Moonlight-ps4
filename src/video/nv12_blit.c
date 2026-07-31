// Blit NV12. hrep4: expand Y/UV ×4 (workaround stride DCE bpp×4).
#include "nv12_blit.h"
#include "../log.h"
#include "../orbis/video_out_c.h"

#include <string.h>
#include <stdint.h>
#include <sys/types.h>

static int s_inited;
static int s_mode; /* 0=cpu_wc 1=wb_garlic */
static void *s_garlic_va;
static size_t s_fb_size;
static int s_fb_mem_type = ML_DMEM_TYPE_GARLIC;

int nv12_blit_init(void) {
    s_inited = 1;
    if (s_fb_mem_type == ML_DMEM_TYPE_WB_GARLIC || s_fb_mem_type == ML_DMEM_TYPE_ONION)
        s_mode = 1;
    LOGI("nv12_blit: init mode=%s", nv12_blit_mode_name());
    return 0;
}

void nv12_blit_shutdown(void) {
    s_garlic_va = NULL;
    s_fb_size = 0;
    s_mode = 0;
    s_fb_mem_type = ML_DMEM_TYPE_GARLIC;
    s_inited = 0;
}

int nv12_blit_bind_decoder_fb(void *garlic_va, off_t phys, size_t size, int mem_type) {
    (void)phys;
    s_garlic_va = garlic_va;
    s_fb_size = size;
    s_fb_mem_type = mem_type;

    if (mem_type == ML_DMEM_TYPE_WB_GARLIC || mem_type == ML_DMEM_TYPE_ONION) {
        s_mode = 1;
        LOGI("nv12_blit: decoder FB %s (cacheable CPU read)",
             mem_type == ML_DMEM_TYPE_ONION ? "ONION" : "WB_GARLIC");
        return 0;
    }
    s_mode = 0;
    LOGI("nv12_blit: decoder FB WC_GARLIC (memcpy CPU)");
    return 0;
}

const char *nv12_blit_mode_name(void) {
    return s_mode == 1 ? "cacheable" : "cpu_wc";
}

static void expand_row_x4_to(uint8_t *dst, const uint8_t *src, int n) {
    uint32_t *d = (uint32_t *)(void *)dst;
    int i = 0;
    for (; i + 3 < n; i += 4) {
        d[i + 0] = (uint32_t)src[i + 0] * 0x01010101u;
        d[i + 1] = (uint32_t)src[i + 1] * 0x01010101u;
        d[i + 2] = (uint32_t)src[i + 2] * 0x01010101u;
        d[i + 3] = (uint32_t)src[i + 3] * 0x01010101u;
    }
    for (; i < n; i++)
        d[i] = (uint32_t)src[i] * 0x01010101u;
}

static void expand_uv_x4_to(uint8_t *dst, const uint8_t *src, int n) {
    uint32_t *d = (uint32_t *)(void *)dst;
    int pairs = n / 2;
    for (int i = 0; i < pairs; i++) {
        uint32_t u = src[i * 2];
        uint32_t v = src[i * 2 + 1];
        uint32_t p = u | (v << 8) | (u << 16) | (v << 24);
        d[i * 2] = p;
        d[i * 2 + 1] = p;
    }
}

int nv12_blit_copy(uint8_t *dst, int dst_pitch, int buf_h,
                   const uint8_t *src_y, int src_pitch, int src_h, int width,
                   int expand_x4) {
    if (!dst || !src_y || dst_pitch <= 0 || src_pitch <= 0 || buf_h <= 0 || src_h <= 0)
        return -1;

    int rows = src_h < buf_h ? src_h : buf_h;
    int w = width > 0 ? width : (src_pitch < dst_pitch ? src_pitch : dst_pitch);
    if (w > src_pitch)
        w = src_pitch;
    if (w > dst_pitch)
        w = dst_pitch;

    int hrep4 = expand_x4 && width > 0 && dst_pitch >= width * 4;
    const uint8_t *src_uv = src_y + (size_t)src_pitch * (size_t)src_h;
    uint8_t *dst_uv = dst + (size_t)dst_pitch * (size_t)buf_h;
    int uv_rows = rows / 2;

    if (hrep4) {
        int copy_w = w;
        if (copy_w * 4 > dst_pitch)
            copy_w = dst_pitch / 4;
        uint8_t line[7680] __attribute__((aligned(64)));
        if ((size_t)dst_pitch > sizeof(line))
            return -1;
        for (int r = 0; r < rows; r++) {
            expand_row_x4_to(line, src_y + (size_t)r * (size_t)src_pitch, copy_w);
            memcpy(dst + (size_t)r * (size_t)dst_pitch, line, (size_t)copy_w * 4u);
        }
        for (int r = 0; r < uv_rows; r++) {
            expand_uv_x4_to(line, src_uv + (size_t)r * (size_t)src_pitch, copy_w);
            memcpy(dst_uv + (size_t)r * (size_t)dst_pitch, line, (size_t)copy_w * 4u);
        }
        return 0;
    }

    size_t row_bytes = (size_t)w;
    if (src_pitch == dst_pitch && (int)row_bytes == src_pitch) {
        memcpy(dst, src_y, (size_t)src_pitch * (size_t)rows);
        memcpy(dst_uv, src_uv, (size_t)src_pitch * (size_t)uv_rows);
    } else {
        for (int r = 0; r < rows; r++) {
            memcpy(dst + (size_t)r * (size_t)dst_pitch,
                   src_y + (size_t)r * (size_t)src_pitch, row_bytes);
        }
        for (int r = 0; r < uv_rows; r++) {
            memcpy(dst_uv + (size_t)r * (size_t)dst_pitch,
                   src_uv + (size_t)r * (size_t)src_pitch, row_bytes);
        }
    }
    return 0;
}
