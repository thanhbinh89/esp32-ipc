#include "sdkconfig.h"
#include "esp_err.h"
#include "pedestrian_detect_task.h"

#if CONFIG_APP_ENABLE_AI

#include <string.h>
#include <new>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "pedestrian_detect.hpp"
#include "dl_image_define.hpp"
#include "app_camera_pipeline.hpp"
#include "osd.h"
#include "video_task.h"

/* OSD box colour in YUV (red). */
#define OSD_Y           76
#define OSD_U           84
#define OSD_V           255
#define OSD_THICKNESS   4

/* Detection coords are in PED_DETECT_* space; scale up to full frame for OSD. */
#define OSD_SCALE_X     (CAM_WIDTH / PED_DETECT_WIDTH)
#define OSD_SCALE_Y     (CAM_HEIGHT / PED_DETECT_HEIGHT)

static const char *TAG = "ped_detect";

static SemaphoreHandle_t s_box_mutex = NULL;
static ped_box_t s_boxes[PED_DETECT_MAX_BOX];
static int s_box_count = 0;
/* Constructed in pedestrian_detect_task_start(), not statically: the esp-dl model
 * claims internal RAM on construction, and a file-scope instance would do that from
 * a global constructor — before app_main and before other components' constructors
 * (esp_hosted allocates its DMA mempool from one), leaving them to fail at boot. */
static PedestrianDetect *s_detect = NULL;
static pipeline_handle_t s_feed_pipeline = NULL;

static void store_results(const std::list<dl::detect::result_t> &results) {
    int n = 0;
    xSemaphoreTake(s_box_mutex, portMAX_DELAY);
    for (const auto &res : results) {
        if (n >= PED_DETECT_MAX_BOX) {
            break;
        }
        if (res.box.size() < 4) {
            continue;
        }

        s_boxes[n].x1 = res.box[0];
        s_boxes[n].y1 = res.box[1];
        s_boxes[n].x2 = res.box[2];
        s_boxes[n].y2 = res.box[3];
        s_boxes[n].score = res.score;
        n++;
    }
    s_box_count = n;
    xSemaphoreGive(s_box_mutex);

    ESP_LOGI(TAG, "pedestrians: %d", n);
    for (int i = 0; i < n; i++) {
        ESP_LOGI(TAG, "[%d] (%d,%d)-(%d,%d) score=%.2f", i,
                 s_boxes[i].x1, s_boxes[i].y1, s_boxes[i].x2, s_boxes[i].y2, s_boxes[i].score);
    }
}

static void detect_task(void *arg) {
    // log entry to confirm task is running
    ESP_LOGI(TAG, "pedestrian detection task started");

    while (true) {
        camera_pipeline_buffer_element *cpre = camera_pipeline_recv_element(s_feed_pipeline, portMAX_DELAY);
        if (!cpre) {
            continue;
        }

        dl::image::img_t img;
        img.data = cpre->buffer;
        img.width = PED_DETECT_WIDTH;
        img.height = PED_DETECT_HEIGHT;
        img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565;

        int64_t t0 = esp_timer_get_time();

        store_results(s_detect->run(img));

        int64_t t1 = esp_timer_get_time();

        ESP_LOGD(TAG, "Inference time: %.2f ms", (t1 - t0) / 1000.0);

        camera_pipeline_queue_element_index(s_feed_pipeline, cpre->index);
        
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

void pedestrian_detect_task_start(void *arg) {
    s_feed_pipeline = *((pipeline_handle_t *)arg);

    s_box_mutex = xSemaphoreCreateMutex();
    if (!s_box_mutex) {
        ESP_LOGE(TAG, "Mutex alloc failed");
        return;
    }

    s_detect = new (std::nothrow) PedestrianDetect();
    if (!s_detect) {
        ESP_LOGE(TAG, "Detector alloc failed");
        return;
    }

    if (xTaskCreatePinnedToCore(detect_task, "detect", 8192, NULL, 7, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "Detect task create failed");
        return;
    }
}

void pedestrian_detect_overlay_last_boxes(uint8_t *yuv420) {
    ped_box_t boxes[PED_DETECT_MAX_BOX];
    xSemaphoreTake(s_box_mutex, portMAX_DELAY);
    int n = s_box_count < PED_DETECT_MAX_BOX ? s_box_count : PED_DETECT_MAX_BOX;
    memcpy(boxes, s_boxes, n * sizeof(ped_box_t));
    xSemaphoreGive(s_box_mutex);

    for (int i = 0; i < n; i++) {
        osd_draw_rect_yuv420(yuv420, CAM_WIDTH, CAM_HEIGHT,
                             boxes[i].x1 * OSD_SCALE_X, boxes[i].y1 * OSD_SCALE_Y,
                             boxes[i].x2 * OSD_SCALE_X, boxes[i].y2 * OSD_SCALE_Y,
                             OSD_Y, OSD_U, OSD_V, OSD_THICKNESS);
    }
}

#else  /* !CONFIG_APP_ENABLE_AI: stubs, detector model never instantiated */

void pedestrian_detect_task_start(void *arg) {
    (void)arg;
}

#endif /* CONFIG_APP_ENABLE_AI */
