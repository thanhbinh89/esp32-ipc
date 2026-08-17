#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "app_config.h"
#include "hal_audio.h"
#include "hal_netif.h"
#include "hal_video_init.h"
#include "task_audio.h"
#include "task_video.h"
#include "webrtc_api.h"

static const char *TAG = "main";

/*
 * Boot: bring up the network, the camera and the audio codec, then hand the work
 * to the tasks. Everything past this point is driven by them.
 */
extern "C" void app_main(void) {
    ESP_ERROR_CHECK(hal_netif_start_and_wait_ip());

    /* Video first: esp_video_init() creates the shared I2C bus that the audio
     * codec then joins via i2c_master_get_bus_handle(). */
    ESP_LOGI(TAG, "Initializing video...");
    ESP_ERROR_CHECK(hal_video_init());

    ESP_LOGI(TAG, "Initializing audio...");
    if (hal_audio_init() == ESP_OK) {
        xTaskCreatePinnedToCore(task_audio, "audio", TASK_AUDIO_STACK_SIZE, NULL,
                                TASK_PRIO_AUDIO, NULL, TASK_CORE_AUDIO);
    } else {
        ESP_LOGE(TAG, "audio codec init failed; running without audio");
    }

    ESP_LOGI(TAG, "Starting video task...");
    xTaskCreatePinnedToCore(task_video, "camera", TASK_VIDEO_STACK_SIZE, NULL,
                            TASK_PRIO_VIDEO_INIT, NULL, TASK_CORE_VIDEO);

    ESP_LOGI(TAG, "Starting WebRTC task...");
    xTaskCreatePinnedToCore(task_webrtc, "webrtc", TASK_WEBRTC_STACK_SIZE, NULL,
                            TASK_PRIO_WEBRTC, NULL, TASK_CORE_NET);

    /* Nothing left to do here. Returning ends the main task and frees its stack;
     * the tasks above keep the system running. */
}
