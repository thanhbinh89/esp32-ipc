#include "webrtc_task.h"

#include <stdbool.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "esp_log.h"
#include "esp_cpu.h"
#include "sdkconfig.h"

#include "peer.h"

static const char *TAG = "webrtc";

PeerConnection *g_pc = nullptr;
PeerConnectionState eState = PEER_CONNECTION_CLOSED;
SemaphoreHandle_t g_pc_lock = nullptr;

static bool s_datachannel_open = false;
static volatile bool s_keyframe_requested = false;

static void on_ice_state_change(PeerConnectionState state, void *user_data)
{
    ESP_LOGI(TAG, "PeerConnectionState: %d", state);
    eState = state;
    if (state != PEER_CONNECTION_COMPLETED) {
        s_datachannel_open = false;
    }
}

static void on_dc_message(char *msg, size_t len, void *user_data, uint16_t sid)
{
    ESP_LOGD(TAG, "datachannel msg sid=%u len=%u", sid, (unsigned)len);
}

static void on_dc_open(void *user_data)
{
    ESP_LOGI(TAG, "datachannel open");
    s_datachannel_open = true;
}

static void on_dc_close(void *user_data)
{
    ESP_LOGI(TAG, "datachannel close");
    s_datachannel_open = false;
}

static void on_request_keyframe(void *user_data)
{
    ESP_LOGI(TAG, "request keyframe");
    s_keyframe_requested = true;
}

bool webrtc_take_keyframe_request(void)
{
    bool requested = s_keyframe_requested;
    s_keyframe_requested = false;
    return requested;
}

static void peer_connection_task(void *arg)
{
    while (true) {
        if (xSemaphoreTake(g_pc_lock, portMAX_DELAY) == pdTRUE) {
            peer_connection_loop(g_pc);
            xSemaphoreGive(g_pc_lock);
        }
        vTaskDelay(pdMS_TO_TICKS(1));
    }

    vTaskDelete(NULL);
}

void webrtc_task(void *arg)
{
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

    g_pc_lock = xSemaphoreCreateMutex();
    if (!g_pc_lock) {
        ESP_LOGE(TAG, "mutex alloc failed");
        goto failed1;
    }

    peer_init();
    g_pc = peer_connection_create(&cfg);
    if (!g_pc) {
        ESP_LOGE(TAG, "peer_connection_create failed");
        goto failed2;
    }

    peer_connection_oniceconnectionstatechange(g_pc, on_ice_state_change);
    peer_connection_ondatachannel(g_pc, on_dc_message, on_dc_open, on_dc_close);
    peer_signaling_connect(CONFIG_SIGNALING_URL, CONFIG_SIGNALING_TOKEN, g_pc);

    ESP_LOGI(TAG, "signaling URL: %s", CONFIG_SIGNALING_URL);

    if (xTaskCreatePinnedToCore(peer_connection_task, "peer", 8192, NULL, 5, NULL, 0) != pdPASS) {
        ESP_LOGE(TAG, "peer_connection task create failed");
        goto failed2;
    }

    while (true) {
        peer_signaling_loop();
        vTaskDelay(pdMS_TO_TICKS(10));
    }

failed2:
    peer_connection_destroy(g_pc);
    peer_deinit();
failed1:
    vTaskDelete(NULL);
}
