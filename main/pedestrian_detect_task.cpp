#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_heap_caps.h"

#include "pedestrian_detect_task.h"
#include "pedestrian_detect.hpp"
#include "dl_image_define.hpp"
#include "app_camera_pipeline.hpp"

static const char *TAG = "ped_detect";

static SemaphoreHandle_t s_box_mutex = NULL;
static ped_box_t s_boxes[PED_DETECT_MAX_BOX];
static int s_box_count = 0;
static PedestrianDetect detect;
static pipeline_handle_t s_feed_pipeline = NULL;

static void store_results(const std::list<dl::detect::result_t> &results)
{
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

static void detect_task(void *arg)
{
    // log entry to confirm task is running
    ESP_LOGI(TAG, "detect task started");

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

        store_results(detect.run(img));

        int64_t t1 = esp_timer_get_time();

        ESP_LOGD(TAG, "Inference time: %.2f ms", (t1 - t0) / 1000.0);

        camera_pipeline_queue_element_index(s_feed_pipeline, cpre->index);
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}

esp_err_t pedestrian_detect_task_start(void *arg)
{
    s_feed_pipeline = *((pipeline_handle_t *)arg);

    s_box_mutex = xSemaphoreCreateMutex();
    if (!s_box_mutex) {
        ESP_LOGE(TAG, "mutex alloc failed");
        return ESP_FAIL;
    }

    if (xTaskCreatePinnedToCore(detect_task, "detect", 8192, NULL, 7, NULL, 1) != pdPASS) {
        ESP_LOGE(TAG, "detect task create failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

int pedestrian_detect_get_boxes(ped_box_t *out, int max)
{
    if (!s_box_mutex) {
        return 0;
    }
    xSemaphoreTake(s_box_mutex, portMAX_DELAY);
    int n = s_box_count < max ? s_box_count : max;
    memcpy(out, s_boxes, n * sizeof(ped_box_t));
    xSemaphoreGive(s_box_mutex);
    return n;
}
