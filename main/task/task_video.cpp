#include <string.h>
#include <inttypes.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/queue.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "app_config.h"
#include "hal_video_init.h"
#include "detector.h"
#include "hal_h264_encoder.h"
#include "hal_ppa.h"
#include "hal_video_capture.h"
#include "osd.h"
#include "task_video.h"
#include "webrtc_api.h"

static const char *TAG = "video";

/* Detections come back in DET_* space; scale up to full frame for the overlay.
 * Exact integers only because DET_* is an exact 1/16-step fraction of the frame
 * — see the note in app_config.h. */
#define OSD_SCALE_X (CAM_WIDTH / DET_WIDTH)
#define OSD_SCALE_Y (CAM_HEIGHT / DET_HEIGHT)
static_assert(CAM_WIDTH % DET_WIDTH == 0, "OSD x scale must be exact");
static_assert(CAM_HEIGHT % DET_HEIGHT == 0, "OSD y scale must be exact");
static_assert(HAL_PPA_SCALE_IS_EXACT(CAM_WIDTH, DET_WIDTH), "PPA cannot express this x scale");
static_assert(HAL_PPA_SCALE_IS_EXACT(CAM_HEIGHT, DET_HEIGHT), "PPA cannot express this y scale");

typedef struct {
    hal_vcap_t cap;
    hal_h264_t enc;
    ppa_client_handle_t ppa;
    QueueHandle_t cap_queue;
    const detector_iface_t *det;

    uint32_t stat_cap;
    uint32_t stat_cap_drop;
    uint32_t stat_enc;
    uint32_t stat_send_ok;
    uint32_t stat_send_fail;
    uint32_t stat_bytes;

    /* Per-stage cost of the encode loop, summed over the reporting window.
     * The loop is fully serial, so these add up to the frame interval and show
     * directly which stage caps the frame rate. */
    int64_t us_ppa;
    int64_t us_osd;
    int64_t us_enc;
    int64_t us_send;
} video_ctx_t;

static video_ctx_t s_ctx;

/*
 * Drain the capture device as fast as it produces frames, handing descriptors —
 * never pixels — to the encode task. Keeping this separate means a slow encode
 * never backs up the driver's own ring.
 */
static void video_capture_task(void *arg) {
    ESP_LOGI(TAG, "video_capture_task started");

    while (true) {
        hal_vcap_frame_t frame;
        if (hal_vcap_acquire(&s_ctx.cap, &frame) != ESP_OK) {
            continue;
        }
        s_ctx.stat_cap++;

        if (xQueueSend(s_ctx.cap_queue, &frame, 0) != pdTRUE) {
            /* Encode is behind. Drop this frame, but hand the buffer straight
             * back — leaking it would starve the capture device within a few
             * frames and stop the stream entirely. */
            s_ctx.stat_cap_drop++;
            hal_vcap_release(&s_ctx.cap, &frame);
        }
    }
}

/* Feed one frame to the detector, if it is that frame's turn and the detector has
 * a free buffer to take it. See DET_FEED_EVERY_N for why this is rate-limited. */
static void feed_detector(uint8_t *yuv420) {
    static uint32_t frame_no;
    if (frame_no++ % DET_FEED_EVERY_N != 0) {
        return;
    }

    det_input_t in;
    if (!s_ctx.det->acquire_input(&in)) {
        return; /* detector still busy; skip detection, keep encoding */
    }

    if (hal_ppa_yuv420_to_rgb565(s_ctx.ppa, yuv420, CAM_WIDTH, CAM_HEIGHT,
                                 in.buffer, DET_INPUT_BYTES,
                                 DET_WIDTH, DET_HEIGHT) != ESP_OK) {
        s_ctx.det->discard(&in);
        return;
    }
    s_ctx.det->submit(&in);
}

/* Draw the latest boxes, then publish only the rows the OSD actually wrote. */
static void overlay_boxes(const hal_vcap_frame_t *frame) {
    det_box_t boxes[DET_MAX_BOX];
    int n = s_ctx.det->get_boxes(boxes, DET_MAX_BOX);

    osd_dirty_t dirty;
    osd_draw_boxes_yuv420(frame->data, CAM_WIDTH, CAM_HEIGHT, boxes, n,
                          OSD_SCALE_X, OSD_SCALE_Y, &dirty);

    for (int i = 0; i < dirty.count; i++) {
        hal_vcap_publish_rows(&s_ctx.cap, frame, dirty.range[i].first, dirty.range[i].last);
    }
}

/*
 * Share each captured frame with the detector, burn in the latest boxes,
 * H.264-encode it and push it to the peer.
 */
