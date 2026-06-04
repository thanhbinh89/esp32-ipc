#include "osd.h"

static inline void put_pixel_yuv420(uint8_t *buf, int width, int height,
                                    int x, int y, uint8_t yc, uint8_t uc, uint8_t vc)
{
    if (x < 0 || y < 0 || x >= width || y >= height) {
        return;
    }

    uint8_t *y_plane = buf;
    uint8_t *u_plane = buf + width * height;
    uint8_t *v_plane = u_plane + (width / 2) * (height / 2);

    y_plane[y * width + x] = yc;

    int cx = x >> 1;
    int cy = y >> 1;
    int c_idx = cy * (width / 2) + cx;
    u_plane[c_idx] = uc;
    v_plane[c_idx] = vc;
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
