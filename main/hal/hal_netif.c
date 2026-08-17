#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "esp_check.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "sdkconfig.h"

#if CONFIG_APP_NETIF_ETH
#include "esp_eth.h"
#include "esp_eth_driver.h"
#else
#include "esp_wifi.h"
#endif

#include "hal_netif.h"

#define GOT_IP_BIT BIT0

static const char *TAG = "hal_netif";

static EventGroupHandle_t s_event_group;

static void on_got_ip(void *arg, esp_event_base_t base, int32_t id, void *data) {
    const esp_netif_ip_info_t *ip = &((ip_event_got_ip_t *)data)->ip_info;

    ESP_LOGI(TAG, "IP:" IPSTR " Mask:" IPSTR " GW:" IPSTR,
             IP2STR(&ip->ip), IP2STR(&ip->netmask), IP2STR(&ip->gw));
    xEventGroupSetBits(s_event_group, GOT_IP_BIT);
}

#if CONFIG_APP_NETIF_ETH

#define ETH_PHY_ADDR     CONFIG_ESP_ETH_PHY_ADDR
#define ETH_PHY_RST_GPIO CONFIG_ESP_ETH_PHY_RST_GPIO
#define ETH_MDC_GPIO     CONFIG_ESP_ETH_MDC_GPIO
#define ETH_MDIO_GPIO    CONFIG_ESP_ETH_MDIO_GPIO

static void on_eth_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    uint8_t mac[6] = {0};
    esp_eth_handle_t handle = *(esp_eth_handle_t *)data;

    switch (id) {
    case ETHERNET_EVENT_CONNECTED:
        esp_eth_ioctl(handle, ETH_CMD_G_MAC_ADDR, mac);
        ESP_LOGI(TAG, "link up, HW addr %02x:%02x:%02x:%02x:%02x:%02x",
                 mac[0], mac[1], mac[2], mac[3], mac[4], mac[5]);
        break;
    case ETHERNET_EVENT_DISCONNECTED:
        ESP_LOGI(TAG, "link down");
        break;
    default:
        break;
    }
}

static esp_err_t netif_start(void) {
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = ETH_PHY_ADDR;
    phy_config.reset_gpio_num = ETH_PHY_RST_GPIO;

    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = ETH_MDIO_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
    ESP_RETURN_ON_FALSE(mac && phy, ESP_ERR_NO_MEM, TAG, "MAC or PHY create failed");

    esp_eth_handle_t eth_handle = NULL;
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
    ESP_RETURN_ON_ERROR(esp_eth_driver_install(&eth_config, &eth_handle), TAG, "driver install failed");

    esp_netif_config_t netif_cfg = ESP_NETIF_DEFAULT_ETH();
    esp_netif_t *netif = esp_netif_new(&netif_cfg);
    ESP_RETURN_ON_ERROR(esp_netif_attach(netif, esp_eth_new_netif_glue(eth_handle)), TAG, "attach failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(ETH_EVENT, ESP_EVENT_ANY_ID, on_eth_event, NULL),
                        TAG, "ETH_EVENT register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, on_got_ip, NULL),
                        TAG, "IP_EVENT register failed");

    ESP_LOGI(TAG, "starting ethernet");
    return esp_eth_start(eth_handle);
}

#else /* CONFIG_APP_NETIF_WIFI */

#define WIFI_MAX_RETRY 100

static int s_retry_num;

static void on_wifi_event(void *arg, esp_event_base_t base, int32_t id, void *data) {
    switch (id) {
    case WIFI_EVENT_STA_START:
        esp_wifi_connect();
        break;
    case WIFI_EVENT_STA_DISCONNECTED:
        if (s_retry_num < WIFI_MAX_RETRY) {
            s_retry_num++;
            ESP_LOGI(TAG, "retry %d/%d", s_retry_num, WIFI_MAX_RETRY);
            esp_wifi_connect();
        } else {
            ESP_LOGE(TAG, "giving up connecting to the AP");
        }
        break;
    default:
        break;
    }
}

static esp_err_t netif_start(void) {
    esp_netif_create_default_wifi_sta();

    wifi_init_config_t init_cfg = WIFI_INIT_CONFIG_DEFAULT();
    ESP_RETURN_ON_ERROR(esp_wifi_init(&init_cfg), TAG, "wifi init failed");

    ESP_RETURN_ON_ERROR(esp_event_handler_register(WIFI_EVENT, ESP_EVENT_ANY_ID, on_wifi_event, NULL),
                        TAG, "WIFI_EVENT register failed");
    ESP_RETURN_ON_ERROR(esp_event_handler_register(IP_EVENT, IP_EVENT_STA_GOT_IP, on_got_ip, NULL),
                        TAG, "IP_EVENT register failed");

    wifi_config_t wifi_config = {
        .sta = {
            .ssid = CONFIG_ESP_WIFI_SSID,
            .password = CONFIG_ESP_WIFI_PASSWORD,
            .threshold = { .authmode = WIFI_AUTH_WPA2_WPA3_PSK },
        },
    };
    ESP_RETURN_ON_ERROR(esp_wifi_set_mode(WIFI_MODE_STA), TAG, "set mode failed");
    ESP_RETURN_ON_ERROR(esp_wifi_set_config(WIFI_IF_STA, &wifi_config), TAG, "set config failed");

    ESP_LOGI(TAG, "starting wifi");
    return esp_wifi_start();
}

#endif /* CONFIG_APP_NETIF_ETH */

esp_err_t hal_netif_start_and_wait_ip(void) {
    s_event_group = xEventGroupCreate();
    ESP_RETURN_ON_FALSE(s_event_group, ESP_ERR_NO_MEM, TAG, "event group alloc failed");

    ESP_RETURN_ON_ERROR(esp_netif_init(), TAG, "netif init failed");
    ESP_RETURN_ON_ERROR(esp_event_loop_create_default(), TAG, "event loop create failed");
    ESP_RETURN_ON_ERROR(netif_start(), TAG, "netif start failed");

    ESP_LOGI(TAG, "waiting for IP...");
    xEventGroupWaitBits(s_event_group, GOT_IP_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
    return ESP_OK;
}
