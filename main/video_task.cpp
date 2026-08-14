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

#include "app_define.h"
#include "app_video.h"
#include "video_task.h"
#include "pedestrian_detect_task.h"
#include "app_camera_pipeline.hpp"
#include "osd.h"
#include "task_webrtc.h"

static const char *TAG = "video";

typedef struct {
    struct v4l2_buffer buf;
} video_cap_item_t;

typedef struct {
    int cap_fd;
    int enc_fd;
    uint8_t *cap_mmap[CAP_BUF_COUNT];
    size_t cap_buffer_len[CAP_BUF_COUNT];
    uint8_t *enc_out_mmap;
    QueueHandle_t cap_queue;
    pipeline_handle_t feed_pipeline;
    ppa_client_handle_t ppa_handle;

    uint32_t stat_cap;
    uint32_t stat_cap_drop;
    uint32_t stat_enc;
    uint32_t stat_send_ok;
    uint32_t stat_send_fail;
    uint32_t stat_bytes;
} video_ctx_t;

static video_ctx_t s_ctx = {};

static esp_err_t h264_set_ctrl(int fd, uint32_t id, int32_t value, const char *what) {
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

static esp_err_t h264_force_idr(int fd) {
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

/**
* PPA-convert one full YUV420 frame (CAM_WIDTH*CAM_HEIGHT) to 
* a downscaled RGB565 buffer (PED_DETECT_WIDTH * PED_DETECT_HEIGHT).
* Blocking.
**/
static void convert_to_rgb_for_detector(ppa_client_handle_t ppa, pipeline_handle_t feed, uint8_t *yuv420) {
    if (!feed) {
        return;
    }

    // Get a free buffer element from the pipeline to hold the downscaled RGB565 frame
    camera_pipeline_buffer_element *buff_element = camera_pipeline_get_queued_element(feed);
    if (!buff_element) {
        return; /* detector still busy with the previous frame */
    }

    // Configure the PPA scale-rotate-mirror operation to downscale and convert the YUV420 frame to RGB565
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

    cfg.out.buffer = buff_element->buffer;
    cfg.out.buffer_size = PED_DETECT_WIDTH * PED_DETECT_HEIGHT * 2;
    cfg.out.pic_w = PED_DETECT_WIDTH;
    cfg.out.pic_h = PED_DETECT_HEIGHT;
    cfg.out.block_offset_x = 0;
    cfg.out.block_offset_y = 0;
    cfg.out.srm_cm = PPA_SRM_COLOR_MODE_RGB565;

    cfg.rotation_angle = PPA_SRM_ROTATION_ANGLE_0;
    cfg.scale_x = (float)PED_DETECT_WIDTH / CAM_WIDTH;
    cfg.scale_y = (float)PED_DETECT_HEIGHT / CAM_HEIGHT;
    cfg.byte_swap = false;
    cfg.mode = PPA_TRANS_MODE_BLOCKING;

    // Perform the PPA scale-rotate-mirror operation to downscale and convert the YUV420 frame to RGB565
    if (ppa_do_scale_rotate_mirror(ppa, &cfg) != ESP_OK) {
        // If the PPA operation fails, return the buffer element to the pipeline and skip this frame
        camera_pipeline_queue_element_index(feed, buff_element->index);
        return;
    }

    // Flush the CPU cache for the downscaled RGB565 buffer so that the detector sees the latest data
    esp_cache_msync(buff_element->buffer, cfg.out.buffer_size,
                    ESP_CACHE_MSYNC_FLAG_DIR_M2C);

    // Notify the pipeline that the buffer element is ready for detection
    camera_pipeline_done_element(feed, buff_element);
}

/** 
 * Entry point for the video capture task.
 * Captures YUV420 frames from the MIPI-CSI/ISP path and sends them to the video_encode_task via a FreeRTOS queue.
*/
static void video_capture_task(void *arg) {
    ESP_LOGI(TAG, "video_capture_task started");

    while (true) {
        video_cap_item_t item = {};
        item.buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        item.buf.memory = V4L2_MEMORY_MMAP;

        // Dequeue a captured frame from the MIPI-CSI capture device (YUV420 from ISP)
        ESP_ERROR_CHECK(ioctl(s_ctx.cap_fd, VIDIOC_DQBUF, &item.buf));
        s_ctx.stat_cap++;
        // Send the captured frame to the video_encode_task via the cap_queue.
        if (xQueueSend(s_ctx.cap_queue, &item, 0) != pdTRUE) {
            s_ctx.stat_cap_drop++;

            // ESP_ERROR_CHECK(ioctl(s_ctx.cap_fd, VIDIOC_QBUF, &item.buf));
        }
    }
}

/**
 * Entry point for the video encode task.
 * Receives captured YUV420 frames from the video_capture_task,
 * shares each frame with the pedestrian detector (PPA-converted to RGB565),
 * overlays the latest detection boxes (OSD) onto the YUV420 frame,
 * then H.264-encodes and sends the encoded frames over WebRTC.
 */
static void video_encode_task(void *arg) {
    ESP_LOGI(TAG, "video_capture_task started");

    while (true) {

        // Receive a captured YUV420 frame from the video_capture_task via the cap_queue.
        video_cap_item_t cap_item;
        if (xQueueReceive(s_ctx.cap_queue, &cap_item, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        // Get the pointer to the captured YUV420 frame from the MIPI-CSI capture device
        uint8_t *yuv_frame = s_ctx.cap_mmap[cap_item.buf.index];

        // Check if a keyframe (IDR) is requested by the WebRTC peer connection
        bool keyframe_requested = webrtc_take_keyframe_request();
        if (keyframe_requested) {
            // Force an IDR frame in the H.264 encoder to satisfy the keyframe request from the WebRTC peer connection
            if (h264_force_idr(s_ctx.enc_fd) == ESP_OK) {
                ESP_LOGI(TAG, "keyframe requested: force IDR ok");
            } else {
                ESP_LOGW(TAG, "keyframe requested: force IDR failed");
            }
        }

#if CONFIG_APP_ENABLE_AI
        /**
         * Convert the captured YUV420 frame to a downscaled RGB565 buffer for the pedestrian detector.
         * The downscaled RGB565 buffer is sent to the pedestrian detector via the feed_pipeline.
         * The pedestrian detector runs in a separate task and processes the downscaled RGB565 frames asynchronously.
         * The latest detection boxes are overlaid onto the YUV420 frame (OSD)
         */
        convert_to_rgb_for_detector(s_ctx.ppa_handle, s_ctx.feed_pipeline, yuv_frame);

        /**
         * Overlay the latest detection boxes onto the captured YUV420 frame (OSD).
         */
        pedestrian_detect_overlay_last_boxes(yuv_frame);
#endif

        // Flush the CPU cache for the captured YUV420 frame so that the H.264 encoder sees the latest data
        esp_cache_msync(yuv_frame, s_ctx.cap_buffer_len[cap_item.buf.index],
                        ESP_CACHE_MSYNC_FLAG_DIR_C2M | ESP_CACHE_MSYNC_FLAG_UNALIGNED);

        // Queue the captured YUV420 frame to the H.264 encoder (m2m) device for encoding
        struct v4l2_buffer enc_in_buf = {};
        enc_in_buf.index = 0;
        enc_in_buf.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
        enc_in_buf.memory = V4L2_MEMORY_USERPTR;
        enc_in_buf.m.userptr = (unsigned long)yuv_frame;
        enc_in_buf.length = cap_item.buf.bytesused;
        ESP_ERROR_CHECK(ioctl(s_ctx.enc_fd, VIDIOC_QBUF, &enc_in_buf));

        // Dequeue the encoded H.264 frame from the H.264 encoder (m2m) device
        struct v4l2_buffer enc_out_buf = {};
        enc_out_buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        enc_out_buf.memory = V4L2_MEMORY_MMAP;
        ESP_ERROR_CHECK(ioctl(s_ctx.enc_fd, VIDIOC_DQBUF, &enc_out_buf));
        s_ctx.stat_enc++;
        s_ctx.stat_bytes += enc_out_buf.bytesused;
        uint32_t bytesused = enc_out_buf.bytesused;

        // Send the encoded H.264 frame over WebRTC if the peer connection is established and in the completed state
        if (g_pc && eState == PEER_CONNECTION_COMPLETED && enc_out_buf.bytesused > 0) {
            if (xSemaphoreTake(g_pc_lock, pdMS_TO_TICKS(2)) == pdTRUE) {
                ESP_LOGD(TAG, "Send video frame: %u bytes", (unsigned)enc_out_buf.bytesused);
                if (peer_connection_send_video(g_pc, s_ctx.enc_out_mmap, bytesused) < 0) {
                    s_ctx.stat_send_fail++;
                } else {
                    s_ctx.stat_send_ok++;
                }
                xSemaphoreGive(g_pc_lock);
            } else {
                s_ctx.stat_send_fail++;
            }
        }

        // Re-queue the captured YUV420 frame back to the MIPI-CSI capture device for reuse
        ESP_ERROR_CHECK(ioctl(s_ctx.cap_fd, VIDIOC_QBUF, &cap_item.buf));
        // Queue the encoded H.264 frame back to the H.264 encoder (m2m) device for reuse
        ESP_ERROR_CHECK(ioctl(s_ctx.enc_fd, VIDIOC_DQBUF, &enc_in_buf));
        // Re-queue the encoded H.264 frame back to the H.264 encoder (m2m) device for reuse
        ESP_ERROR_CHECK(ioctl(s_ctx.enc_fd, VIDIOC_QBUF, &enc_out_buf));
    }
}

static void capture_fd_init() {
    struct v4l2_format format;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;

    /* Open MIPI-CSI capture device (ISP outputs YUV420 from sensor RAW) */
    s_ctx.cap_fd = open(ESP_VIDEO_MIPI_CSI_DEVICE_NAME, O_RDONLY);
    assert(s_ctx.cap_fd >= 0);

    // Set the capture format to YUV420 (full resolution)
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = CAM_WIDTH;
    format.fmt.pix.height = CAM_HEIGHT;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    ESP_ERROR_CHECK(ioctl(s_ctx.cap_fd, VIDIOC_S_FMT, &format));

    // Request CAP_BUF_COUNT buffers for memory-mapped capture
    memset(&req, 0, sizeof(req));
    req.count = CAP_BUF_COUNT;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ESP_ERROR_CHECK(ioctl(s_ctx.cap_fd, VIDIOC_REQBUFS, &req));

    // Query and memory-map each of the requested buffers for capture
    for (int i = 0; i < CAP_BUF_COUNT; i++) {
        memset(&buf, 0, sizeof(buf));
        buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
        buf.memory = V4L2_MEMORY_MMAP;
        buf.index = i;
        ESP_ERROR_CHECK(ioctl(s_ctx.cap_fd, VIDIOC_QUERYBUF, &buf));
        // Memory-map the buffer to user space so that we can access the captured YUV420 frames directly
        s_ctx.cap_mmap[i] = (uint8_t *)mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                            MAP_SHARED, s_ctx.cap_fd, buf.m.offset);
        assert(s_ctx.cap_mmap[i]);
        s_ctx.cap_buffer_len[i] = buf.length;
        // Queue the buffer back to the capture device so that it can be filled with captured frames
        ESP_ERROR_CHECK(ioctl(s_ctx.cap_fd, VIDIOC_QBUF, &buf));
    }
}

static void encode_fd_init() {
    struct v4l2_format format;
    struct v4l2_requestbuffers req;
    struct v4l2_buffer buf;

    /* Open H.264 hardware encoder (m2m) device */
    s_ctx.enc_fd = open(ESP_VIDEO_H264_DEVICE_NAME, O_RDONLY);
    assert(s_ctx.enc_fd >= 0);

    h264_set_ctrl(s_ctx.enc_fd, V4L2_CID_MPEG_VIDEO_H264_I_PERIOD, H264_I_PERIOD, "H.264 I period");
    h264_set_ctrl(s_ctx.enc_fd, V4L2_CID_MPEG_VIDEO_BITRATE, H264_BITRATE, "H.264 bitrate");
    h264_set_ctrl(s_ctx.enc_fd, V4L2_CID_MPEG_VIDEO_H264_MIN_QP, H264_MIN_QP, "H.264 min QP");
    h264_set_ctrl(s_ctx.enc_fd, V4L2_CID_MPEG_VIDEO_H264_MAX_QP, H264_MAX_QP, "H.264 max QP");

    /* encode input stream: feed YUV420 frames */
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    format.fmt.pix.width = CAM_WIDTH;
    format.fmt.pix.height = CAM_HEIGHT;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_YUV420;
    ESP_ERROR_CHECK(ioctl(s_ctx.enc_fd, VIDIOC_S_FMT, &format));

    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    req.memory = V4L2_MEMORY_USERPTR;
    ESP_ERROR_CHECK(ioctl(s_ctx.enc_fd, VIDIOC_REQBUFS, &req));

    /* encode output stream: receive H.264 bitstream */
    memset(&format, 0, sizeof(format));
    format.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    format.fmt.pix.width = CAM_WIDTH;
    format.fmt.pix.height = CAM_HEIGHT;
    format.fmt.pix.pixelformat = V4L2_PIX_FMT_H264;
    ESP_ERROR_CHECK(ioctl(s_ctx.enc_fd, VIDIOC_S_FMT, &format));

    memset(&req, 0, sizeof(req));
    req.count = 1;
    req.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    req.memory = V4L2_MEMORY_MMAP;
    ESP_ERROR_CHECK(ioctl(s_ctx.enc_fd, VIDIOC_REQBUFS, &req));
}

void video_task(void *arg) {
    ESP_LOGI(TAG, "video_task started");

    s_ctx.cap_fd = -1;
    s_ctx.enc_fd = -1;
    s_ctx.feed_pipeline = (pipeline_handle_t)arg;

    ppa_client_config_t ppa_cfg = {.oper_type = PPA_OPERATION_SRM,};
    ESP_ERROR_CHECK(ppa_register_client(&ppa_cfg, &s_ctx.ppa_handle));

    ESP_ERROR_CHECK(app_video_init());

    ESP_LOGI(TAG, "capture fd init: capture=%s", ESP_VIDEO_MIPI_CSI_DEVICE_NAME);
    // Initialize the MIPI-CSI capture device (ISP outputs YUV420 from sensor RAW) and memory-map the capture buffers
    capture_fd_init();
    ESP_LOGI(TAG, "encode fd init: capture=%s", ESP_VIDEO_H264_DEVICE_NAME);
    // Initialize the H.264 encoder (m2m) device and memory-map the output buffer for encoded H.264 bitstream
    encode_fd_init();

    // Query the output buffer from the H.264 encoder (m2m) device to get the memory-mapped address for the encoded H.264 bitstream
    struct v4l2_buffer buf;
    memset(&buf, 0, sizeof(buf));
    buf.type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    buf.memory = V4L2_MEMORY_MMAP;
    buf.index = 0;
    ESP_ERROR_CHECK(ioctl(s_ctx.enc_fd, VIDIOC_QUERYBUF, &buf));

    // Memory-map the output buffer from the H.264 encoder (m2m) device to user space so that we can access the encoded H.264 bitstream directly
    s_ctx.enc_out_mmap = (uint8_t *)mmap(NULL, buf.length, PROT_READ | PROT_WRITE,
                                         MAP_SHARED, s_ctx.enc_fd, buf.m.offset);
    assert(s_ctx.enc_out_mmap);
    // Queue the output buffer back to the H.264 encoder (m2m) device so that it can be filled with encoded H.264 bitstream
    ESP_ERROR_CHECK(ioctl(s_ctx.enc_fd, VIDIOC_QBUF, &buf));

    ESP_LOGI(TAG, "Starting capture and encode fd");
    // Start streaming on the capture and encode devices
    int type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_ERROR_CHECK(ioctl(s_ctx.enc_fd, VIDIOC_STREAMON, &type));
    type = V4L2_BUF_TYPE_VIDEO_OUTPUT;
    ESP_ERROR_CHECK(ioctl(s_ctx.enc_fd, VIDIOC_STREAMON, &type));
    type = V4L2_BUF_TYPE_VIDEO_CAPTURE;
    ESP_ERROR_CHECK(ioctl(s_ctx.cap_fd, VIDIOC_STREAMON, &type));

    // Create a FreeRTOS queue to pass captured frames from the video_capture_task to the video_encode_task
    s_ctx.cap_queue = xQueueCreate(VIDEO_QUEUE_LEN, sizeof(video_cap_item_t));
    assert(s_ctx.cap_queue);

    ESP_LOGI(TAG, "Starting video_capture_task");
    // Start the video_capture_task and video_encode_task on separate FreeRTOS tasks pinned to different CPU cores
    xTaskCreatePinnedToCore(video_capture_task, "video_cap", TASK_CAPTURE_STACK_SIZE, NULL, 5, NULL, 0);

    ESP_LOGI(TAG, "Starting video_encode_task");
    // Start the video_encode_task on a separate FreeRTOS task pinned to a different CPU core
    xTaskCreatePinnedToCore(video_encode_task, "video_enc", TASK_ENCODE_STACK_SIZE, NULL, 5, NULL, 0);

    int64_t stat_last_us = esp_timer_get_time();

    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));

        int64_t now_us = esp_timer_get_time();
        uint32_t elapsed_ms = (uint32_t)((now_us - stat_last_us) / 1000);
        ESP_LOGI(TAG,
                 "%ums: cap=%u cap_drop=%u enc=%u send_ok=%u send_fail=%u bitrate=%ukbps q_cap=%u",
                 elapsed_ms, s_ctx.stat_cap, s_ctx.stat_cap_drop, s_ctx.stat_enc,
                 s_ctx.stat_send_ok, s_ctx.stat_send_fail,
                 (unsigned)((s_ctx.stat_bytes * 8ULL) / elapsed_ms),
                 (unsigned)uxQueueMessagesWaiting(s_ctx.cap_queue));

        s_ctx.stat_cap = 0;
        s_ctx.stat_cap_drop = 0;
        s_ctx.stat_enc = 0;
        s_ctx.stat_send_ok = 0;
        s_ctx.stat_send_fail = 0;
        s_ctx.stat_bytes = 0;
        stat_last_us = now_us;
    }
}
