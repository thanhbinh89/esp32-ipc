#pragma once

#include "esp_err.h"
#include "esp_codec_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_audio_codec_init(void);
esp_codec_dev_handle_t app_audio_codec_get_handle(void);

#ifdef __cplusplus
}
#endif
