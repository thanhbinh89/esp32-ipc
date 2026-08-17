#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_cpu.h"
#include "sdkconfig.h"

#include "peer.h"

#include "app_config.h"
#include "webrtc_api.h"

static const char *TAG = "webrtc";

/* Private to this file — reachable only through the webrtc_* functions below. */
static PeerConnection *s_pc = nullptr;
static PeerConnectionState s_state = PEER_CONNECTION_CLOSED;
static SemaphoreHandle_t s_pc_lock = nullptr;

static bool s_datachannel_open = false;
static volatile bool s_keyframe_requested = false;

static void on_ice_state_change(PeerConnectionState state, void *user_data) {
    ESP_LOGI(TAG, "PeerConnectionState: %d", state);
    s_state = state;
    if (state != PEER_CONNECTION_COMPLETED) {
        s_datachannel_open = false;
    }
}

static void on_dc_message(char *msg, size_t len, void *user_data, uint16_t sid) {
    ESP_LOGD(TAG, "datachannel msg sid=%u len=%u", sid, (unsigned)len);
}

static void on_dc_open(void *user_data) {
    ESP_LOGI(TAG, "datachannel open");
    s_datachannel_open = true;
}

static void on_dc_close(void *user_data) {
    ESP_LOGI(TAG, "datachannel close");
    s_datachannel_open = false;
}

static void on_request_keyframe(void *user_data) {
    ESP_LOGI(TAG, "request keyframe");
    s_keyframe_requested = true;
}

bool webrtc_take_keyframe_request(void) {
    bool requested = s_keyframe_requested;
    s_keyframe_requested = false;
    return requested;
}

bool webrtc_is_streaming(void) {
    return s_pc && s_state == PEER_CONNECTION_COMPLETED;
}

/* Shared body of the two senders: gate on state, take the lock, hand off. */
static int webrtc_send(int (*send)(PeerConnection *, const uint8_t *, size_t),
                       const uint8_t *buf, size_t len, uint32_t timeout_ms) {
    if (!webrtc_is_streaming() || len == 0) {
        return 0;
    }
    if (xSemaphoreTake(s_pc_lock, pdMS_TO_TICKS(timeout_ms)) != pdTRUE) {
        return -1;
    }

    int ret = send(s_pc, buf, len);
    xSemaphoreGive(s_pc_lock);

    return ret < 0 ? ret : (int)len;
}

int webrtc_send_video(const uint8_t *buf, size_t len, uint32_t timeout_ms) {
    return webrtc_send(peer_connection_send_video, buf, len, timeout_ms);
}

int webrtc_send_audio(const uint8_t *buf, size_t len, uint32_t timeout_ms) {
    return webrtc_send(peer_connection_send_audio, buf, len, timeout_ms);
}

static void peer_connection_task(void *arg) {
    while (true) {
        if (xSemaphoreTake(s_pc_lock, portMAX_DELAY) == pdTRUE) {
            peer_connection_loop(s_pc);
            xSemaphoreGive(s_pc_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }
}

void task_webrtc(void *arg) {
    ESP_LOGI(TAG, "task_webrtc started");

    PeerConfiguration cfg = {};
    cfg.ice_servers[0].urls = CONFIG_STUN_URL;
    if (CONFIG_TURN) {
        cfg.ice_servers[1].urls = CONFIG_TURN_URL;
        cfg.ice_servers[1].username = CONFIG_TURN_USERNAME;
        cfg.ice_servers[1].credential = CONFIG_TURN_CREDENTIAL;
    }

    cfg.audio_codec = CODEC_PCMA;
    cfg.video_codec = CODEC_H264;
    cfg.datachannel = DATA_CHANNEL_BINARY;
    cfg.on_request_keyframe = on_request_keyframe;

    s_pc_lock = xSemaphoreCreateMutex();
    if (!s_pc_lock) {
        ESP_LOGE(TAG, "mutex alloc failed");
        vTaskDelete(NULL);
        return;
    }

    peer_init();
    s_pc = peer_connection_create(&cfg);
    if (!s_pc) {
        ESP_LOGE(TAG, "peer_connection_create failed");
        peer_deinit();
        vTaskDelete(NULL);
        return;
    }

    peer_connection_oniceconnectionstatechange(s_pc, on_ice_state_change);
    peer_connection_ondatachannel(s_pc, on_dc_message, on_dc_open, on_dc_close);
    peer_signaling_connect(CONFIG_SIGNALING_URL, CONFIG_SIGNALING_TOKEN, s_pc);

    ESP_LOGI(TAG, "signaling URL: %s", CONFIG_SIGNALING_URL);

    if (xTaskCreatePinnedToCore(peer_connection_task, "peer", TASK_PEER_STACK_SIZE, NULL,
                                TASK_PRIO_PEER, NULL, TASK_CORE_NET) != pdPASS) {
        ESP_LOGE(TAG, "peer_connection task create failed");
        peer_connection_destroy(s_pc);
        peer_deinit();
        vTaskDelete(NULL);
        return;
    }

    while (true) {
        peer_signaling_loop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }
}
