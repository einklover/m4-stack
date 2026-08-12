#pragma once

// QEMU plugin-debug networking: OpenCores open_eth + DHCP.
// Production Wi-Fi radio is not modeled. Callers that check WiFi.status() must
// use the compat helpers below so TCP still rides on the ETH netif and the UI
// believes it is "already on Wi-Fi".

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG

#ifdef __cplusplus
extern "C" {
#endif

// Start open_eth, wait up to timeout_ms for a DHCP lease. Returns true if up.
bool m4QemuNetStart(uint32_t timeout_ms);

// ETH has a non-zero IPv4 address.
bool m4QemuNetIsUp(void);

// Compatibility: treat QEMU ETH as "Wi-Fi connected" for host/plugin checks.
bool m4QemuNetWifiCompatConnected(void);

// Fake STA identity shown to UI / m4adb (never a real radio SSID).
const char* m4QemuNetSsid(void);

// Best-effort IPv4 string (empty if down).
void m4QemuNetLocalIp(char* out, size_t outLen);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
#include <Arduino.h>
#include <WiFi.h>
#include <string>

// C++ helpers — prefer these over raw WiFi.* in QEMU-aware paths.
namespace M4QemuNet {

inline bool staConnected() {
  return m4QemuNetWifiCompatConnected() ||
         (WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0));
}

inline String localIpString() {
  if (m4QemuNetWifiCompatConnected()) {
    char buf[16] = {};
    m4QemuNetLocalIp(buf, sizeof(buf));
    if (buf[0]) return String(buf);
  }
  if (WiFi.status() == WL_CONNECTED) return WiFi.localIP().toString();
  return String();
}

inline String ssidString() {
  if (m4QemuNetWifiCompatConnected()) return String(m4QemuNetSsid());
  if (WiFi.status() == WL_CONNECTED) return WiFi.SSID();
  return String();
}

inline int rssiOrZero() {
  if (m4QemuNetWifiCompatConnected()) return 0;
  if (WiFi.status() == WL_CONNECTED) return WiFi.RSSI();
  return -127;
}

inline std::string localIpStd() {
  const String s = localIpString();
  return std::string(s.c_str());
}

inline std::string ssidStd() {
  const String s = ssidString();
  return std::string(s.c_str());
}

}  // namespace M4QemuNet
#endif

#else  // !M4_QEMU_PLUGIN_DEBUG

static inline bool m4QemuNetStart(uint32_t) { return false; }
static inline bool m4QemuNetIsUp(void) { return false; }
static inline bool m4QemuNetWifiCompatConnected(void) { return false; }
static inline const char* m4QemuNetSsid(void) { return ""; }
static inline void m4QemuNetLocalIp(char* out, size_t outLen) {
  if (out && outLen) out[0] = '\0';
}

#ifdef __cplusplus
#include <Arduino.h>
#include <WiFi.h>
#include <string>
namespace M4QemuNet {
inline bool staConnected() {
  return WiFi.status() == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0);
}
inline String localIpString() {
  return staConnected() ? WiFi.localIP().toString() : String();
}
inline String ssidString() { return staConnected() ? WiFi.SSID() : String(); }
inline int rssiOrZero() { return staConnected() ? WiFi.RSSI() : -127; }
inline std::string localIpStd() {
  const String s = localIpString();
  return std::string(s.c_str());
}
inline std::string ssidStd() {
  const String s = ssidString();
  return std::string(s.c_str());
}
}  // namespace M4QemuNet
#endif

#endif
