#include <string.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>

#include "esp_cache.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_video_device.h"
#include "esp_video_ioctl.h"

#include "hal_video_capture.h"

static const char *TAG = "hal_vcap";

esp_err_t hal_vcap_open(hal_vcap_t *cap, uint32_t width, uint32_t height) {
    struct v4l2_format format;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;

    cap->fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    ESP_RETURN_ON_FALSE(cap->fd >= 0, ESP_FAIL, TAG, "open %s failed", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);

    /* Without this the driver waits forever, see VIDEO_DQBUF_TIMEOUT_MS. */
    struct timeval dqbuf_timeout = {
        .tv_sec = VIDEO_DQBUF_TIMEOUT_MS / 1000,
        .tv_usec = (VIDEO_DQBUF_TIMEOUT_MS % 1000) * 1000,
    };
    if (ioctl(cap->fd, VIDIOC_S_DQBUF_TIMEOUT, &dqbuf_timeout) != 0) {
        ESP_LOGW(TAG, "DQBUF timeout not settable; a camera stall will hang this task");
    }

    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = width;
    format.fmt.pix.height = height;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    ESP_RETURN_ON_FALSE(ioctl(cap->fd, VIDIOC_S_FMT, &format) == 0, ESP_FAIL, TAG, "S_FMT failed");

    cap->width = width;
    cap->height = height;
    cap->stride = width + (width >> 1); /* packed YUV420: 2 luma + 1 chroma per pair */

    memset(&req, 0, sizeof(req));
    req.count = CAP_BUF_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ESP_RETURN_ON_FALSE(ioctl(cap->fd, VIDIOC_REQBUFS, &req) == 0, ESP_FAIL, TAG, "REQBUFS failed");

    for (int i = 0; i < CAP_BUF_COUNT; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        ESP_RETURN_ON_FALSE(ioctl(cap->fd, VIDIOC_QUERYBUF, &buf) == 0, ESP_FAIL, TAG, "QUERYBUF %d failed", i);

        cap->mmap[i] = (uint8_t *)mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                       MAP_SHARED, cap->fd, buf.m.offset);
        ESP_RETURN_ON_FALSE(cap->mmap[i], ESP_ERR_NO_MEM, TAG, "mmap %d failed", i);
        cap->length[i] = buf.length;

        ESP_RETURN_ON_FALSE(ioctl(cap->fd, VIDIOC_QBUF, &buf) == 0, ESP_FAIL, TAG, "QBUF %d failed", i);
    }

    ESP_LOGI(TAG, "%s: %ux%u YUV420, %d buffers", ESP_VIDEO_MIPI_CSI_DEVICE_NAME,
             (unsigned)width, (unsigned)height, CAP_BUF_COUNT);
    return ESP_OK;
}

esp_err_t hal_vcap_start(hal_vcap_t *cap) {
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_RETURN_ON_FALSE(ioctl(cap->fd, VIDIOC_STREAMON, &type) == 0, ESP_FAIL, TAG, "STREAMON failed");
    return ESP_OK;
}

esp_err_t hal_vcap_acquire(hal_vcap_t *cap, hal_vcap_frame_t *frame) {
    memset(frame, 0, sizeof(*frame));
    frame->buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    frame->buf.memory = V4L2_MEMORY_MMAP;

    int64_t t0 = esp_timer_get_time();
    if (ioctl(cap->fd, VIDIOC_DQBUF, &frame->buf) != 0) {
        /* Loud on purpose: this is the point where the stream silently died before,
         * and the 1 Hz stats line cannot tell a stalled camera from an idle one. */
        ESP_LOGE(TAG, "no frame from the camera after %d ms",
                 (int)((esp_timer_get_time() - t0) / 1000));
        return ESP_ERR_TIMEOUT;
    }

    frame->data = cap->mmap[frame->buf.index];
    frame->bytesused = frame->buf.bytesused;
    return ESP_OK;
}

esp_err_t hal_vcap_release(hal_vcap_t *cap, const hal_vcap_frame_t *frame) {
    struct v4l2_buffer buf = frame->buf;
    ESP_RETURN_ON_FALSE(ioctl(cap->fd, VIDIOC_QBUF, &buf) == 0, ESP_FAIL, TAG, "QBUF failed");
    return ESP_OK;
}

esp_err_t hal_vcap_publish_rows(const hal_vcap_t *cap, const hal_vcap_frame_t *frame,
                                int y_first, int y_last) {
    if (y_first < 0) {
        y_first = 0;
    }
    if (y_last > (int)cap->height - 1) {
        y_last = (int)cap->height - 1;
    }
    if (y_first > y_last) {
        return ESP_OK;
    }

    return esp_cache_msync(frame->data + (size_t)y_first * cap->stride,
                           (size_t)(y_last - y_first + 1) * cap->stride,
                           ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
}
