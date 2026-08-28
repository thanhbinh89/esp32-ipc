#include "task_audio.h"

#include <stdint.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_codec_dev.h"

#include "app_config.h"
#include "g711.h"
#include "hal_audio.h"
#include "webrtc_api.h"

static const char *TAG = "audio";

void task_audio(void *arg) {
    ESP_LOGD(TAG, "task_audio started");

    esp_codec_dev_handle_t codec = hal_audio_get_handle();
    if (!codec) {
        ESP_LOGE(TAG, "codec not initialized");
        vTaskDelete(NULL);
        return;
    }

    int16_t *pcm = (int16_t *)malloc(AUDIO_READ_BYTES);
    uint8_t *g711a = (uint8_t *)malloc(AUDIO_READ_BYTES / sizeof(int16_t));
    if (!pcm || !g711a) {
        ESP_LOGE(TAG, "audio buffer alloc failed");
        free(pcm);
        free(g711a);
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        int ret = esp_codec_dev_read(codec, pcm, AUDIO_READ_BYTES);
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGE(TAG, "read failed: %d", ret);
            vTaskDelay(pdMS_TO_TICKS(100));
            continue;
        }

        int frames = AUDIO_READ_BYTES / sizeof(int16_t);
        for (int i = 0; i < frames; i++) {
            g711a[i] = g711a_encode(pcm[i]);
        }

        /* Bounded wait, unlike video's best-effort drop: this task must come
         * back round within 20 ms or the I2S RX ring overruns. */
        webrtc_send_audio(g711a, frames);
    }
}
