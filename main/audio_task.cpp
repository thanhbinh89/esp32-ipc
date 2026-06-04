#include "audio_task.h"

#include <stdint.h>
#include <stdlib.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_codec_dev.h"
#include "app_audio_codec.h"
#include "webrtc_task.h"

/* 20 ms @ 8 kHz mono, int16 -> 160 samples -> 160 PCMA bytes per packet. */
#define AUDIO_READ_BYTES 320

static const char *TAG = "audio";

static uint8_t linear16_to_g711a(int16_t sample)
{
    const uint16_t seg_end[8] = {0x1F, 0x3F, 0x7F, 0xFF, 0x1FF, 0x3FF, 0x7FF, 0xFFF};
    uint8_t mask;
    uint8_t aval;
    uint16_t pcm;
    int seg;

    pcm = sample < 0 ? (uint16_t)(-sample - 1) : (uint16_t)sample;
    mask = sample >= 0 ? 0xD5 : 0x55;
    pcm >>= 3;

    for (seg = 0; seg < 8; seg++) {
        if (pcm <= seg_end[seg]) {
            break;
        }
    }

    if (seg >= 8) {
        return 0x7F ^ mask;
    }

    aval = (uint8_t)(seg << 4);
    if (seg < 2) {
        aval |= (pcm >> 1) & 0x0F;
    } else {
        aval |= (pcm >> seg) & 0x0F;
    }
    return aval ^ mask;
}

void audio_task(void *arg)
{
    esp_codec_dev_handle_t codec = app_audio_codec_get_handle();
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
            g711a[i] = linear16_to_g711a(pcm[i]);
        }

        if (g_pc && eState == PEER_CONNECTION_COMPLETED) {
            if (xSemaphoreTake(g_pc_lock, portMAX_DELAY) == pdTRUE) {
                ESP_LOGD(TAG, "send audio frame: %d bytes", frames);
                peer_connection_send_audio(g_pc, g711a, frames);
                xSemaphoreGive(g_pc_lock);
            }
        }
    }
}
