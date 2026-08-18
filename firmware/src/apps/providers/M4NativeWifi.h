#pragma once

#include <cstdint>
#include <functional>
#include <string>

namespace M4NativeWifi {

struct Result {
  bool ok = false;
  bool alreadyConnected = false;
  std::string error;
};

using CancelFn = std::function<bool()>;

// True when the TCP stack is already usable (STA associated, or QEMU ETH).
// Does not start the radio. Call this before any getaddrinfo/TLS: on ESP32,
// lwIP's tcpip mutex is NULL until Wi-Fi/ETH has been brought up, and
// esp_http_client_perform() then asserts in xQueueSemaphoreTake.
bool isReady();

// Connect using the system Wi-Fi credential store. Passwords never cross the
// provider/UI boundary and are never logged. Connection is intentionally kept
// alive for the provider/read session; the provider worker owns no separate
// credential cache.
Result ensureConnected(uint32_t timeoutMs = 20000, const CancelFn& cancelled = {});

}  // namespace M4NativeWifi