static void video_encode_task(void *arg) {
    ESP_LOGI(TAG, "video_encode_task started");

    while (true) {
        hal_vcap_frame_t frame;
        if (xQueueReceive(s_ctx.cap_queue, &frame, portMAX_DELAY) != pdTRUE) {
            continue;
        }

        if (webrtc_take_keyframe_request()) {
            ESP_LOGI(TAG, "keyframe requested");
            hal_h264_force_idr(&s_ctx.enc);
        }

        int64_t t0 = esp_timer_get_time();
        feed_detector(frame.data);
        int64_t t1 = esp_timer_get_time();
        overlay_boxes(&frame);
        int64_t t2 = esp_timer_get_time();

        hal_h264_bitstream_t bits;
        if (hal_h264_encode(&s_ctx.enc, frame.data, frame.bytesused, &bits) == ESP_OK) {
            int64_t t3 = esp_timer_get_time();
            s_ctx.stat_enc++;
            s_ctx.stat_bytes += bits.bytesused;

            int sent = webrtc_send_video(bits.data, bits.bytesused, WEBRTC_VIDEO_LOCK_MS);
            if (sent > 0) {
                s_ctx.stat_send_ok++;
            } else if (sent < 0) {
                s_ctx.stat_send_fail++;
            }
            hal_h264_release(&s_ctx.enc, &bits);

            s_ctx.us_enc += t3 - t2;
            s_ctx.us_send += esp_timer_get_time() - t3;
        }
        s_ctx.us_ppa += t1 - t0;
        s_ctx.us_osd += t2 - t1;

        hal_vcap_release(&s_ctx.cap, &frame);
    }
}

static void log_stats(int64_t *last_us) {
    int64_t now_us = esp_timer_get_time();
    uint32_t elapsed_ms = (uint32_t)((now_us - *last_us) / 1000);
    if (elapsed_ms == 0) {
        return;
    }

    uint32_t n = s_ctx.stat_enc ? s_ctx.stat_enc : 1;
    ESP_LOGI(TAG, "%ums: cap=%u cap_drop=%u enc=%u send_ok=%u send_fail=%u bitrate=%ukbps q_cap=%u",
             elapsed_ms, s_ctx.stat_cap, s_ctx.stat_cap_drop, s_ctx.stat_enc,
             s_ctx.stat_send_ok, s_ctx.stat_send_fail,
             (unsigned)((s_ctx.stat_bytes * 8ULL) / elapsed_ms),
             (unsigned)uxQueueMessagesWaiting(s_ctx.cap_queue));
    ESP_LOGI(TAG, "  per frame: ppa=%.1fms osd=%.1fms enc=%.1fms send=%.1fms total=%.1fms",
             s_ctx.us_ppa / (1000.0 * n), s_ctx.us_osd / (1000.0 * n),
             s_ctx.us_enc / (1000.0 * n), s_ctx.us_send / (1000.0 * n),
             (s_ctx.us_ppa + s_ctx.us_osd + s_ctx.us_enc + s_ctx.us_send) / (1000.0 * n));

    s_ctx.stat_cap = 0;
    s_ctx.stat_cap_drop = 0;
    s_ctx.stat_enc = 0;
    s_ctx.stat_send_ok = 0;
    s_ctx.stat_send_fail = 0;
    s_ctx.stat_bytes = 0;
    s_ctx.us_ppa = 0;
    s_ctx.us_osd = 0;
    s_ctx.us_enc = 0;
    s_ctx.us_send = 0;
    *last_us = now_us;
}

void task_video(void *arg) {
    ESP_LOGI(TAG, "task_video started");

    memset(&s_ctx, 0, sizeof(s_ctx));
    s_ctx.det = &detector_pedestrian;

    ppa_client_config_t ppa_cfg = {};
    ppa_cfg.oper_type = PPA_OPERATION_SRM;
    ESP_ERROR_CHECK(ppa_register_client(&ppa_cfg, &s_ctx.ppa));

    ESP_ERROR_CHECK(hal_video_init());
    ESP_ERROR_CHECK(hal_vcap_open(&s_ctx.cap, CAM_WIDTH, CAM_HEIGHT));

    hal_h264_cfg_t enc_cfg = {
        .width = CAM_WIDTH,
        .height = CAM_HEIGHT,
        .i_period = H264_I_PERIOD,
        .bitrate = H264_BITRATE,
        .min_qp = H264_MIN_QP,
        .max_qp = H264_MAX_QP,
    };
    ESP_ERROR_CHECK(hal_h264_open(&s_ctx.enc, &enc_cfg));

    ESP_ERROR_CHECK(hal_h264_start(&s_ctx.enc));
    ESP_ERROR_CHECK(hal_vcap_start(&s_ctx.cap));

    s_ctx.cap_queue = xQueueCreate(VIDEO_QUEUE_LEN, sizeof(hal_vcap_frame_t));
    assert(s_ctx.cap_queue);

    /* Start the detector before the producers, so the first frame already has a
     * buffer to go into. A failure here is not fatal: video keeps streaming. */
    if (s_ctx.det->start() != ESP_OK) {
        ESP_LOGW(TAG, "detector unavailable; streaming without detection");
    }

    xTaskCreatePinnedToCore(video_capture_task, "video_cap", TASK_CAPTURE_STACK_SIZE, NULL,
                            TASK_PRIO_VIDEO_CAP, NULL, TASK_CORE_VIDEO);
    xTaskCreatePinnedToCore(video_encode_task, "video_enc", TASK_ENCODE_STACK_SIZE, NULL,
                            TASK_PRIO_VIDEO_ENC, NULL, TASK_CORE_VIDEO);

    /* Setup is done; all this task does from here is print stats once a second,
     * so it must not outrank the tasks doing the work. */
    vTaskPrioritySet(NULL, TASK_PRIO_STATS);

    int64_t last_us = esp_timer_get_time();
    while (true) {
        vTaskDelay(pdMS_TO_TICKS(1000));
        log_stats(&last_us);
    }
}
