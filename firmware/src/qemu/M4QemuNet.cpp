#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG

#include "qemu/M4QemuNet.h"

#include <Arduino.h>
#include <cstdio>
#include <cstring>

#include "esp_err.h"
#include "esp_eth.h"
#include "esp_eth_mac.h"
#include "esp_eth_phy.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_netif.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"

// Force open_eth API visible (prebuilt Arduino sdkconfig leaves it unset).
#ifndef CONFIG_ETH_USE_OPENETH
#define CONFIG_ETH_USE_OPENETH 1
#endif
#include "esp_eth_mac_openeth.h"

static const char* TAG = "M4QemuNet";
static constexpr int kGotIpBit = BIT0;

static EventGroupHandle_t s_events = nullptr;
static esp_eth_handle_t s_eth = nullptr;
static esp_netif_t* s_netif = nullptr;
static bool s_up = false;
static char s_ip[16] = {};

extern "C" esp_eth_phy_t* esp_eth_phy_new_dp83848(const eth_phy_config_t* config);

static void onGotIp(void*, esp_event_base_t, int32_t, void* event_data) {
  auto* e = static_cast<ip_event_got_ip_t*>(event_data);
  std::snprintf(s_ip, sizeof(s_ip), IPSTR, IP2STR(&e->ip_info.ip));
  s_up = true;
  ESP_LOGI(TAG, "got IP %s", s_ip);
  if (s_events) xEventGroupSetBits(s_events, kGotIpBit);
}

bool m4QemuNetStart(uint32_t timeout_ms) {
  if (s_up) return true;

  esp_err_t err = esp_netif_init();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "esp_netif_init failed: %s", esp_err_to_name(err));
    return false;
  }
  err = esp_event_loop_create_default();
  if (err != ESP_OK && err != ESP_ERR_INVALID_STATE) {
    ESP_LOGE(TAG, "event loop failed: %s", esp_err_to_name(err));
    return false;
  }

  if (!s_events) s_events = xEventGroupCreate();
  if (s_events) xEventGroupClearBits(s_events, kGotIpBit);

  esp_netif_config_t cfg = ESP_NETIF_DEFAULT_ETH();
  s_netif = esp_netif_new(&cfg);
  if (!s_netif) {
    ESP_LOGE(TAG, "esp_netif_new failed");
    return false;
  }

  eth_mac_config_t mac_config = ETH_MAC_DEFAULT_CONFIG();
  eth_phy_config_t phy_config = ETH_PHY_DEFAULT_CONFIG();
  phy_config.autonego_timeout_ms = 100;
  phy_config.reset_gpio_num = -1;

  esp_eth_mac_t* mac = esp_eth_mac_new_openeth(&mac_config);
  esp_eth_phy_t* phy = esp_eth_phy_new_dp83848(&phy_config);
  if (!mac || !phy) {
    ESP_LOGE(TAG, "openeth mac/phy create failed mac=%p phy=%p", mac, phy);
    return false;
  }

  esp_eth_config_t eth_config = ETH_DEFAULT_CONFIG(mac, phy);
  err = esp_eth_driver_install(&eth_config, &s_eth);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "eth driver install failed: %s", esp_err_to_name(err));
    return false;
  }

  esp_eth_netif_glue_handle_t glue = esp_eth_new_netif_glue(s_eth);
  esp_netif_attach(s_netif, glue);
  esp_event_handler_register(IP_EVENT, IP_EVENT_ETH_GOT_IP, &onGotIp, nullptr);

  err = esp_eth_start(s_eth);
  if (err != ESP_OK) {
    ESP_LOGE(TAG, "eth start failed: %s", esp_err_to_name(err));
    return false;
  }

  Serial.printf("[%lu] [M4-QEMU-NET] open_eth started, waiting DHCP…\n", millis());
  if (s_events) {
    const EventBits_t bits =
        xEventGroupWaitBits(s_events, kGotIpBit, pdFALSE, pdTRUE, pdMS_TO_TICKS(timeout_ms));
    if (!(bits & kGotIpBit)) {
      Serial.printf("[%lu] [M4-QEMU-NET] DHCP timeout after %ums\n", millis(),
                    static_cast<unsigned>(timeout_ms));
      return false;
    }
  }
  Serial.printf("[%lu] [M4-QEMU-NET] up ip=%s (WiFi-compat connected=1)\n", millis(), s_ip);
  return true;
}

bool m4QemuNetIsUp(void) { return s_up; }

bool m4QemuNetWifiCompatConnected(void) { return s_up; }

void m4QemuNetLocalIp(char* out, size_t outLen) {
  if (!out || !outLen) return;
  if (!s_up) {
    out[0] = '\0';
    return;
  }
  std::snprintf(out, outLen, "%s", s_ip);
}

#endif  // M4_QEMU_PLUGIN_DEBUG
