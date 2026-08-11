#include "apps/providers/M4NativeWifi.h"

#include "WifiCredentialStore.h"

#include <Arduino.h>
#include <WiFi.h>

#include <algorithm>

namespace M4NativeWifi {

Result ensureConnected(uint32_t timeoutMs, const CancelFn& cancelled) {
  Result r;
  if (WiFi.status() == WL_CONNECTED) {
    r.ok = true;
    r.alreadyConnected = true;
    return r;
  }

  timeoutMs = std::max<uint32_t>(1000, std::min<uint32_t>(60000, timeoutMs));
  (void)WIFI_STORE.loadFromFile();
  const auto& creds = WIFI_STORE.getCredentials();
  if (creds.empty()) {
    r.error = "no_saved_wifi";
    return r;
  }

  WiFi.mode(WIFI_STA);
  const uint32_t started = millis();
  for (const auto& c : creds) {
    if (cancelled && cancelled()) {
      WiFi.disconnect(false, false);
      r.error = "cancelled";
      return r;
    }
    if (millis() - started >= timeoutMs) break;

    WiFi.disconnect(false, false);
    delay(20);
    WiFi.begin(c.ssid.c_str(), c.password.c_str());

    const uint32_t remaining = timeoutMs - std::min(timeoutMs, millis() - started);
    const uint32_t attemptMs = std::min<uint32_t>(8000, remaining);
    const uint32_t attemptStart = millis();
    while (millis() - attemptStart < attemptMs) {
      if (cancelled && cancelled()) {
        WiFi.disconnect(false, false);
        r.error = "cancelled";
        return r;
      }
      if (WiFi.status() == WL_CONNECTED) {
        r.ok = true;
        return r;
      }
      delay(100);
    }
  }

  if (cancelled && cancelled()) r.error = "cancelled";
  else if (millis() - started >= timeoutMs) r.error = "wifi_timeout";
  else r.error = "wifi_connect_failed";
  return r;
}

}  // namespace M4NativeWifi
