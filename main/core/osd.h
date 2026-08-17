#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "app_config.h"
#include "detector.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Rows a draw call wrote to, so the caller can publish just those instead of a
 * whole multi-megabyte frame.
 *
 * Corner marks dirty two thin bands per box — one at the top, one at the bottom —
 * with nothing in between, which is the entire reason they are drawn as corners
 * rather than closed rectangles: a closed rectangle's side edges dirty one row
 * each all the way down, forcing the caller to publish the box's full height to
 * announce a handful of bytes per row.
 *
 * Ranges are kept merged and sorted, so overlapping boxes cost no extra work.
 */
#define OSD_MAX_DIRTY_RANGES (2 * DET_MAX_BOX)

typedef struct {
    int first;
    int last;
} osd_rows_t;

typedef struct {
    int count;
    osd_rows_t range[OSD_MAX_DIRTY_RANGES];
} osd_dirty_t;

/*
 * Draw corner marks for each detection onto an ESP32-P4 packed YUV420
 * (O_UYY_E_VYY) frame — the layout the hardware H.264 encoder requires. Each
 * line is 1.5*width bytes, grouped as [C Y Y][C Y Y]..., with the chroma byte
 * being U on even lines and V on odd lines.
 *
 * Box coordinates are in the detector's input space and are scaled up by
 * scale_x/scale_y, then clipped to the frame. Colour, thickness and arm length
 * come from app_config.h.
 *
 * *dirty is reset first, so it comes back empty when there was nothing to draw.
 */
void osd_draw_boxes_yuv420(uint8_t *buf, int width, int height,
                           const det_box_t *boxes, int count,
                           int scale_x, int scale_y, osd_dirty_t *dirty);

#ifdef __cplusplus
}
#endif
