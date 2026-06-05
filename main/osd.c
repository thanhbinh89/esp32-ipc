#include <stddef.h>
#include "osd.h"

static inline void put_pixel_yuv420(uint8_t *buf, int width, int height,
                                    int x, int y, uint8_t yc, uint8_t uc, uint8_t vc)
{
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return;
    }

    /* ESP32-P4 "YUV420" is the packed O_UYY_E_VYY layout the H.264 encoder
     * requires: each line is 1.5*width bytes, grouped as [C Y Y][C Y Y]...,
     * where the leading chroma byte C is U on even lines and V on odd lines. */
    int stride = width + (width >> 1);
    uint8_t *line = buf + (size_t)y * stride;
    int group = x >> 1;

    line[group * 3 + 1 + (x & 1)] = yc;         /* luma */
    line[group * 3] = (y & 1) ? vc : uc;        /* U on even rows, V on odd rows */
}

void osd_draw_rect_yuv420(uint8_t *buf, int width, int height,
                          int x1, int y1, int x2, int y2,
                          uint8_t yc, uint8_t uc, uint8_t vc, int thickness)
{
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }

    for (int t = 0; t < thickness; ++t) {
        for (int x = x1; x <= x2; ++x) {
            put_pixel_yuv420(buf, width, height, x, y1 + t, yc, uc, vc);
            put_pixel_yuv420(buf, width, height, x, y2 - t, yc, uc, vc);
        }
        for (int y = y1; y <= y2; ++y) {
            put_pixel_yuv420(buf, width, height, x1 + t, y, yc, uc, vc);
            put_pixel_yuv420(buf, width, height, x2 - t, y, yc, uc, vc);
        }
    }
}
