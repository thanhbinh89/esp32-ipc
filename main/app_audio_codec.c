#include "app_audio_codec.h"

#include "driver/gpio.h"
#include "driver/i2c_master.h"
#include "driver/i2s_std.h"
#include "esp_check.h"
#include "esp_log.h"
#include "esp_codec_dev_defaults.h"

#define AUDIO_I2C_NUM         I2C_NUM_0
#define AUDIO_I2S_NUM         I2S_NUM_0
#define AUDIO_I2C_SCL_IO      GPIO_NUM_8
#define AUDIO_I2C_SDA_IO      GPIO_NUM_7
#define AUDIO_I2S_MCK_IO      GPIO_NUM_13
#define AUDIO_I2S_BCK_IO      GPIO_NUM_12
#define AUDIO_I2S_WS_IO       GPIO_NUM_10
#define AUDIO_I2S_DO_IO       GPIO_NUM_9
#define AUDIO_I2S_DI_IO       GPIO_NUM_11
#define AUDIO_PA_IO           GPIO_NUM_53
#define AUDIO_SAMPLE_RATE     8000
#define AUDIO_MCLK_MULTIPLE   I2S_MCLK_MULTIPLE_384
#define AUDIO_VOLUME          70
#define AUDIO_MIC_GAIN_DB     30.0f

static const char *TAG = "audio_codec";

static esp_codec_dev_handle_t s_codec;
static const audio_codec_data_if_t *s_i2s_data_if;
static i2s_chan_handle_t s_tx_handle;
static i2s_chan_handle_t s_rx_handle;

static esp_err_t audio_i2s_init(void)
{
    if (s_i2s_data_if) {
        return ESP_OK;
    }

    i2s_chan_config_t chan_cfg = I2S_CHANNEL_DEFAULT_CONFIG(AUDIO_I2S_NUM, I2S_ROLE_MASTER);
    chan_cfg.auto_clear = true;
    ESP_RETURN_ON_ERROR(i2s_new_channel(&chan_cfg, &s_tx_handle, &s_rx_handle), TAG, "new i2s channel failed");

    i2s_std_config_t std_cfg = {
        .clk_cfg = I2S_STD_CLK_DEFAULT_CONFIG(AUDIO_SAMPLE_RATE),
        .slot_cfg = I2S_STD_PHILIPS_SLOT_DEFAULT_CONFIG(I2S_DATA_BIT_WIDTH_16BIT, I2S_SLOT_MODE_MONO),
        .gpio_cfg = {
            .mclk = AUDIO_I2S_MCK_IO,
            .bclk = AUDIO_I2S_BCK_IO,
            .ws = AUDIO_I2S_WS_IO,
            .dout = AUDIO_I2S_DO_IO,
            .din = AUDIO_I2S_DI_IO,
            .invert_flags = {
                .mclk_inv = false,
                .bclk_inv = false,
                .ws_inv = false,
            },
        },
    };
    std_cfg.clk_cfg.mclk_multiple = AUDIO_MCLK_MULTIPLE;

    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_tx_handle, &std_cfg), TAG, "init i2s tx failed");
    ESP_RETURN_ON_ERROR(i2s_channel_init_std_mode(s_rx_handle, &std_cfg), TAG, "init i2s rx failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_tx_handle), TAG, "enable i2s tx failed");
    ESP_RETURN_ON_ERROR(i2s_channel_enable(s_rx_handle), TAG, "enable i2s rx failed");

    audio_codec_i2s_cfg_t i2s_cfg = {
        .port = AUDIO_I2S_NUM,
        .rx_handle = s_rx_handle,
        .tx_handle = s_tx_handle,
    };
    s_i2s_data_if = audio_codec_new_i2s_data(&i2s_cfg);
    ESP_RETURN_ON_FALSE(s_i2s_data_if, ESP_FAIL, TAG, "new i2s data interface failed");

    return ESP_OK;
}

esp_err_t app_audio_codec_init(void)
{
    if (s_codec) {
        return ESP_OK;
    }

    i2c_master_bus_handle_t i2c_handle = NULL;
    ESP_RETURN_ON_ERROR(i2c_master_get_bus_handle(AUDIO_I2C_NUM, &i2c_handle), TAG, "get shared i2c bus failed");
    ESP_RETURN_ON_ERROR(audio_i2s_init(), TAG, "audio i2s init failed");

    const audio_codec_gpio_if_t *gpio_if = audio_codec_new_gpio();
    ESP_RETURN_ON_FALSE(gpio_if, ESP_FAIL, TAG, "new gpio interface failed");

    audio_codec_i2c_cfg_t i2c_cfg = {
        .port = AUDIO_I2C_NUM,
        .addr = ES8311_CODEC_DEFAULT_ADDR,
        .bus_handle = i2c_handle,
    };
    const audio_codec_ctrl_if_t *i2c_ctrl_if = audio_codec_new_i2c_ctrl(&i2c_cfg);
    ESP_RETURN_ON_FALSE(i2c_ctrl_if, ESP_FAIL, TAG, "new i2c ctrl interface failed");

    esp_codec_dev_hw_gain_t gain = {
        .pa_voltage = 5.0,
        .codec_dac_voltage = 3.3,
    };
    es8311_codec_cfg_t es8311_cfg = {
        .ctrl_if = i2c_ctrl_if,
        .gpio_if = gpio_if,
        .codec_mode = ESP_CODEC_DEV_WORK_MODE_BOTH,
        .pa_pin = AUDIO_PA_IO,
        .pa_reverted = false,
        .master_mode = false,
        .use_mclk = true,
        .digital_mic = false,
        .invert_mclk = false,
        .invert_sclk = false,
        .hw_gain = gain,
        .no_dac_ref = false,
        .mclk_div = AUDIO_MCLK_MULTIPLE,
    };
    const audio_codec_if_t *es8311_dev = es8311_codec_new(&es8311_cfg);
    ESP_RETURN_ON_FALSE(es8311_dev, ESP_FAIL, TAG, "new es8311 codec failed");

    esp_codec_dev_cfg_t codec_dev_cfg = {
        .dev_type = ESP_CODEC_DEV_TYPE_IN_OUT,
        .codec_if = es8311_dev,
        .data_if = s_i2s_data_if,
    };
    s_codec = esp_codec_dev_new(&codec_dev_cfg);
    ESP_RETURN_ON_FALSE(s_codec, ESP_FAIL, TAG, "new codec dev failed");

    esp_codec_dev_sample_info_t sample_info = {
        .bits_per_sample = 16,
        .channel = 1,
        .channel_mask = 0,
        .sample_rate = AUDIO_SAMPLE_RATE,
        .mclk_multiple = AUDIO_MCLK_MULTIPLE,
    };
    ESP_RETURN_ON_FALSE(esp_codec_dev_open(s_codec, &sample_info) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "open codec failed");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_out_vol(s_codec, AUDIO_VOLUME) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "set volume failed");
    ESP_RETURN_ON_FALSE(esp_codec_dev_set_in_gain(s_codec, AUDIO_MIC_GAIN_DB) == ESP_CODEC_DEV_OK, ESP_FAIL, TAG, "set mic gain failed");

    ESP_LOGI(TAG, "ES8311 ready: i2c=%d scl=%d sda=%d i2s=%d sample_rate=%d", AUDIO_I2C_NUM,
             AUDIO_I2C_SCL_IO, AUDIO_I2C_SDA_IO, AUDIO_I2S_NUM, AUDIO_SAMPLE_RATE);
    return ESP_OK;
}

esp_codec_dev_handle_t app_audio_codec_get_handle(void)
{
    return s_codec;
}
