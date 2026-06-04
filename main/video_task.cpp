#include <stdio.h>
#include <string.h>
#include <inttypes.h>
#include <fcntl.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_cache.h"
#include "esp_timer.h"
#include "driver/ppa.h"
#include "linux/videodev2.h"
#include "esp_video_device.h"

#include "app_video.h"
#include "video_task.h"
#include "pedestrian_detect_task.h"
#include "app_camera_pipeline.hpp"
#include "osd.h"
#include "webrtc_task.h"

static const char *TAG = "video";

#define CAM_WIDTH       1920
#define CAM_HEIGHT      1080
#define CAP_BUF_COUNT   3
#define VIDEO_QUEUE_LEN 2

#define H264_I_PERIOD   30
#define H264_BITRATE    2000000
#define H264_MIN_QP     30
#define H264_MAX_QP     40

/* OSD box colour in YUV (red). */
#define OSD_Y           76
#define OSD_U           84
#define OSD_V           255
#define OSD_THICKNESS   16

/* Detection coords are in PED_DETECT_* space; scale up to full frame for OSD. */
#define OSD_SCALE_X     (CAM_WIDTH / PED_DETECT_WIDTH)
#define OSD_SCALE_Y     (CAM_HEIGHT / PED_DETECT_HEIGHT)

typedef struct {
    struct v4l2_buffer buf;
} video_cap_item_t;

typedef struct {
    int cap_fd;
    int m2m_fd;
    uint8_t *cap_buffer[CAP_BUF_COUNT];
    size_t cap_buffer_len[CAP_BUF_COUNT];
    uint8_t *m2m_cap_buffer;
    QueueHandle_t cap_queue;
    pipeline_handle_t feed_pipeline;
    ppa_client_handle_t ppa_handle;

    uint32_t stat_cap;
    uint32_t stat_cap_drop;
    uint32_t stat_enc;
    uint32_t stat_enc_drop;
    uint32_t stat_send_ok;
    uint32_t stat_send_fail;
    uint32_t stat_key_req;
    uint32_t stat_idr_ok;
    uint32_t stat_idr_fail;
    uint32_t stat_bytes;
    uint64_t stat_cap_dq_us;
    uint64_t stat_enc_q_us;
    uint64_t stat_enc_dq_us;
    uint64_t stat_send_us;
    uint32_t stat_cap_dq_max_us;
    uint32_t stat_enc_q_max_us;
    uint32_t stat_enc_dq_max_us;
    uint32_t stat_send_max_us;
} video_ctx_t;

