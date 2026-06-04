#pragma once

#include "esp_eth_driver.h"

#ifdef __cplusplus
extern "C" {
#endif

esp_err_t app_eth_init(esp_eth_handle_t *eth_handle_out);

#ifdef __cplusplus
}
#endif
