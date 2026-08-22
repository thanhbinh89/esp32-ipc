#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "esp_err.h"
#include "linux/videodev2.h"

#include "app_config.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Hardware H.264 encoder as a V4L2 mem2mem device. Raw YUV420 frames go in by
 * pointer (USERPTR, no copy), the bitstream comes back in mapped buffers.
 */
typedef struct {
    int fd;
    uint8_t *out_mmap[ENC_OUT_BUF_COUNT];
    size_t out_length;   /*!< bytes the driver mapped per bitstream buffer */
    bool no_force_idr;  /*!< latched once the driver rejects FORCE_KEY_FRAME */
} hal_h264_t;

/* One encoded frame. `buf` must be handed back verbatim to hal_h264_release(). */
typedef struct {
    struct v4l2_buffer buf;
    const uint8_t *data;
    size_t bytesused;
} hal_h264_bitstream_t;

typedef struct {
    uint32_t width;
    uint32_t height;
    int32_t i_period;
    int32_t bitrate;
    int32_t min_qp;
    int32_t max_qp;
} hal_h264_cfg_t;

/* Open the device, apply rate control, map ENC_OUT_BUF_COUNT bitstream buffers. */
esp_err_t hal_h264_open(hal_h264_t *enc, const hal_h264_cfg_t *cfg);

/* Begin streaming on both queues. Call once after hal_h264_open(). */
esp_err_t hal_h264_start(hal_h264_t *enc);

/*
 * Encode one frame. `yuv420` must stay valid until this returns; it is read by
 * DMA, so the caller is responsible for having written back any CPU edits.
 * Blocks until the bitstream for this frame is available.
 */
esp_err_t hal_h264_encode(hal_h264_t *enc, uint8_t *yuv420, size_t len,
                          hal_h264_bitstream_t *out);

/* Give a bitstream buffer back to the encoder. */
esp_err_t hal_h264_release(hal_h264_t *enc, const hal_h264_bitstream_t *out);

/* Make the next encoded frame an IDR, e.g. in response to an RTCP PLI. */
esp_err_t hal_h264_force_idr(hal_h264_t *enc);

#ifdef __cplusplus
}
#endif
