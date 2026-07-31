#include "ui_draw.h"
#include "ui_font.h"

#include <string.h>

void ui_clear(ui_surface_t *s, uint32_t argb) {
    for (int y = 0; y < s->h; y++) {
        uint32_t *row = (uint32_t *)(void *)(s->fb + (size_t)y * (size_t)s->pitch);
        for (int x = 0; x < s->w; x++)
            row[x] = argb;
    }
}

void ui_rect(ui_surface_t *s, int x, int y, int w, int h, uint32_t argb) {
    if (x < 0) { w += x; x = 0; }
    if (y < 0) { h += y; y = 0; }
    if (x + w > s->w) w = s->w - x;
    if (y + h > s->h) h = s->h - y;
    if (w <= 0 || h <= 0)
        return;
    for (int r = 0; r < h; r++) {
        uint32_t *row = (uint32_t *)(void *)(s->fb + (size_t)(y + r) * (size_t)s->pitch);
        for (int c = 0; c < w; c++)
            row[x + c] = argb;
    }
}

static void draw_glyph(ui_surface_t *s, int x, int y, int scale, uint32_t argb,
                       const unsigned char *glyph) {
    for (int gy = 0; gy < UI_GLYPH_H; gy++) {
        unsigned bits = glyph[gy];
        if (!bits)
            continue;
        for (int gx = 0; gx < UI_GLYPH_W; gx++) {
            if (!((bits >> gx) & 1u))
                continue;
            ui_rect(s, x + gx * scale, y + gy * scale, scale, scale, argb);
        }
    }
}

void ui_text(ui_surface_t *s, int x, int y, int scale, uint32_t argb, const char *str) {
    if (scale < 1)
        scale = 1;
    int cx = x;
    for (const char *p = str; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c < 32 || c > 126)
            c = '?';
        draw_glyph(s, cx, y, scale, argb, ui_font8x8[c - 32]);
        cx += UI_GLYPH_W * scale;
    }
}

int ui_text_w(int scale, const char *str) {
    if (scale < 1)
        scale = 1;
    return (int)strlen(str) * UI_GLYPH_W * scale;
}
