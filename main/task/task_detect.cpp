#include "sdkconfig.h"
#include "esp_err.h"

#include "app_config.h"
#include "detector.h"

#if CONFIG_APP_ENABLE_AI

#include <string.h>
#include <new>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_timer.h"

#include "pedestrian_detect.hpp"
#include "dl_image_define.hpp"
#include "frame_pool.hpp"

static const char *TAG = "ped_detect";

static SemaphoreHandle_t s_box_mutex = NULL;
static det_box_t s_boxes[DET_MAX_BOX];
static int s_box_count = 0;

/* Constructed in start(), not statically: the esp-dl model claims internal RAM on
 * construction, and a file-scope instance would do that from a global constructor
 * — before app_main and before other components' constructors (esp_hosted
 * allocates its DMA mempool from one), leaving them to fail at boot. */
static PedestrianDetect *s_detect = NULL;
static pipeline_handle_t s_feed = NULL;

static void store_results(const std::list<dl::detect::result_t> &results) {
    int n = 0;
    xSemaphoreTake(s_box_mutex, portMAX_DELAY);
    for (const auto &res : results) {
        if (n >= DET_MAX_BOX) {
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

    ESP_LOGD(TAG, "pedestrians: %d", n);
    for (int i = 0; i < n; i++) {
        ESP_LOGD(TAG, "[%d] (%d,%d)-(%d,%d) score=%.2f", i,
                 s_boxes[i].x1, s_boxes[i].y1, s_boxes[i].x2, s_boxes[i].y2, s_boxes[i].score);
    }
}

static void detect_task(void *arg) {
    ESP_LOGI(TAG, "pedestrian detection task started");

    int64_t acc_us = 0;
    int acc_n = 0;

    while (true) {
        frame_pool_element_t *elem = frame_pool_recv(s_feed, portMAX_DELAY);
        if (!elem) {
            continue;
        }

        dl::image::img_t img;
        img.data = elem->buffer;
        img.width = DET_WIDTH;
        img.height = DET_HEIGHT;
        img.pix_type = dl::image::DL_IMAGE_PIX_TYPE_RGB565;

        int64_t t0 = esp_timer_get_time();
        store_results(s_detect->run(img));
        int64_t t1 = esp_timer_get_time();

        /* Averaged over a window rather than logged per frame: this is the number
         * to watch when changing the feed resolution or the esp-dl preprocessor
         * caps, and a per-inference line would drown the rest of the log. */
        acc_us += t1 - t0;
        if (++acc_n == DET_TIMING_WINDOW) {
            ESP_LOGI(TAG, "inference avg %.1f ms over %d frames",
                     acc_us / (1000.0 * DET_TIMING_WINDOW), DET_TIMING_WINDOW);
            acc_us = 0;
            acc_n = 0;
        }

        frame_pool_release(s_feed, elem->index);

        /* Yield one tick even though frame_pool_recv() above can block. It only
         * blocks while the camera is ahead of us; once inference is slower than
         * the feed interval a buffer is always waiting, this task never leaves
         * the CPU, and IDLE1 stops feeding the task watchdog. At 1 ms a tick
         * against a ~100 ms inference the cost is noise. */
        vTaskDelay(1);
    }
}

static esp_err_t ped_start(void) {
    frame_pool_cfg_t cfg = {
        .elem_num = DET_FEED_ELEMENTS,
        .align_size = 128,
        .caps = MALLOC_CAP_SPIRAM,
        .buffer_size = DET_INPUT_BYTES,
    };
    if (frame_pool_new(&cfg, &s_feed) != ESP_OK) {
        ESP_LOGE(TAG, "Feed pool alloc failed");
        return ESP_ERR_NO_MEM;
    }

    s_box_mutex = xSemaphoreCreateMutex();
    if (!s_box_mutex) {
        ESP_LOGE(TAG, "Mutex alloc failed");
        return ESP_ERR_NO_MEM;
    }

    s_detect = new (std::nothrow) PedestrianDetect();
    if (!s_detect) {
        ESP_LOGE(TAG, "Detector alloc failed");
        return ESP_ERR_NO_MEM;
    }

    if (xTaskCreatePinnedToCore(detect_task, "detect", TASK_DETECT_STACK_SIZE, NULL,
                               TASK_PRIO_DETECT, NULL, TASK_CORE_DETECT) != pdPASS) {
        ESP_LOGE(TAG, "Detect task create failed");
        return ESP_FAIL;
    }
    return ESP_OK;
}

static bool ped_acquire_input(det_input_t *in) {
    frame_pool_element_t *elem = frame_pool_acquire(s_feed);
    if (!elem) {
        return false;
    }
    in->token = elem;
    in->buffer = elem->buffer;
    return true;
}

static void ped_submit(const det_input_t *in) {
    frame_pool_submit(s_feed, (frame_pool_element_t *)in->token);
}

static void ped_discard(const det_input_t *in) {
    frame_pool_release(s_feed, ((frame_pool_element_t *)in->token)->index);
}

static int ped_get_boxes(det_box_t *out, int max) {
    if (!s_box_mutex) {
        return 0;
    }
    xSemaphoreTake(s_box_mutex, portMAX_DELAY);
    int n = s_box_count < max ? s_box_count : max;
    memcpy(out, s_boxes, n * sizeof(det_box_t));
    xSemaphoreGive(s_box_mutex);
    return n;
}

const detector_iface_t detector_pedestrian = {
    .start = ped_start,
    .acquire_input = ped_acquire_input,
    .submit = ped_submit,
    .discard = ped_discard,
    .get_boxes = ped_get_boxes,
};

#else /* !CONFIG_APP_ENABLE_AI: a detector that never finds anything, so callers
       * need no conditional compilation of their own. */

static esp_err_t ped_start(void) { return ESP_ERR_NOT_SUPPORTED; }
static bool ped_acquire_input(det_input_t *in) { (void)in; return false; }
static void ped_submit(const det_input_t *in) { (void)in; }
static void ped_discard(const det_input_t *in) { (void)in; }
static int ped_get_boxes(det_box_t *out, int max) { (void)out; (void)max; return 0; }

const detector_iface_t detector_pedestrian = {
    .start = ped_start,
    .acquire_input = ped_acquire_input,
    .submit = ped_submit,
    .discard = ped_discard,
    .get_boxes = ped_get_boxes,
};

#endif /* CONFIG_APP_ENABLE_AI */
