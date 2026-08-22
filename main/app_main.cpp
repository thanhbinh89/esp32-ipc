#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_heap_caps.h"
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
 * Internal RAM is the scarce resource on this board: PSRAM is 32 MB but only
 * ~422 KB of internal SRAM exists, and lwIP, esp_hosted and every task stack must
 * come from it. libpeer's 25 KB PeerConnection was already failing to fit, so
 * report what each boot step costs rather than guessing at the budget.
 */
static void log_internal_ram(const char *stage) {
    ESP_LOGI(TAG, "internal RAM after %-14s: free %6u, largest block %6u",
             stage,
             (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
             (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL));
}

/*
 * Boot: bring up the network, the camera and the audio codec, then hand the work
 * to the tasks. Everything past this point is driven by them.
 */
extern "C" void app_main(void) {
    log_internal_ram("boot");

    ESP_ERROR_CHECK(hal_netif_start_and_wait_ip());
    log_internal_ram("netif");

    /* Video first: esp_video_init() creates the shared I2C bus that the audio
     * codec then joins via i2c_master_get_bus_handle(). */
    ESP_LOGI(TAG, "Initializing video...");
    ESP_ERROR_CHECK(hal_video_init());
    log_internal_ram("video init");

    ESP_LOGI(TAG, "Initializing audio...");
    if (hal_audio_init() == ESP_OK) {
        xTaskCreatePinnedToCore(task_audio, "audio", TASK_AUDIO_STACK_SIZE, NULL,
                                TASK_PRIO_AUDIO, NULL, TASK_CORE_AUDIO);
    } else {
        ESP_LOGE(TAG, "audio codec init failed; running without audio");
    }
    log_internal_ram("audio");

    ESP_LOGI(TAG, "Starting video task...");
    xTaskCreatePinnedToCore(task_video, "camera", TASK_VIDEO_STACK_SIZE, NULL,
                            TASK_PRIO_VIDEO_INIT, NULL, TASK_CORE_VIDEO);

    ESP_LOGI(TAG, "Starting WebRTC task...");
    xTaskCreatePinnedToCore(task_webrtc, "webrtc", TASK_WEBRTC_STACK_SIZE, NULL,
                            TASK_PRIO_WEBRTC, NULL, TASK_CORE_WEBRTC);

    /* Nothing left to do here. Returning ends the main task and frees its stack;
     * the tasks above keep the system running. */
}
