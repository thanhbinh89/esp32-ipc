#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Draw a rectangle outline directly onto an ESP32-P4 packed YUV420
 * (O_UYY_E_VYY) frame buffer — the layout the hardware H.264 encoder requires.
 * Each line is 1.5*width bytes, grouped as [C Y Y][C Y Y]..., with the chroma
 * byte being U on even lines and V on odd lines. Coordinates are in luma
 * (full-resolution) pixels.
 *
 * buf      : start of the O_UYY_E_VYY buffer
 * width    : luma width  (must be even)
 * height   : luma height (must be even)
 * y,u,v    : colour of the box in YUV
 * thickness: line thickness in luma pixels
 */
void osd_draw_rect_yuv420(uint8_t *buf, int width, int height,
                          int x1, int y1, int x2, int y2,
                          uint8_t y, uint8_t u, uint8_t v, int thickness);

#ifdef __cplusplus
}
#endif
