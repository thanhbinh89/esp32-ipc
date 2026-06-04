#include "app_video.h"

#include "esp_check.h"
#include "esp_log.h"
#include "esp_video_init.h"
#define APP_MIPI_CSI_SCCB_I2C_PORT    GPIO_NUM_0
#define APP_MIPI_CSI_SCCB_I2C_SCL_PIN GPIO_NUM_8
#define APP_MIPI_CSI_SCCB_I2C_SDA_PIN GPIO_NUM_7
#define APP_MIPI_CSI_SCCB_I2C_FREQ    100000
#define APP_MIPI_CSI_SENSOR_RESET_PIN GPIO_NUM_NC
#define APP_MIPI_CSI_SENSOR_PWDN_PIN  GPIO_NUM_NC

static const char *TAG = "app_video";
static bool s_is_init;

static const esp_video_init_csi_config_t s_csi_config = {
    .sccb_config = {
        .init_sccb = true,
        .i2c_config = {
            .port = APP_MIPI_CSI_SCCB_I2C_PORT,
            .scl_pin = APP_MIPI_CSI_SCCB_I2C_SCL_PIN,
            .sda_pin = APP_MIPI_CSI_SCCB_I2C_SDA_PIN,
        },
        .freq = APP_MIPI_CSI_SCCB_I2C_FREQ,
    },
    .reset_pin = APP_MIPI_CSI_SENSOR_RESET_PIN,
    .pwdn_pin = APP_MIPI_CSI_SENSOR_PWDN_PIN,
};

static const esp_video_init_config_t s_video_config = {
    .csi = &s_csi_config,
};

esp_err_t app_video_init(void)
{
    if (s_is_init) {
        return ESP_OK;
    }

    ESP_LOGI(TAG, "MIPI-CSI camera sensor I2C port=%d, scl_pin=%d, sda_pin=%d, freq=%d",
             APP_MIPI_CSI_SCCB_I2C_PORT,
             APP_MIPI_CSI_SCCB_I2C_SCL_PIN,
             APP_MIPI_CSI_SCCB_I2C_SDA_PIN,
             APP_MIPI_CSI_SCCB_I2C_FREQ);

    ESP_RETURN_ON_ERROR(esp_video_init(&s_video_config), TAG, "failed to initialize video");
    s_is_init = true;

    return ESP_OK;
}
