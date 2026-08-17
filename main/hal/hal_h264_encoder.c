#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "esp_check.h"
#include "esp_log.h"
#include "esp_video_device.h"

#include "hal_h264_encoder.h"

static const char *TAG = "hal_h264";

static esp_err_t set_ctrl(int fd, uint32_t id, int32_t value, const char *what) {
    struct v4l2_ext_control control[1] = {};
    control[0].id = id;
    control[0].value = value;

    struct v4l2_ext_controls controls = {};
    controls.ctrl_class = V4L2_CID_CODEC_CLASS;
    controls.count = 1;
    controls.controls = control;

    if (ioctl(fd, VIDIOC_S_EXT_CTRLS, &controls) != 0) {
        ESP_LOGW(TAG, "failed to set %s", what);
        return ESP_FAIL;
    }
    return ESP_OK;
}

esp_err_t hal_h264_open(hal_h264_t *enc, const hal_h264_cfg_t *cfg) {
    struct v4l2_format format;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;

    enc->fd = open(ESP_VIDEO_H264_DEVICE_NAME, O_RDONLY);
    ESP_RETURN_ON_FALSE(enc->fd >= 0, ESP_FAIL, TAG, "open %s failed", ESP_VIDEO_H264_DEVICE_NAME);

    set_ctrl(enc->fd, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, cfg->i_period, "I period");
    set_ctrl(enc->fd, V4L2_CID_MPEG_VIDEO_BITRATE, cfg->bitrate, "bitrate");
    set_ctrl(enc->fd, V4L2_CID_MPEG_VIDEO_H264_MIN_QP, cfg->min_qp, "min QP");
    set_ctrl(enc->fd, V4L2_CID_MPEG_VIDEO_H264_MAX_QP, cfg->max_qp, "max QP");

    /* Input queue: raw YUV420 frames, passed by pointer so nothing is copied. */
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    format.fmt.pix.width = cfg->width;
    format.fmt.pix.height = cfg->height;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    ESP_RETURN_ON_FALSE(ioctl(enc->fd, VIDIOC_S_FMT, &format) == 0, ESP_FAIL, TAG, "S_FMT output failed");

    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    req.memory = V4L2_MEMORY_USERPTR;
    ESP_RETURN_ON_FALSE(ioctl(enc->fd, VIDIOC_REQBUFS, &req) == 0, ESP_FAIL, TAG, "REQBUFS output failed");

    /* Output queue: the H.264 bitstream. */
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = cfg->width;
    format.fmt.pix.height = cfg->height;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
    ESP_RETURN_ON_FALSE(ioctl(enc->fd, VIDIOC_S_FMT, &format) == 0, ESP_FAIL, TAG, "S_FMT capture failed");

    memset(&req, 0, sizeof(req));
    req.count = ENC_OUT_BUF_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ESP_RETURN_ON_FALSE(ioctl(enc->fd, VIDIOC_REQBUFS, &req) == 0, ESP_FAIL, TAG, "REQBUFS capture failed");

    for (int i = 0; i < ENC_OUT_BUF_COUNT; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        ESP_RETURN_ON_FALSE(ioctl(enc->fd, VIDIOC_QUERYBUF, &buf) == 0, ESP_FAIL, TAG, "QUERYBUF %d failed", i);

        enc->out_mmap[i] = (uint8_t *)mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                           MAP_SHARED, enc->fd, buf.m.offset);
        ESP_RETURN_ON_FALSE(enc->out_mmap[i], ESP_ERR_NO_MEM, TAG, "mmap %d failed", i);

        ESP_RETURN_ON_FALSE(ioctl(enc->fd, VIDIOC_QBUF, &buf) == 0, ESP_FAIL, TAG, "QBUF %d failed", i);
    }

    ESP_LOGI(TAG, "%s: %ux%u, I period %d, %d bps, QP %d-%d, %d bitstream buffers",
             ESP_VIDEO_H264_DEVICE_NAME, (unsigned)cfg->width, (unsigned)cfg->height,
             (int)cfg->i_period, (int)cfg->bitrate, (int)cfg->min_qp, (int)cfg->max_qp,
             ENC_OUT_BUF_COUNT);
    return ESP_OK;
}

esp_err_t hal_h264_start(hal_h264_t *enc) {
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(enc->fd, VIDIOC_STREAMON, &type) == 0, ESP_FAIL, TAG, "STREAMON capture failed");
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ESP_RETURN_ON_FALSE(ioctl(enc->fd, VIDIOC_STREAMON, &type) == 0, ESP_FAIL, TAG, "STREAMON output failed");
    return ESP_OK;
}

esp_err_t hal_h264_encode(hal_h264_t *enc, uint8_t *yuv420, size_t len,
                          hal_h264_bitstream_t *out) {
    struct v4l2_buffer in_buf = {};
    in_buf.index = 0;
    in_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    in_buf.memory = V4L2_MEMORY_USERPTR;
    in_buf.m.userptr = (unsigned long)yuv420;
    in_buf.length = len;
    ESP_RETURN_ON_FALSE(ioctl(enc->fd, VIDIOC_QBUF, &in_buf) == 0, ESP_FAIL, TAG, "QBUF input failed");

    memset(out, 0, sizeof(*out));
    out->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    out->buf.memory = V4L2_MEMORY_MMAP;
    ESP_RETURN_ON_FALSE(ioctl(enc->fd, VIDIOC_DQBUF, &out->buf) == 0, ESP_FAIL, TAG, "DQBUF bitstream failed");

    out->data = enc->out_mmap[out->buf.index];
    out->bytesused = out->buf.bytesused;

    /* Reclaim the input slot now that the encoder is done reading the frame. */
    ESP_RETURN_ON_FALSE(ioctl(enc->fd, VIDIOC_DQBUF, &in_buf) == 0, ESP_FAIL, TAG, "DQBUF input failed");
    return ESP_OK;
}

esp_err_t hal_h264_release(hal_h264_t *enc, const hal_h264_bitstream_t *out) {
    struct v4l2_buffer buf = out->buf;
    ESP_RETURN_ON_FALSE(ioctl(enc->fd, VIDIOC_QBUF, &buf) == 0, ESP_FAIL, TAG, "QBUF bitstream failed");
    return ESP_OK;
}

/*
 * On-demand IDR is not reachable through this driver.
 *
 * esp_video's H.264 device implements exactly four ext-controls — I_PERIOD,
 * BITRATE, MIN_QP and MAX_QP — and rejects everything else, so
 * V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME fails with ESP_ERR_NOT_SUPPORTED. Nor can
 * it be emulated by briefly setting I_PERIOD to 1: the driver only reads `gop`
 * when it builds the encoder at STREAMON, so a later write updates a field
 * nothing reads again.
 *
 * That leaves the periodic IDR as the only recovery path, which is why
 * H264_I_PERIOD has to stay short enough for a joining browser to sync quickly.
 * The attempt is made once so the limitation is visible in the log, then latched
 * off — a browser sends a PLI burst, and three driver error lines per PLI would
 * bury everything else.
 */
esp_err_t hal_h264_force_idr(hal_h264_t *enc) {
    if (enc->no_force_idr) {
        return ESP_ERR_NOT_SUPPORTED;
    }

    esp_err_t ret = set_ctrl(enc->fd, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1, "force key frame");
    if (ret != ESP_OK) {
        enc->no_force_idr = true;
        ESP_LOGW(TAG, "driver has no force-key-frame control; "
                      "relying on the periodic IDR every %d frames", H264_I_PERIOD);
    }
    return ret;
}
