#include "task_speaker.h"

#include <math.h>
#include "freertos/FreeRTOS.h"
#include "freertos/stream_buffer.h"
#include "freertos/task.h"
#include "esp_codec_dev.h"
#include "esp_log.h"
#include "sdkconfig.h"

#include "app_config.h"
#include "g711.h"
#include "hal_audio.h"

static const char *TAG = "speaker";

/*
 * Jitter buffer between the peer task and this one.
 *
 * A StreamBuffer rather than a queue of packets, because nothing downstream
 * needs the RTP packet boundaries: A-law carries no inter-sample state, so any
 * split of the byte stream decodes identically. That also makes this
 * indifferent to the remote's ptime -- Chrome sends 20 ms of PCMA, a peer that
 * sends 10 or 60 needs no special case here.
 *
 * Exactly one producer (the peer task) and one consumer (this task), which is
 * the only arrangement a StreamBuffer is safe for.
 */
static StreamBufferHandle_t s_jitter;
static volatile uint32_t s_dropped;

bool speaker_submit_g711a(const uint8_t *payload, size_t len) {
    if (!s_jitter || len == 0) {
        return false;
    }

    /* Zero timeout on purpose. This runs on the peer task, which is also driving
     * ICE, DTLS and the outgoing RTP drain -- blocking here to wait for the
     * speaker would stall the uplink in order to protect the downlink. */
    size_t sent = xStreamBufferSend(s_jitter, payload, len, 0);
    if (sent != len) {
        s_dropped += len - sent;
        return false;
    }
    return true;
}

#if CONFIG_APP_AUDIO_WELCOME_TONE
/*
 * Power-on chime, synthesised rather than embedded.
 *
 * At 8 kHz mono the alternative was a PCM asset in flash -- ~8 KB for every half
 * second -- for a sound that is three sine tones. Phase is accumulated and
 * wrapped instead of being recomputed as freq * n, so the argument to sinf()
 * stays small however long a note runs, and each note is faded in and out so the
 * DAC never sees a step edge (which the PA reproduces as a click).
 */
static void play_welcome_tone(esp_codec_dev_handle_t codec) {
    static const struct {
        float hz;
        int ms;
    } notes[] = {{523.25f, 120}, {659.25f, 120}, {783.99f, 260}};

    const float fade = (float)(AUDIO_TONE_FADE_MS * AUDIO_SAMPLE_RATE / 1000);
    int16_t chunk[AUDIO_SAMPLES_PER_PACKET];

    for (size_t n = 0; n < sizeof(notes) / sizeof(notes[0]); n++) {
        const int total = notes[n].ms * AUDIO_SAMPLE_RATE / 1000;
        const float step = 2.0f * (float)M_PI * notes[n].hz / AUDIO_SAMPLE_RATE;
        float phase = 0.0f;

        for (int done = 0; done < total;) {
            int count = total - done;
            if (count > AUDIO_SAMPLES_PER_PACKET) {
                count = AUDIO_SAMPLES_PER_PACKET;
            }

            for (int i = 0; i < count; i++) {
                float env = fminf((float)(done + i) / fade,
                                  (float)(total - done - i) / fade);
                if (env > 1.0f) {
                    env = 1.0f;
                }
                chunk[i] = (int16_t)(AUDIO_TONE_AMPLITUDE * env * sinf(phase));

                phase += step;
                if (phase >= 2.0f * (float)M_PI) {
                    phase -= 2.0f * (float)M_PI;
                }
            }

            if (esp_codec_dev_write(codec, chunk, count * (int)sizeof(int16_t)) != ESP_CODEC_DEV_OK) {
                ESP_LOGW(TAG, "welcome tone write failed");
                return;
            }
            done += count;
        }
    }
}
#endif

void task_speaker(void *arg) {
    esp_codec_dev_handle_t codec = hal_audio_get_handle();
    if (!codec) {
        ESP_LOGE(TAG, "codec not initialized");
        vTaskDelete(NULL);
        return;
    }

    /* Created before anything else in the task: task_webrtc is started after
     * this one and a peer takes seconds to negotiate, but speaker_submit_g711a()
     * still checks for NULL rather than relying on that ordering. */
    s_jitter = xStreamBufferCreate(AUDIO_SPK_JITTER_BYTES, AUDIO_SAMPLES_PER_PACKET);
    if (!s_jitter) {
        ESP_LOGE(TAG, "jitter buffer alloc failed");
        vTaskDelete(NULL);
        return;
    }

#if CONFIG_APP_AUDIO_WELCOME_TONE
    play_welcome_tone(codec);
#endif
    ESP_LOGI(TAG, "downlink ready: %d ms jitter buffer", AUDIO_SPK_JITTER_MS);

    uint8_t alaw[AUDIO_SAMPLES_PER_PACKET];
    int16_t pcm[AUDIO_SAMPLES_PER_PACKET];
    uint32_t reported = 0;
    TickType_t next_report = 0;

    while (true) {
        /* Blocks indefinitely when nobody is talking. There is nothing useful to
         * do on a timeout: the TX channel is configured auto_clear, so an idle
         * DMA buffer is already silence and writing zeros would only burn cycles
         * on core 0 for every second of quiet. */
        size_t got = xStreamBufferReceive(s_jitter, alaw, sizeof(alaw), portMAX_DELAY);
        if (got == 0) {
            continue;
        }

        for (size_t i = 0; i < got; i++) {
            pcm[i] = g711a_decode(alaw[i]);
        }

        int ret = esp_codec_dev_write(codec, pcm, (int)(got * sizeof(int16_t)));
        if (ret != ESP_CODEC_DEV_OK) {
            ESP_LOGW(TAG, "write failed: %d", ret);
        }

        /* Overflow means the peer is delivering faster than 8 kHz real time, or
         * this task is not being scheduled. Reported from here rather than from
         * the submit path, which must stay off the peer task's critical path. */
        uint32_t dropped = s_dropped;
        if (dropped != reported && xTaskGetTickCount() >= next_report) {
            ESP_LOGW(TAG, "jitter buffer overflow: %u bytes dropped",
                     (unsigned)(dropped - reported));
            reported = dropped;
            next_report = xTaskGetTickCount() + pdMS_TO_TICKS(1000);
        }
    }
}
