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
#include "esp_wifi.h"
#include "app_ethernet.h"
#include "sdkconfig.h"

#include "app_video.h"
#include "video_task.h"
#include "pedestrian_detect_task.h"
#include "app_camera_pipeline.hpp"
#include "app_audio.h"
#include "task_audio.h"
#include "task_webrtc.h"

static const char *TAG = "main";

#define TASK_AUDIO_STACK_SIZE 4096
#define TASK_VIDEO_STACK_SIZE 4096
#define TASK_WEBRTC_STACK_SIZE 4096


#define GOT_IP_BIT BIT0

static EventGroupHandle_t s_net_event_group;
static pipeline_handle_t s_feed_pipeline = NULL;
#if CONFIG_APP_NETIF_WIFI
static int s_net_retry_num = 0;
#endif

#if CONFIG_APP_NETIF_ETH
/** Event handler for Ethernet events */
static void eth_event_handler(void *arg, esp_event_base_t event_base,
                              int32_t event_id, void *event_data) {
    uint8_t mac_addr[6] = {0};
    /* we can get the ethernet driver handle from event data */
    esp_eth_handle_t eth_handle = *(esp_eth_handle_t *)event_data;

    switch (event_id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(eth_handle, ETH_CMD_G_MAC_ADDR, mac_addr);
        ESP_LOGI(TAG, "Ethernet link up");
        ESP_LOGI(TAG, "Ethernet HW addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac_addr[0], mac_addr[1], mac_addr[2], mac_addr[3], mac_addr[4], mac_addr[5]);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "Ethernet link down");
        break;
    case ETHERNET_EVENT_START:
        ESP_LOGI(TAG, "Ethernet started");
        break;
    case ETHERNET_EVENT_STOP:
        ESP_LOGI(TAG, "Ethernet stopped");
        break;
    default:
        break;
    }
}
#endif /* CONFIG_APP_NETIF_ETH */

#if CONFIG_APP_NETIF_WIFI
static void wifi_event_handler(void* arg, esp_event_base_t event_base,
                                int32_t event_id, void* event_data) {
    switch (event_id) {
    case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        if (s_net_retry_num < 100) {
            esp_wifi_connect();
            ESP_LOGI(TAG, "Retry to connect to the AP");
        }
        ESP_LOGI(TAG,"Connect to the AP fail");
        break;
    default:
        break;
    }
}
#endif /* CONFIG_APP_NETIF_WIFI */

/** Event handler for IP_EVENT_ETH_GOT_IP */
static void got_ip_event_handler(void *arg, esp_event_base_t event_base,
                                 int32_t event_id, void *event_data) {
    ip_event_got_ip_t *event = (ip_event_got_ip_t *) event_data;
    const esp_netif_ip_info_t *ip_info = &event->ip_info;

    ESP_LOGI(TAG, "Got IP address");
    ESP_LOGI(TAG, "IP:" IPSTR, IP2STR(&ip_info->ip));
    ESP_LOGI(TAG, "Mask:" IPSTR, IP2STR(&ip_info->netmask));
    ESP_LOGI(TAG, "GW:" IPSTR, IP2STR(&ip_info->gw));
    if (s_net_event_group) {
        xEventGroupSetBits(s_net_event_group, GOT_IP_BIT);
    }
#if CONFIG_APP_NETIF_WIFI
    s_net_retry_num = 0;
#endif /* CONFIG_APP_NETIF_WIFI */
}

/**
 * The entrypoint of the application. This function initializes the TCP/IP stack, 
 * creates the default event loop, and starts the Ethernet or Wi-Fi driver depending on the configuration. 
 * It also initializes video and audio tasks, as well as a WebRTC task for streaming.
 **/
extern "C" void app_main(void) {
    // create event group for network events
    s_net_event_group = xEventGroupCreate();

    // initialize TCP/IP stack and create default event loop
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    // initialize Ethernet or Wi-Fi depending on the configuration
#if CONFIG_APP_NETIF_ETH
    ESP_LOGI(TAG, "Initializing ethernet...");
    esp_eth_handle_t eth_handle;
    ESP_ERROR_CHECK(app_eth_init(&eth_handle));

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *eth_netif = esp_netif_new(&netif_cfg);
    esp_eth_netif_glue_handle_t eth_netif_glue = esp_eth_new_netif_glue(eth_handle);
    ESP_ERROR_CHECK(esp_netif_attach(eth_netif, eth_netif_glue));

    // register event handlers for Ethernet and IP events
    ESP_ERROR_CHECK(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, &eth_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &got_ip_event_handler, NULL));
    // start Ethernet driver
    ESP_ERROR_CHECK(esp_eth_start(eth_handle));
#elif CONFIG_APP_NETIF_WIFI
    ESP_LOGI(TAG, "Initializing wifi...");
    esp_netif_create_default_wifi_sta();
    wifi_init_config_t wifi_init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_wifi_init(&wifi_init_cfg));
    // register event handlers for Wi-Fi and IP events
    ESP_ERROR_CHECK(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, NULL));
    ESP_ERROR_CHECK(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, &got_ip_event_handler, NULL));
    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_ESP_WIFI_SSID,
            .password = CONFIG_ESP_WIFI_PASSWORD,
            /* Authmode threshold resets to WPA2 as default if password matches WPA2 standards (password len => 8).
             * If you want to connect the device to deprecated WEP/WPA networks, Please set the threshold value
             * to WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK and set the password with length and format matching to
             * WIFI_AUTH_WEP/WIFI_AUTH_WPA_PSK standards.
             */
            .threshold = {
                .authmode = WIFI_AUTH_WPA2_WPA3_PSK,
            },
        },
    };
    ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
    ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wifi_config));
    // start Wi-Fi driver
    ESP_ERROR_CHECK(esp_wifi_start());
#endif

    // wait for IP address
    ESP_LOGI(TAG, "Waiting for IP...");
    xEventGroupWaitBits(s_net_event_group, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);

    // video init
    ESP_LOGI(TAG, "Initializing video...");
    ESP_ERROR_CHECK(app_video_init());
    
    // audio init
    ESP_LOGI(TAG, "Initializing audio...");
    if (app_audio_init() == ESP_OK) {
        // audio task
        ESP_LOGI(TAG, "Starting audio task...");
        xTaskCreatePinnedToCore(task_audio, "audio", TASK_AUDIO_STACK_SIZE, NULL, 4, NULL, 0);
    } else {
        ESP_LOGE(TAG, "audio codec init failed; running without audio");
    }
    
    // video task
    ESP_LOGI(TAG, "Starting video tasks...");
    xTaskCreatePinnedToCore(video_task, "camera", TASK_VIDEO_STACK_SIZE, s_feed_pipeline, 5, NULL, 0);
    
    // webrtc task
    ESP_LOGI(TAG, "Starting WebRTC tasks...");
    xTaskCreatePinnedToCore(task_webrtc, "webrtc", TASK_WEBRTC_STACK_SIZE, NULL, 6, NULL, 0);

#if CONFIG_APP_ENABLE_AI
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
    ESP_LOGI(TAG, "Starting pedestrian detection task...");
    ESP_ERROR_CHECK(pedestrian_detect_task_start(&s_feed_pipeline));
#endif

    while (true) {
        // delay to avoid watchdog timer trigger
        vTaskDelay(pdMS_TO_TICKS(1000));
    }

}
