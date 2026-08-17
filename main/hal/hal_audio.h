#pragma once

#include "esp_err.h"
#include "esp_codec_dev.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t hal_audio_init(void);
esp_codec_dev_handle_t hal_audio_get_handle(void);

#ifdef __cplusplus
}
#endif
