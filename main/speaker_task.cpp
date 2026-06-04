#include "speaker_task.h"

#include <stdint.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "app_audio_codec.h"

static const char *TAG = "speaker";

extern const uint8_t canon_pcm_start[] asm("_binary_canon_pcm_start");
extern const uint8_t canon_pcm_end[] asm("_binary_canon_pcm_end");

void speaker_task(void *arg)
{
    esp_codec_dev_handle_t codec = app_audio_codec_get_handle();
    if (!codec) {
        ESP_LOGE(TAG, "codec not initialized");
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        uint8_t *data = (uint8_t *)canon_pcm_start;
        int remain = canon_pcm_end - canon_pcm_start;

        while (remain > 0) {
            int chunk = remain > 4096 ? 4096 : remain;
            int ret = esp_codec_dev_write(codec, data, chunk);
            if (ret != ESP_CODEC_DEV_OK) {
                ESP_LOGE(TAG, "write failed: %d", ret);
                vTaskDelay(pdMS_TO_TICKS(1000));
                break;
            }
            data += chunk;
            remain -= chunk;
        }

        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}
