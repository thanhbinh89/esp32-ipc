#include <stddef.h>

#include "osd.h"

/* ESP32-P4 "YUV420" is the packed O_UYY_E_VYY layout the H.264 encoder requires:
 * each line is 1.5*width bytes, grouped as [C Y Y][C Y Y]..., where the leading
 * chroma byte C is U on even lines and V on odd lines. So one 3-byte group holds
 * both luma samples of a horizontal pixel pair, which lets a horizontal run be
 * written two pixels at a time. */

/* Horizontal run over x1..x2 inclusive. Coordinates must already be clipped. */
static void draw_hrun(uint8_t *buf, int stride, int y, int x1, int x2,
                      uint8_t yc, uint8_t uc, uint8_t vc) {
    uint8_t *line = buf + (size_t)y * stride;
    const uint8_t c = (y & 1) ? vc : uc;
    int x = x1;

    if (x & 1) { /* odd leading pixel: second luma of its group */
        uint8_t *g = line + (x >> 1) * 3;
        g[0] = c;
        g[2] = yc;
        x++;
    }
    for (; x + 1 <= x2; x += 2) { /* whole groups */
        uint8_t *g = line + (x >> 1) * 3;
        g[0] = c;
        g[1] = yc;
        g[2] = yc;
    }
    if (x == x2) { /* even trailing pixel: first luma of its group */
        uint8_t *g = line + (x >> 1) * 3;
        g[0] = c;
        g[1] = yc;
    }
}

/* Vertical run over y1..y2 inclusive. Coordinates must already be clipped. */
static void draw_vrun(uint8_t *buf, int stride, int x, int y1, int y2,
                      uint8_t yc, uint8_t uc, uint8_t vc) {
    uint8_t *p = buf + (size_t)y1 * stride + (x >> 1) * 3;
    const int luma = 1 + (x & 1);

    for (int y = y1; y <= y2; ++y, p += stride) {
        p[luma] = yc;
        p[0] = (y & 1) ? vc : uc;
    }
}

/* Record rows [first, last], merging into any range it touches. The list stays
 * sorted, so a single left-to-right pass is enough. */
static void dirty_add(osd_dirty_t *d, int first, int last) {
    int i = 0;
    while (i < d->count && d->range[i].last + 1 < first) {
        i++;
    }

    /* Absorb every range the new one now overlaps or abuts. */
    int j = i;
    while (j < d->count && d->range[j].first <= last + 1) {
        if (d->range[j].first < first) {
            first = d->range[j].first;
        }
        if (d->range[j].last > last) {
            last = d->range[j].last;
        }
        j++;
    }

    if (j > i) { /* replace the absorbed run with the merged range */
        for (int k = i + 1, src = j; src < d->count; k++, src++) {
            d->range[k] = d->range[src];
        }
        d->count -= (j - i) - 1;
    } else {
        if (d->count >= OSD_MAX_DIRTY_RANGES) {
            /* Cannot happen with <= DET_MAX_BOX boxes, but never drop a range:
             * an unpublished row would show a stale box edge. Widen instead. */
            if (first < d->range[d->count - 1].first) {
                d->range[d->count - 1].first = first;
            }
            if (last > d->range[d->count - 1].last) {
                d->range[d->count - 1].last = last;
            }
            return;
        }
        for (int k = d->count; k > i; k--) {
            d->range[k] = d->range[k - 1];
        }
        d->count++;
    }

    d->range[i].first = first;
    d->range[i].last = last;
}

/* Four L-shaped marks, one per corner, clipped to the frame. */
static void draw_corners(uint8_t *buf, int width, int height,
                        int x1, int y1, int x2, int y2, osd_dirty_t *dirty) {
    if (x1 > x2) { int t = x1; x1 = x2; x2 = t; }
    if (y1 > y2) { int t = y1; y1 = y2; y2 = t; }

    const int cx1 = x1 < 0 ? 0 : x1;
    const int cy1 = y1 < 0 ? 0 : y1;
    const int cx2 = x2 > width - 1 ? width - 1 : x2;
    const int cy2 = y2 > height - 1 ? height - 1 : y2;
    if (cx1 > cx2 || cy1 > cy2) {
        return;
    }

    /* Never let the marks meet in the middle, or they become a full rectangle
     * again and the dirty bands merge into the whole box height. */
    int arm = OSD_CORNER_LEN;
    if (arm > (cx2 - cx1) / 2) {
        arm = (cx2 - cx1) / 2;
    }
    if (arm > (cy2 - cy1) / 2) {
        arm = (cy2 - cy1) / 2;
    }
    int thick = OSD_THICKNESS;
    if (thick > arm) {
        thick = arm;
    }
    if (arm < 1 || thick < 1) {
        return;
    }

    const int stride = width + (width >> 1);

    for (int t = 0; t < thick; ++t) {
        const int top = cy1 + t;
        const int bot = cy2 - t;
        draw_hrun(buf, stride, top, cx1, cx1 + arm, OSD_Y, OSD_U, OSD_V);
        draw_hrun(buf, stride, top, cx2 - arm, cx2, OSD_Y, OSD_U, OSD_V);
        draw_hrun(buf, stride, bot, cx1, cx1 + arm, OSD_Y, OSD_U, OSD_V);
        draw_hrun(buf, stride, bot, cx2 - arm, cx2, OSD_Y, OSD_U, OSD_V);

        const int lft = cx1 + t;
        const int rgt = cx2 - t;
        draw_vrun(buf, stride, lft, cy1, cy1 + arm, OSD_Y, OSD_U, OSD_V);
        draw_vrun(buf, stride, lft, cy2 - arm, cy2, OSD_Y, OSD_U, OSD_V);
        draw_vrun(buf, stride, rgt, cy1, cy1 + arm, OSD_Y, OSD_U, OSD_V);
        draw_vrun(buf, stride, rgt, cy2 - arm, cy2, OSD_Y, OSD_U, OSD_V);
    }

    /* Two bands, not the whole box: nothing between them was touched. */
    dirty_add(dirty, cy1, cy1 + arm);
    dirty_add(dirty, cy2 - arm, cy2);
}

void osd_draw_boxes_yuv420(uint8_t *buf, int width, int height,
                           const det_box_t *boxes, int count,
                           int scale_x, int scale_y, osd_dirty_t *dirty) {
    dirty->count = 0;

    for (int i = 0; i < count; i++) {
        draw_corners(buf, width, height,
                     boxes[i].x1 * scale_x, boxes[i].y1 * scale_y,
                     boxes[i].x2 * scale_x, boxes[i].y2 * scale_y,
                     dirty);
    }
}
