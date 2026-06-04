#include "app_ethernet.h"

#include "esp_check.h"
#include "esp_log.h"

#define APP_ETH_MDC_GPIO 31
#define APP_ETH_MDIO_GPIO 52
#define APP_ETH_PHY_RST_GPIO 51
#define APP_ETH_PHY_ADDR 1

static const char *TAG = "app_eth_init";

esp_err_t app_eth_init(esp_eth_handle_t *eth_handle_out)
{
    ESP_RETURN_ON_FALSE(eth_handle_out != NULL, ESP_ERR_INVALID_ARG, TAG, "Ethernet handle cannot be NULL");

    esp_err_t ret = ESP_OK;
    eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
    eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
    phy_config.phy_addr = APP_ETH_PHY_ADDR;
    phy_config.reset_gpio_num = APP_ETH_PHY_RST_GPIO;

    eth_esp32_emac_config_t emac_config = ETH_ESP32_EMAC_DEFAULT_CONFIG();
    emac_config.smi_gpio.mdc_num = APP_ETH_MDC_GPIO;
    emac_config.smi_gpio.mdio_num = APP_ETH_MDIO_GPIO;

    esp_eth_mac_t *mac = esp_eth_mac_new_esp32(&emac_config, &mac_config);
    esp_eth_phy_t *phy = esp_eth_phy_new_ip101(&phy_config);
    esp_eth_handle_t eth_handle = NULL;
    esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);

    ESP_GOTO_ON_FALSE(mac != NULL && phy != NULL, ESP_ERR_NO_MEM, err, TAG, "Ethernet MAC or PHY create failed");
    ESP_GOTO_ON_ERROR(esp_eth_driver_install(&eth_config, &eth_handle), err, TAG, "Ethernet driver install failed");

    *eth_handle_out = eth_handle;
    return ESP_OK;

err:
    if (eth_handle != NULL) {
        esp_eth_driver_uninstall(eth_handle);
    }
    if (mac != NULL) {
        mac->del(mac);
    }
    if (phy != NULL) {
        phy->del(phy);
    }
    return ret == ESP_OK ? ESP_FAIL : ret;
}
