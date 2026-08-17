#pragma once

#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "linux/videodev2.h"

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * MIPI-CSI capture device (the ISP delivers packed YUV420 from sensor RAW).
 * State is owned by the caller — nothing here is global.
 */
typedef struct {
    int fd;
    uint32_t width;
    uint32_t height;
    uint32_t stride;   /*!< bytes per row: 1.5 * width for packed YUV420 */
    uint8_t *mmap[CAP_BUF_COUNT];
    size_t length[CAP_BUF_COUNT];
} hal_vcap_t;

/* One captured frame. `buf` must be handed back verbatim to hal_vcap_release(). */
typedef struct {
    struct v4l2_buffer buf;
    uint8_t *data;
    size_t bytesused;
} hal_vcap_frame_t;

/* Open the device, set the format, map CAP_BUF_COUNT buffers and queue them all. */
esp_err_t hal_vcap_open(hal_vcap_t *cap, uint32_t width, uint32_t height);

/* Begin streaming. Call once after hal_vcap_open(). */
esp_err_t hal_vcap_start(hal_vcap_t *cap);

/* Block until a frame is ready and take ownership of it. */
esp_err_t hal_vcap_acquire(hal_vcap_t *cap, hal_vcap_frame_t *frame);

/* Give a frame back to the driver so it can be filled again. */
esp_err_t hal_vcap_release(hal_vcap_t *cap, const hal_vcap_frame_t *frame);

/*
 * Publish CPU edits to rows [y_first, y_last] of a held frame, so the DMA
 * consumers that read it next — the H.264 encoder, the PPA — see them.
 *
 * The frame arrives and leaves by DMA, so a frame the CPU never wrote needs no
 * call at all; writing back all ~3 MB every time costs far more than the edits.
 * Cache maintenance lives here rather than in the drawing code because this
 * module owns the buffer and knows its geometry.
 */
esp_err_t hal_vcap_publish_rows(const hal_vcap_t *cap, const hal_vcap_frame_t *frame,
                                int y_first, int y_last);

#ifdef __cplusplus
}
#endif
