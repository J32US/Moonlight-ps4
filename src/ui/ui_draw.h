// Software drawing onto a BGRA framebuffer (uint32 0xAARRGGBB little-endian).
#pragma once

#include <stdint.h>

typedef struct {
    uint8_t *fb;
    int pitch; /* bytes per row */
    int w, h;  /* pixels */
} ui_surface_t;

void ui_clear(ui_surface_t *s, uint32_t argb);
void ui_rect(ui_surface_t *s, int x, int y, int w, int h, uint32_t argb);
/* ASCII text 32..126 with scaled 8x8 font. */
void ui_text(ui_surface_t *s, int x, int y, int scale, uint32_t argb, const char *str);
int ui_text_w(int scale, const char *str);