static esp_err_t h264_set_ctrl(int fd, uint32_t id, int32_t value, const char *what)
{
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

static esp_err_t h264_force_idr(int fd)
{
#ifdef V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME
    return h264_set_ctrl(fd, V4L2_CID_MPEG_VIDEO_FORCE_KEY_FRAME, 1, "H.264 force key frame");
#else
    static bool warned = false;
    if (!warned) {
        ESP_LOGW(TAG, "H.264 force key frame control not available in this IDF V4L2 header");
        warned = true;
    }
    return ESP_ERR_NOT_SUPPORTED;
#endif
}

/* PPA-convert one full YUV420 frame to a downscaled RGB565 buffer (big-endian to
 * match the detector's preprocessor). Blocking. */
static void feed_detector(ppa_client_handle_t ppa, pipeline_handle_t feed,
                          const uint8_t *yuv420)
{
    if (!feed) {
        return;
    }

    camera_pipeline_buffer_element *el = camera_pipeline_get_queued_element(feed);
    if (!el) {
        return; /* detector still busy with the previous frame */
    }

    ppa_srm_oper_config_t cfg = {};
    cfg.in.buffer = yuv420;
    cfg.in.pic_w = CAM_WIDTH;
    cfg.in.pic_h = CAM_HEIGHT;
    cfg.in.block_w = CAM_WIDTH;
    cfg.in.block_h = CAM_HEIGHT;
    cfg.in.block_offset_x = 0;
    cfg.in.block_offset_y = 0;
    cfg.in.srm_cm = PPA_SRM_COLOR_MODE_YUV420;
    cfg.in.yuv_range = PPA_COLOR_RANGE_LIMIT;
    cfg.in.yuv_std = PPA_COLOR_CONV_STD_RGB_YUV_BT601;

    cfg.out.buffer = el->buffer;
    cfg.out.buffer_size = PED_DETECT_WIDTH * PED_DETECT_HEIGHT * 2;
    cfg.out.pic_w = PED_DETECT_WIDTH;
    cfg.out.pic_h = PED_DETECT_HEIGHT;
    cfg.out.block_offset_x = 0;
    cfg.out.block_offset_y = 0;
    cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

    cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    cfg.scale_x = (float)PED_DETECT_WIDTH / CAM_WIDTH;
    cfg.scale_y = (float)PED_DETECT_HEIGHT / CAM_HEIGHT;
    cfg.byte_swap = false; /* produce big-endian RGB565 for the model preprocessor */
    cfg.mode = PPA_TRANS_MODE_BLOCKING;

    if (ppa_do_scale_rotate_mirror(ppa, &cfg) != ESP_OK) {
        camera_pipeline_queue_element_index(feed, el->index);
        return;
    }

    esp_cache_msync(el->buffer, cfg.out.buffer_size,
                    ESP_CACHE_MSYNC_FLAG_DIR_M2C);

    camera_pipeline_done_element(feed, el);
}

static void overlay_boxes(uint8_t *yuv420)
{
    ped_box_t boxes[PED_DETECT_MAX_BOX];
    int n = pedestrian_detect_get_boxes(boxes, PED_DETECT_MAX_BOX);

    for (int i = 0; i < n; i++) {
        osd_draw_rect_yuv420(yuv420, CAM_WIDTH, CAM_HEIGHT,
                             boxes[i].x1 * OSD_SCALE_X, boxes[i].y1 * OSD_SCALE_Y,
                             boxes[i].x2 * OSD_SCALE_X, boxes[i].y2 * OSD_SCALE_Y,
                             OSD_Y, OSD_U, OSD_V, OSD_THICKNESS);
    }
}

static void video_capture_task(void *arg)
{
    video_ctx_t *ctx = (video_ctx_t *)arg;

    while (true) {
        video_cap_item_t item = {};
        item.buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        item.buf.memory = V4L2_MEMORY_MMAP;

        int64_t t0_us = esp_timer_get_time();
        ESP_ERROR_CHECK(ioctl(ctx->cap_fd, VIDIOC_DQBUF, &item.buf));
        uint32_t dt_us = (uint32_t)(esp_timer_get_time() - t0_us);
        ctx->stat_cap_dq_us += dt_us;
        if (dt_us > ctx->stat_cap_dq_max_us) {
            ctx->stat_cap_dq_max_us = dt_us;
        }
        ctx->stat_cap++;

        if (xQueueSend(ctx->cap_queue, &item, 0) != pdTRUE) {
            ctx->stat_cap_drop++;
            ESP_ERROR_CHECK(ioctl(ctx->cap_fd, VIDIOC_QBUF, &item.buf));
        }
    }
}

static void video_encode_task(void *arg)
{
    video_ctx_t *ctx = (video_ctx_t *)arg;

    while (true) {
        video_cap_item_t cap_item;
        if (xQueueReceive(ctx->cap_queue, &cap_item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        uint8_t *frame = ctx->cap_buffer[cap_item.buf.index];
        bool keyframe_requested = webrtc_take_keyframe_request();

        if (keyframe_requested) {
            ctx->stat_key_req++;
            if (h264_force_idr(ctx->m2m_fd) == ESP_OK) {
                ctx->stat_idr_ok++;
                ESP_LOGI(TAG, "keyframe requested: force IDR ok");
            } else {
                ctx->stat_idr_fail++;
                ESP_LOGW(TAG, "keyframe requested: force IDR failed");
            }
        }
#if 0
        /* Share the post-ISP YUV420 frame with the detector (PPA -> RGB565). Done
         * before OSD so the detector sees the unannotated frame. */
        feed_detector(ctx->ppa_handle, ctx->feed_pipeline, frame);

        /* Overlay the latest detection boxes onto the YUV420 frame, then flush CPU
         * writes so the encoder DMA reads the annotated pixels. */
        overlay_boxes(frame);
        esp_cache_msync(frame, ctx->cap_buffer_len[cap_item.buf.index],
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);
#endif
        struct v4l2_buffer m2m_out_buf = {};
        m2m_out_buf.index = 0;
        m2m_out_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        m2m_out_buf.memory = V4L2_MEMORY_USERPTR;
        m2m_out_buf.m.userptr = (unsigned long)frame;
        m2m_out_buf.length = cap_item.buf.bytesused;

        int64_t t0_us = esp_timer_get_time();
        ESP_ERROR_CHECK(ioctl(ctx->m2m_fd, VIDIOC_QBUF, &m2m_out_buf));
        uint32_t dt_us = (uint32_t)(esp_timer_get_time() - t0_us);
        ctx->stat_enc_q_us += dt_us;
        if (dt_us > ctx->stat_enc_q_max_us) {
            ctx->stat_enc_q_max_us = dt_us;
        }

        struct v4l2_buffer m2m_cap_buf = {};
        m2m_cap_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        m2m_cap_buf.memory = V4L2_MEMORY_MMAP;

        t0_us = esp_timer_get_time();
        ESP_ERROR_CHECK(ioctl(ctx->m2m_fd, VIDIOC_DQBUF, &m2m_cap_buf));
        dt_us = (uint32_t)(esp_timer_get_time() - t0_us);
        ctx->stat_enc_dq_us += dt_us;
        if (dt_us > ctx->stat_enc_dq_max_us) {
            ctx->stat_enc_dq_max_us = dt_us;
        }

        ctx->stat_enc++;
        ctx->stat_bytes += m2m_cap_buf.bytesused;

        if (g_pc && eState == PEER_CONNECTION_COMPLETED && m2m_cap_buf.bytesused > 0) {
            t0_us = esp_timer_get_time();
            if (xSemaphoreTake(g_pc_lock, pdMS_TO_TICKS(2)) == pdTRUE) {
                ESP_LOGD(TAG, "send video frame: %u bytes", (unsigned)m2m_cap_buf.bytesused);
                if (peer_connection_send_video(g_pc, ctx->m2m_cap_buffer, m2m_cap_buf.bytesused) < 0) {
                    ctx->stat_send_fail++;
                } else {
                    ctx->stat_send_ok++;
                }
                xSemaphoreGive(g_pc_lock);
            } else {
                ctx->stat_send_fail++;
            }
            dt_us = (uint32_t)(esp_timer_get_time() - t0_us);
            ctx->stat_send_us += dt_us;
            if (dt_us > ctx->stat_send_max_us) {
                ctx->stat_send_max_us = dt_us;
            }
        }

        ESP_ERROR_CHECK(ioctl(ctx->m2m_fd, VIDIOC_DQBUF, &m2m_out_buf));
        ESP_ERROR_CHECK(ioctl(ctx->cap_fd, VIDIOC_QBUF, &cap_item.buf));
        ESP_ERROR_CHECK(ioctl(ctx->m2m_fd, VIDIOC_QBUF, &m2m_cap_buf));
    }
}


void video_task(void *arg)
{
    static video_ctx_t ctx = {};
    int type;
    struct v4l2_format format;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;

    ctx.cap_fd = -1;
    ctx.m2m_fd = -1;
    ctx.feed_pipeline = (pipeline_handle_t)arg;

    ppa_client_config_t ppa_cfg = {
        .oper_type = PPA_OPERATION_SRM,
    };
    ESP_ERROR_CHECK(ppa_register_client(&ppa_cfg, &ctx.ppa_handle));

    ESP_ERROR_CHECK(app_video_init());

    /* Open MIPI-CSI capture device (ISP outputs YUV420 from sensor RAW) */
    ctx.cap_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    assert(ctx.cap_fd >= 0);

    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = CAM_WIDTH;
    format.fmt.pix.height = CAM_HEIGHT;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    ESP_ERROR_CHECK(ioctl(ctx.cap_fd, VIDIOC_S_FMT, &format));

    memset(&req, 0, sizeof(req));
    req.count = CAP_BUF_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ESP_ERROR_CHECK(ioctl(ctx.cap_fd, VIDIOC_REQBUFS, &req));

    for (int i = 0; i < CAP_BUF_COUNT; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        ESP_ERROR_CHECK(ioctl(ctx.cap_fd, VIDIOC_QUERYBUF, &buf));

        ctx.cap_buffer[i] = (uint8_t *)mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                            MAP_SHARED, ctx.cap_fd, buf.m.offset);
        assert(ctx.cap_buffer[i]);
        ctx.cap_buffer_len[i] = buf.length;

        ESP_ERROR_CHECK(ioctl(ctx.cap_fd, VIDIOC_QBUF, &buf));
    }

    /* Open H.264 hardware encoder (m2m) device */
    ctx.m2m_fd = open(ESP_VIDEO_H264_DEVICE_NAME, O_RDONLY);
    assert(ctx.m2m_fd >= 0);

    h264_set_ctrl(ctx.m2m_fd, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, H264_I_PERIOD, "H.264 I period");
    h264_set_ctrl(ctx.m2m_fd, V4L2_CID_MPEG_VIDEO_BITRATE, H264_BITRATE, "H.264 bitrate");
    h264_set_ctrl(ctx.m2m_fd, V4L2_CID_MPEG_VIDEO_H264_MIN_QP, H264_MIN_QP, "H.264 min QP");
    h264_set_ctrl(ctx.m2m_fd, V4L2_CID_MPEG_VIDEO_H264_MAX_QP, H264_MAX_QP, "H.264 max QP");

    /* m2m output stream: feed YUV420 frames */
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    format.fmt.pix.width = CAM_WIDTH;
    format.fmt.pix.height = CAM_HEIGHT;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    ESP_ERROR_CHECK(ioctl(ctx.m2m_fd, VIDIOC_S_FMT, &format));

    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    req.memory = V4L2_MEMORY_USERPTR;
    ESP_ERROR_CHECK(ioctl(ctx.m2m_fd, VIDIOC_REQBUFS, &req));

    /* m2m capture stream: receive H.264 bitstream */
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = CAM_WIDTH;
    format.fmt.pix.height = CAM_HEIGHT;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
    ESP_ERROR_CHECK(ioctl(ctx.m2m_fd, VIDIOC_S_FMT, &format));

    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ESP_ERROR_CHECK(ioctl(ctx.m2m_fd, VIDIOC_REQBUFS, &req));

    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    ESP_ERROR_CHECK(ioctl(ctx.m2m_fd, VIDIOC_QUERYBUF, &buf));

    // output encoder
    ctx.m2m_cap_buffer = (uint8_t *)mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                         MAP_SHARED, ctx.m2m_fd, buf.m.offset);
    assert(ctx.m2m_cap_buffer);

    ESP_ERROR_CHECK(ioctl(ctx.m2m_fd, VIDIOC_QBUF, &buf));

    ctx.cap_queue = xQueueCreate(VIDEO_QUEUE_LEN, sizeof(video_cap_item_t));
    assert(ctx.cap_queue);

    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_ERROR_CHECK(ioctl(ctx.m2m_fd, VIDIOC_STREAMON, &type));
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ESP_ERROR_CHECK(ioctl(ctx.m2m_fd, VIDIOC_STREAMON, &type));
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_ERROR_CHECK(ioctl(ctx.cap_fd, VIDIOC_STREAMON, &type));

    ESP_LOGI(TAG, "H.264 stream started: %dx%d (~%dfps, detect off)",
             CAM_WIDTH, CAM_HEIGHT, H264_I_PERIOD);

    xTaskCreatePinnedToCore(video_capture_task, "video_cap", 4096, &ctx, 5, NULL, 0);
    xTaskCreatePinnedToCore(video_encode_task, "video_enc", 4096, &ctx, 5, NULL, 0);

    int64_t stat_last_us = esp_timer_get_time();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        int64_t now_us = esp_timer_get_time();
        uint32_t elapsed_ms = (uint32_t)((now_us - stat_last_us) / 1000);
        uint32_t stat_send_total = ctx.stat_send_ok + ctx.stat_send_fail;
        ESP_LOGI(TAG,
                 "stats %ums: cap=%u cap_drop=%u enc=%u enc_drop=%u send_ok=%u send_fail=%u bitrate=%ukbps key_req=%u idr_ok=%u idr_fail=%u q_cap=%u "
                 "avg_ms cap_dq=%u enc_q=%u enc_dq=%u send=%u max_ms cap_dq=%u enc_q=%u enc_dq=%u send=%u",
                 elapsed_ms, ctx.stat_cap, ctx.stat_cap_drop, ctx.stat_enc, ctx.stat_enc_drop,
                 ctx.stat_send_ok, ctx.stat_send_fail,
                 (unsigned)((ctx.stat_bytes * 8ULL) / elapsed_ms), ctx.stat_key_req,
                 ctx.stat_idr_ok, ctx.stat_idr_fail,
                 (unsigned)uxQueueMessagesWaiting(ctx.cap_queue),
                 ctx.stat_cap ? (unsigned)(ctx.stat_cap_dq_us / ctx.stat_cap / 1000) : 0,
                 ctx.stat_enc ? (unsigned)(ctx.stat_enc_q_us / ctx.stat_enc / 1000) : 0,
                 ctx.stat_enc ? (unsigned)(ctx.stat_enc_dq_us / ctx.stat_enc / 1000) : 0,
                 stat_send_total ? (unsigned)(ctx.stat_send_us / stat_send_total / 1000) : 0,
                 ctx.stat_cap_dq_max_us / 1000, ctx.stat_enc_q_max_us / 1000,
                 ctx.stat_enc_dq_max_us / 1000, ctx.stat_send_max_us / 1000);

        ctx.stat_cap = 0;
        ctx.stat_cap_drop = 0;
        ctx.stat_enc = 0;
        ctx.stat_enc_drop = 0;
        ctx.stat_send_ok = 0;
        ctx.stat_send_fail = 0;
        ctx.stat_key_req = 0;
        ctx.stat_idr_ok = 0;
        ctx.stat_idr_fail = 0;
        ctx.stat_bytes = 0;
        ctx.stat_cap_dq_us = 0;
        ctx.stat_enc_q_us = 0;
        ctx.stat_enc_dq_us = 0;
        ctx.stat_send_us = 0;
        ctx.stat_cap_dq_max_us = 0;
        ctx.stat_enc_q_max_us = 0;
        ctx.stat_enc_dq_max_us = 0;
        ctx.stat_send_max_us = 0;
        stat_last_us = now_us;
    }
}
