#pragma once

// QEMU plugin-debug networking: OpenCores open_eth + DHCP.
// Production Wi-Fi is not modeled; plugins that check WiFi.status() use
// m4QemuNetWifiCompatConnected() so TCP still rides on the ETH netif.

#if defined(M4_QEMU_PLUGIN_DEBUG) && M4_QEMU_PLUGIN_DEBUG

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Start open_eth, wait up to timeout_ms for a DHCP lease. Returns true if up.
bool m4QemuNetStart(uint32_t timeout_ms);

// ETH has a non-zero IPv4 address.
bool m4QemuNetIsUp(void);

// Compatibility: treat QEMU ETH as "Wi-Fi connected" for plugin host checks.
bool m4QemuNetWifiCompatConnected(void);

// Best-effort IPv4 string (empty if down).
void m4QemuNetLocalIp(char* out, size_t outLen);

#ifdef __cplusplus
}
#endif

#else  // !M4_QEMU_PLUGIN_DEBUG

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

static inline bool m4QemuNetStart(uint32_t) { return false; }
static inline bool m4QemuNetIsUp(void) { return false; }
static inline bool m4QemuNetWifiCompatConnected(void) { return false; }
static inline void m4QemuNetLocalIp(char* out, size_t outLen) {
  if (out && outLen) out[0] = '\0';
}

#endif
