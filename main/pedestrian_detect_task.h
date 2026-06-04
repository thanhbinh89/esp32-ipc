#pragma once

#include <stdint.h>

/* RGB565 resolution the camera task downscales each frame to before detection.
 * 16:9, an exact 1/3 of 1920x1080 so OSD coordinates scale back by x3. */
#define PED_DETECT_WIDTH    640
#define PED_DETECT_HEIGHT   360
#define PED_DETECT_MAX_BOX  10

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    int x1;
    int y1;
    int x2;
    int y2;
    float score;
} ped_box_t;

/* Create the detector + detection task. Returns the feed pipeline handle the
 * camera task pushes RGB565 (PED_DETECT_WIDTH x PED_DETECT_HEIGHT) frames into,
 * or NULL on failure. */
esp_err_t pedestrian_detect_task_start(void *arg);

/* Copy the most recent detection boxes (detection-resolution coords) into out.
 * Returns the number of boxes copied (<= max). Thread-safe. */
int pedestrian_detect_get_boxes(ped_box_t *out, int max);

#ifdef __cplusplus
}
#endif
