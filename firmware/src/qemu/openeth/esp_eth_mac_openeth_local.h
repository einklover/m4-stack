#pragma once
#include "esp_eth_com.h"
#include "esp_eth_mac.h"
#ifdef __cplusplus
extern "C" {
#endif
esp_eth_mac_t *esp_eth_mac_new_openeth(const eth_mac_config_t *config);
#ifdef __cplusplus
}
#endif
