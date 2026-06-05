#include <stdio.h>
#include <stdarg.h>
#include <string.h>
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/event_groups.h"
#include "esp_netif.h"
#include "esp_eth.h"
#include "esp_event.h"
#include "esp_log.h"
#include "app_ethernet.h"
#include "sdkconfig.h"

#include "app_video.h"
#include "video_task.h"
#include "pedestrian_detect_task.h"
#include "app_camera_pipeline.hpp"
#include "app_audio_codec.h"
#include "audio_task.h"
#include "webrtc_task.h"

static const char *TAG = "ipc";

#define GOT_IP_BIT BIT0
static EventGroupHandle_t s_net_event_group;
static pipeline_handle_t s_feed_pipeline = NULL;

/** Event handler for Ethernet events */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data)
{
    uint8_t mac_addr[6] = {0};
    /* we can get the ethernet driver handle from event data */
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet Link Up");
        ESP_LOGI(TAG, "Ethernet HW Addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet Link Down");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet Started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet Stopped");
        break;
    default:
        break;
    }
}

/** Event handler for IP_EVENT_ETH_GOT_IP */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data)
{
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Ethernet Got IP Address");
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    ESP_LOGI(TAG, "ETHIP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "ETHMASK:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "ETHGW:" IPSTR, IP2STR(&ip_info->gw));
    ESP_LOGI(TAG, "~~~~~~~~~~~");
    if (s_net_event_group) {
        xEventGroupSetBits(s_net_event_group, GOT_IP_BIT);
    }
}

extern "C" void app_main(void)
{
    s_net_event_group = xEventGroupCreate();

    esp_eth_handle_t eth_handle;
    ESP_ERROR_CHECK(app_eth_init(&eth_handle));

    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&cfg);
    esp_eth_netif_glue_handle_t eth_netif_glue = esp_eth_new_netif_glue(eth_handle);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, eth_netif_glue));

    // Register user defined event handlers
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));

    ESP_ERROR_CHECK(esp_eth_start(eth_handle));

    ESP_LOGI(TAG, "waiting for IP...");
    xEventGroupWaitBits(s_net_event_group, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    // video init
    ESP_ERROR_CHECK(app_video_init());
    // audio init
    if (app_audio_codec_init() == ESP_OK) {
        // audio task
        xTaskCreatePinnedToCore(audio_task, "audio", 4096, NULL, 4, NULL, 0);
    } else {
        ESP_LOGE(TAG, "audio codec init failed; running without audio");
    }
    // video pipeline init
    camera_pipeline_cfg_t feed_cfg = {
        .elem_num = 2,
        .elements = NULL,
        .align_size = 128,
        .caps = MALLOC_CAP_SPIRAM,
        .buffer_size = PED_DETECT_WIDTH * PED_DETECT_HEIGHT * 2,
    };
    if (camera_element_pipeline_new(&feed_cfg, &s_feed_pipeline) != ESP_OK) {
        ESP_LOGE(TAG, "feed pipeline alloc failed; running H.264 only");
    }
    // pedestrian detect init
    ESP_ERROR_CHECK(pedestrian_detect_task_start(&s_feed_pipeline));
    // video task
    xTaskCreatePinnedToCore(video_task, "camera", 4096, s_feed_pipeline, 5, NULL, 0);
    // webrtc task
    xTaskCreatePinnedToCore(webrtc_task, "webrtc", 4096, NULL, 6, NULL, 0);

    while (true)
    {
        // delay to avoid watchdog timer trigger
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

}
