#pragma once

#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Bring up whichever network interface the build selected (Ethernet or Wi-Fi via
 * the esp_wifi_remote co-processor) and block until DHCP hands us an address.
 *
 * The choice is compile-time: CONFIG_APP_NETIF_ETH / CONFIG_APP_NETIF_WIFI.
 * Callers just need "the network is usable now", so both paths are hidden here.
 */
esp_err_t hal_netif_start_and_wait_ip(void);

#ifdef __cplusplus
}
#endif
