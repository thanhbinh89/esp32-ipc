#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Draw a rectangle outline directly onto a planar YUV420 (I420) frame buffer.
 * Coordinates are in luma (full-resolution) pixels. Chroma planes are updated
 * at the matching 2x2-subsampled positions so the box shows in colour.
 *
 * buf      : start of the I420 buffer (Y plane, then U plane, then V plane)
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
